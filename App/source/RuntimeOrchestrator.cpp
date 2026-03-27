#include "RuntimeOrchestrator.h"
#include "Logger.h"
#include "MasterFrameParser.h"
#include "BaselineSubtraction.h"
#include "CMFProcessor.h"
#include "GridIIRProcessor.h"
#include "FeatureExtractor.h"
#include "StylusProcessor.h"
#include "TouchTracker.h"
#include "CoordinateFilter.h"
#include "TouchGestureStateMachine.h"
#include <SetupAPI.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")

namespace App {

// --- 设备路径 ---
const std::wstring DEVICE_PATH_INTERRUPT = L"\\\\.\\Global\\SPBTESTTOOL_MASTER";
const std::wstring DEVICE_PATH_MASTER = L"\\\\.\\Global\\SPBTESTTOOL_MASTER";
const std::wstring DEVICE_PATH_SLAVE = L"\\\\.\\Global\\SPBTESTTOOL_SLAVE";
const GUID VHF_DEVICE_INTERFACE_GUID = {0x59819b74, 0xf102, 0x469a, {0x90, 0x09, 0x3c, 0xaf, 0x35, 0xfd, 0x46, 0x86}};
const GUID PEN_MCU_DEVICE_INTERFACE_GUID = {0xdd0ebedb, 0xf1d6, 0x4cfa, {0xac, 0xca, 0x71, 0xe6, 0x6d, 0x31, 0x78, 0xca}};

const char* AfeCommandToString(AFE_Command cmd) {
    switch (cmd) {
    case AFE_Command::ClearStatus:
        return "ClearStatus";
    case AFE_Command::EnableFreqShift:
        return "EnableFreqShift";
    case AFE_Command::DisableFreqShift:
        return "DisableFreqShift";
    case AFE_Command::StartCalibration:
        return "StartCalibration";
    case AFE_Command::EnterIdle:
        return "EnterIdle";
    case AFE_Command::ForceExitIdle:
        return "ForceExitIdle";
    case AFE_Command::ForceToFreqPoint:
        return "ForceToFreqPoint";
    case AFE_Command::ForceToScanRate:
        return "ForceToScanRate";
    default:
        return "Unknown";
    }
}

inline void WriteU16Le(std::array<uint8_t, 32>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xFFu);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

// HID Descriptor (HidInjectorThp.sys): byte[0] per finger = bit0(TipSwitch) | bit1(Confidence)
// Present (Down/Move): TipSwitch=1, Confidence=1 → 0x03
// Released (Up):       TipSwitch=0, Confidence=0 → 0x00
inline uint8_t EncodeVhfContactState(const Engine::TouchContact& c) {
    // State machine is the sole authority on Up/Down/Move lifecycle.
    // Do NOT check c.state (tracker raw) — only c.reportEvent.
    if (c.reportEvent == Engine::TouchReportUp) {
        return 0x02; // TipSwitch=0, Confidence=1
    }
    return 0x03;     // TipSwitch=1, Confidence=1 (Down or Move)
}

// Generic grid-to-VHF coordinate conversion.
// gridValue: raw grid coordinate, gridMax: physical edge (60 or 40)
// logicalMax: HID logical range maximum (e.g. 16000 for Y, 25600 for X)
// invert: if true, flip the axis (logicalMax - vhf)
inline uint16_t ToVhf(float gridValue, float gridMax, float logicalMax, bool invert) {
    const float norm = std::clamp(gridValue / gridMax, 0.0f, 1.0f);
    const int vhf = std::clamp(static_cast<int>(std::lround(norm * logicalMax)), 0, static_cast<int>(logicalMax));
    return static_cast<uint16_t>(invert ? (static_cast<int>(logicalMax) - vhf) : vhf);
}

RuntimeOrchestrator::RuntimeOrchestrator() {
    LOG_INFO("App", "RuntimeOrchestrator::RuntimeOrchestrator", "Unconnected", "Initializing DeviceRuntime...");
    m_runtime = std::make_unique<DeviceRuntime>(DEVICE_PATH_MASTER, DEVICE_PATH_SLAVE, DEVICE_PATH_INTERRUPT);
    m_runtime->SetAutoMode(false);  // 默认 Manual，保持现有行为
    m_penCommandApi = std::make_unique<Himax::Pen::PenCommandApi>();
    m_dvrBuffer = std::make_unique<RingBuffer<Engine::HeatmapFrame, 480>>();

    // Initialise Engine Pipeline
    m_pipeline.AddProcessor(std::make_unique<Engine::MasterFrameParser>());
    m_pipeline.AddProcessor(std::make_unique<Engine::BaselineSubtraction>());
    m_pipeline.AddProcessor(std::make_unique<Engine::CMFProcessor>());
    m_pipeline.AddProcessor(std::make_unique<Engine::GridIIRProcessor>());
    m_pipeline.AddProcessor(std::make_unique<Engine::FeatureExtractor>());
    m_pipeline.AddProcessor(std::make_unique<Engine::StylusProcessor>());
    m_pipeline.AddProcessor(std::make_unique<Engine::TouchTracker>());
    m_pipeline.AddProcessor(std::make_unique<Engine::CoordinateFilter>());
    m_pipeline.AddProcessor(std::make_unique<Engine::TouchGestureStateMachine>());

    ResetAutoAfeFreqShiftSyncState();
    
    // Load config on startup
    LoadConfig();
}

RuntimeOrchestrator::~RuntimeOrchestrator() {
    Stop();
}

bool RuntimeOrchestrator::Start() {
    if (m_running.exchange(true)) return false;
    LOG_INFO("App", "RuntimeOrchestrator::Start", "Unknown", "Starting background threads...");
    ResetAutoAfeFreqShiftSyncState();
    InitializePenBridge();

    m_runtime->Start();

    m_processingThread = std::thread(&RuntimeOrchestrator::ProcessingThreadFunc, this);
    return true;
}

void RuntimeOrchestrator::Stop() {
    const bool wasRunning = m_running.exchange(false);
    if (wasRunning) {
        LOG_INFO("App", "RuntimeOrchestrator::Stop", "Unknown", "Stopping background threads...");
        m_runtime->Stop();
        if (m_processingThread.joinable()) m_processingThread.join();
        ShutdownPenBridge();
        ResetAutoAfeFreqShiftSyncState();
    }
    {
        std::lock_guard<std::mutex> lock(m_vhfMutex);
        CloseVhfDeviceLocked();
    }
}

void RuntimeOrchestrator::SetVhfReportingEnabled(bool enabled) {
    const bool previous = m_vhfReportingEnabled.exchange(enabled);
    if (previous == enabled) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_vhfMutex);
    if (!enabled) {
        CloseVhfDeviceLocked();
        LOG_INFO("App", "RuntimeOrchestrator::SetVhfReportingEnabled", "UI",
                 "VHF reporting disabled.");
        return;
    }

    if (EnsureVhfDeviceOpenLocked()) {
        LOG_INFO("App", "RuntimeOrchestrator::SetVhfReportingEnabled", "UI",
                 "VHF reporting enabled and injector opened.");
    } else {
        LOG_WARN("App", "RuntimeOrchestrator::SetVhfReportingEnabled", "UI",
                 "VHF reporting enabled, but injector device is not currently available.");
    }
}

bool RuntimeOrchestrator::IsVhfDeviceOpen() const {
    std::lock_guard<std::mutex> lock(m_vhfMutex);
    return m_vhfDeviceHandle != INVALID_HANDLE_VALUE;
}

bool RuntimeOrchestrator::GetLatestFrame(Engine::HeatmapFrame& outFrame) {
    std::lock_guard<std::mutex> lock(m_latestFrameMutex);
    outFrame = m_latestFrame;
    return true;
}

ChipResult<> RuntimeOrchestrator::SwitchAfeMode(AFE_Command cmd, uint8_t param) {
    if (!m_runtime) {
        return std::unexpected(ChipError::InternalError);
    }
    m_runtime->SubmitCommand(command{cmd, param}, CommandSource::External,
                             AfeCommandToString(cmd));
    LOG_INFO("App", "RuntimeOrchestrator::SwitchAfeMode", "UI",
             "Queued AFE command: {}({}), param={}",
             AfeCommandToString(cmd),
             static_cast<int>(cmd),
             static_cast<unsigned int>(param));
    return {};
}

ChipResult<> RuntimeOrchestrator::SafeDeinit() {
    if (!m_runtime) {
        return std::unexpected(ChipError::InternalError);
    }
    const bool wasAcquiring = m_isAcquiring.exchange(false);
    LOG_INFO("App", "RuntimeOrchestrator::SafeDeinit", "UI",
             "Stopping runtime for deinit (wasAcquiring={})...", wasAcquiring);
    m_runtime->Stop();
    ResetAutoAfeFreqShiftSyncState();
    return {};
}

void RuntimeOrchestrator::SetAutoMode(bool enabled) {
    if (m_runtime) m_runtime->SetAutoMode(enabled);
}

bool RuntimeOrchestrator::IsAutoMode() const {
    return m_runtime ? m_runtime->IsAutoMode() : false;
}

void RuntimeOrchestrator::OnFrameFromRuntime(const uint8_t* data, std::size_t len) {
    // FPS 统计
    static constexpr int FPS_WINDOW = 32;
    static std::array<std::chrono::steady_clock::time_point, FPS_WINDOW> fpsRing{};
    static int fpsHead = 0;
    static int fpsCount = 0;
    const auto now = std::chrono::steady_clock::now();
    fpsRing[fpsHead] = now;
    fpsHead = (fpsHead + 1) % FPS_WINDOW;
    if (fpsCount < FPS_WINDOW) ++fpsCount;
    if (fpsCount >= 2) {
        const int oldestIdx = (fpsHead + FPS_WINDOW - fpsCount) % FPS_WINDOW;
        const auto spanUs = std::chrono::duration_cast<std::chrono::microseconds>(
            now - fpsRing[oldestIdx]).count();
        if (spanUs > 0) {
            m_acquisitionFps.store(static_cast<int>(
                std::llround((fpsCount - 1) * 1'000'000.0 / spanUs)));
        }
    }
    // 帧数据送入处理管线
    Engine::HeatmapFrame frame;
    frame.rawData.assign(data, data + len);
    m_frameBuffer.Push(frame);
}

void RuntimeOrchestrator::ProcessingThreadFunc() {
    LOG_INFO("App", "RuntimeOrchestrator::ProcessingThreadFunc", "Unknown", "Processing Thread started.");
    while (m_running) {
        Engine::HeatmapFrame frame;
        // 阻塞等待采集线程 push 原始帧
        if (m_frameBuffer.WaitForData(frame, std::chrono::milliseconds(100))) {
            
            // Split pipeline:
            // 1) Common preprocess (master/baseline/filter/feature)
            // 2) Stylus branch (StylusProcessor) on a copy
            // 3) Touch branch (TouchTracker) on a copy with stylus state injected
            auto* masterParser = static_cast<Engine::IFrameProcessor*>(nullptr);
            auto* baseline = static_cast<Engine::IFrameProcessor*>(nullptr);
            auto* cmf = static_cast<Engine::IFrameProcessor*>(nullptr);
            auto* grid = static_cast<Engine::IFrameProcessor*>(nullptr);
            auto* feature = static_cast<Engine::IFrameProcessor*>(nullptr);
            auto* stylus = static_cast<Engine::IFrameProcessor*>(nullptr);
            auto* touchTracker = static_cast<Engine::IFrameProcessor*>(nullptr);
            auto* coordFilter = static_cast<Engine::IFrameProcessor*>(nullptr);

            for (const auto& p : m_pipeline.GetProcessors()) {
                if (!masterParser && dynamic_cast<Engine::MasterFrameParser*>(p.get())) { masterParser = p.get(); continue; }
                if (!baseline && dynamic_cast<Engine::BaselineSubtraction*>(p.get())) { baseline = p.get(); continue; }
                if (!cmf && dynamic_cast<Engine::CMFProcessor*>(p.get())) { cmf = p.get(); continue; }
                if (!grid && dynamic_cast<Engine::GridIIRProcessor*>(p.get())) { grid = p.get(); continue; }
                if (!feature && dynamic_cast<Engine::FeatureExtractor*>(p.get())) { feature = p.get(); continue; }
                if (!stylus && dynamic_cast<Engine::StylusProcessor*>(p.get())) { stylus = p.get(); continue; }
                if (!touchTracker && dynamic_cast<Engine::TouchTracker*>(p.get())) { touchTracker = p.get(); continue; }
                if (!coordFilter && dynamic_cast<Engine::CoordinateFilter*>(p.get())) { coordFilter = p.get(); continue; }
            }

            auto runProcessor = [](Engine::IFrameProcessor* processor, Engine::HeatmapFrame& inOutFrame) {
                if (processor == nullptr || !processor->IsEnabled()) {
                    return true;
                }
                return processor->Process(inOutFrame);
            };

            bool ok = true;
            const auto t_pipeline_start = std::chrono::steady_clock::now();

            ok = ok && runProcessor(masterParser, frame);
            ok = ok && runProcessor(baseline, frame);
            ok = ok && runProcessor(cmf, frame);
            ok = ok && runProcessor(grid, frame);
            ok = ok && runProcessor(feature, frame);

            if (ok) {
                Engine::HeatmapFrame stylusFrame = frame;
                ok = runProcessor(stylus, stylusFrame);
                if (ok) {
                    HandleAutoAfeFreqShiftSync(stylusFrame.stylus);
                    Engine::HeatmapFrame touchFrame = frame;
                    touchFrame.stylus = stylusFrame.stylus;
                    ok = runProcessor(touchTracker, touchFrame);
                    if (ok) {
                        ok = runProcessor(coordFilter, touchFrame);
                        if (ok) {
                            touchFrame.stylus = stylusFrame.stylus;
                            frame = std::move(touchFrame);
                        }
                    }
                }
            }

            // --- Pipeline latency measurement ---
            const auto t_pipeline_end = std::chrono::steady_clock::now();
            const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                t_pipeline_end - t_pipeline_start).count();
            m_lastFrameProcessUs.store(elapsedUs);
            const int64_t prevAvg = m_avgFrameProcessUs.load();
            m_avgFrameProcessUs.store(prevAvg + (elapsedUs - prevAvg) / 16);

            if (ok) {
                BuildTouchVhfReports(frame);
                DispatchVhfReports(frame);

                // 如果处理成功, 写回给 GUI 
                {
                    std::lock_guard<std::mutex> lock(m_latestFrameMutex);
                    m_latestFrame = frame;
                }

                // Push to DVR buffer (automatically overwrites old frames)
                m_dvrBuffer->PushOverwriting(frame);
            }
        }
    }
    LOG_INFO("App", "RuntimeOrchestrator::ProcessingThreadFunc", "Unknown", "Processing Thread stopped.");
}

void RuntimeOrchestrator::BuildTouchVhfReports(Engine::HeatmapFrame& frame) const {
    // HID Descriptor (HidInjectorThp.sys) report layout (Report ID 0x01):
    //   byte[0]        = Report ID (0x01)
    //   byte[1..6]     = Finger 0: {flags(1), contactId(1), X_lo(1), X_hi(1), Y_lo(1), Y_hi(1)}
    //   byte[7..12]    = Finger 1  ... etc up to 5 fingers per packet
    //   Total = 1 + 5*6 = 31 bytes.  NO ContactCount field in descriptor.
    frame.touchPackets[0] = Engine::TouchPacket{};
    frame.touchPackets[1] = Engine::TouchPacket{};
    frame.touchPackets[0].bytes.fill(0);
    frame.touchPackets[1].bytes.fill(0);
    frame.touchPackets[0].bytes[0] = frame.touchPackets[0].reportId; // 0x01
    frame.touchPackets[1].bytes[0] = frame.touchPackets[1].reportId; // 0x01
    // Keep length = 32 (0x20): driver HidWrite_EvtIoWrite requires exactly 0x20 bytes.
    // byte[31] stays 0 via fill(0) above — no ContactCount field in HID descriptor,
    // so the zero padding is safe and eliminates the previous ghost touch at (0,0)/ID=128.

    std::vector<const Engine::TouchContact*> reportable;
    reportable.reserve(frame.contacts.size());
    for (const auto& c : frame.contacts) {
        if (c.id <= 0 || !c.isReported) {
            continue;
        }
        reportable.push_back(&c);
    }

    const size_t count = std::min<size_t>(10, reportable.size());
    // byte[31] = ContactCount (HID Usage 0x54) — the descriptor ends with this field.
    // Windows HID Hybrid Mode: first packet carries the TOTAL contact count,
    // subsequent packets carry 0 (OS already knows the total from packet 0).
    frame.touchPackets[0].bytes[31] = static_cast<uint8_t>(count);
    // frame.touchPackets[1].bytes[31] stays 0 (fill(0) above)

    const bool flip = m_vhfTransposeEnabled.load();
    for (size_t i = 0; i < count; ++i) {
        const Engine::TouchContact& c = *reportable[i];
        const size_t packetIndex = (i < 5) ? 0 : 1;
        const size_t slot = (i < 5) ? i : (i - 5);
        const size_t base = 1 + slot * 6;
        auto& bytes = frame.touchPackets[packetIndex].bytes;

        bytes[base]     = EncodeVhfContactState(c);  // bit0=TipSwitch, bit1=Confidence
        bytes[base + 1] = static_cast<uint8_t>(std::clamp(c.id, 0, 255));
        // HID descriptor: X LogMax=16000, Y LogMax=25600
        // Physical: grid col 0..60 → X, row 0..40 → Y
        const bool invertX = !flip;
        const bool invertY = flip;
        WriteU16Le(bytes, base + 2, ToVhf(c.y, 40.0f, 16000.0f, invertY));  // byte[2-3] = X slot
        WriteU16Le(bytes, base + 4, ToVhf(c.x, 60.0f, 25600.0f, invertX));  // byte[4-5] = Y slot
    }

    frame.touchPackets[0].valid = (count > 0);
    frame.touchPackets[1].valid = (count > 5);

    // Always log Up events immediately (unthrottled) for diagnostics
    for (const auto& c : frame.contacts) {
        if (c.state == Engine::TouchStateUp) {
            LOG_INFO("App", "BuildTouchVhfReports", "VHF",
                "[UP] id={} isReported={} x={:.1f} y={:.1f} reportEvent={}",
                c.id, c.isReported, c.x, c.y, (int)c.reportEvent);
        }
    }

    // Diagnostic: log contact state every 60 frames
    static int s_logCounter = 0;
    if (++s_logCounter >= 60) {
        s_logCounter = 0;
        LOG_INFO("App", "BuildTouchVhfReports", "VHF",
            "contacts={} reportable={} valid={}",
            frame.contacts.size(), reportable.size(), count);
        for (size_t i = 0; i < std::min<size_t>(frame.contacts.size(), 3); ++i) {
            const auto& c = frame.contacts[i];
            LOG_INFO("App", "BuildTouchVhfReports", "VHF",
                "  [{}] id={} state={} isReported={} x={:.1f} y={:.1f}",
                i, c.id, (int)c.state, c.isReported, c.x, c.y);
        }
    }
}


uint8_t RuntimeOrchestrator::MapPenEventToAck(uint8_t eventCode, bool* outKnown) {
    if (outKnown) {
        *outKnown = true;
    }
    switch (eventCode) {
    case 0x2Fu: return 11u;
    case 0x70u: return 0u;
    case 0x71u: return 1u;
    case 0x72u: return 2u;
    case 0x73u: return 13u;
    case 0x74u: return 3u;
    case 0x75u: return 4u;
    case 0x76u: return 5u;
    case 0x77u: return 6u;
    case 0x78u: return 7u;
    case 0x79u: return 8u;
    case 0x7Bu: return 10u;
    case 0x7Cu: return 12u;
    case 0x7Fu: return 9u;
    default:
        if (outKnown) {
            *outKnown = false;
        }
        return 0u;
    }
}

void RuntimeOrchestrator::ApplyVhfStylusPostTransform(std::array<uint8_t, 13>& packetBytes) const {
    if (m_penEraserToggleState.load() == 1u) {
        // THP_Service!VHF_InjectPenData: (byte1 & 0xFE) | 0x0C
        packetBytes[1] = static_cast<uint8_t>((packetBytes[1] & 0xFEu) | 0x0Cu);
    } else {
        // THP_Service!VHF_InjectPenData: byte1 &= 0xF3
        packetBytes[1] = static_cast<uint8_t>(packetBytes[1] & 0xF3u);
    }
}

std::optional<std::wstring> RuntimeOrchestrator::FindPenMcuDevicePath() const {
    HDEVINFO devInfo = SetupDiGetClassDevsW(&PEN_MCU_DEVICE_INTERFACE_GUID,
                                            nullptr,
                                            nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    std::optional<std::wstring> devicePath = std::nullopt;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA ifData{};
        ifData.cbSize = sizeof(ifData);
        if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr, &PEN_MCU_DEVICE_INTERFACE_GUID, index, &ifData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }

        std::vector<uint8_t> detailBuffer(requiredSize, 0);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, requiredSize, nullptr, nullptr)) {
            continue;
        }

        devicePath = detail->DevicePath;
        break;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return devicePath;
}

void RuntimeOrchestrator::OnPenEvent(const Himax::Pen::PenEvent& event) {
    const uint8_t eventCode = static_cast<uint8_t>(event.code);
    m_penLastEventCode.store(eventCode);

    if (eventCode == static_cast<uint8_t>(Himax::Pen::PenUsbEventCode::EraserToggle)) {
        const uint8_t eraserState = event.payload.empty() ? 0u : event.payload.front();
        m_penEraserToggleState.store(eraserState);
    }

    bool ackKnown = false;
    const uint8_t ackCode = MapPenEventToAck(eventCode, &ackKnown);
    if (!ackKnown || !m_penCommandApi || !m_penCommandApi->IsReady()) {
        return;
    }

    if (auto ackRes = m_penCommandApi->SendEventAck(ackCode); !ackRes) {
        LOG_WARN("App", "RuntimeOrchestrator::OnPenEvent", "BTMCU",
                 "SendEventAck failed, event=0x{:02X}, ack={}, err={}",
                 static_cast<unsigned int>(eventCode),
                 static_cast<unsigned int>(ackCode),
                 static_cast<int>(ackRes.error()));
    }
}

void RuntimeOrchestrator::InitializePenBridge() {
    m_penEraserToggleState.store(0u);
    m_penLastEventCode.store(0u);

    if (!m_penCommandApi) {
        m_penCommandApi = std::make_unique<Himax::Pen::PenCommandApi>();
    }

    const auto devicePath = FindPenMcuDevicePath();
    if (!devicePath.has_value()) {
        LOG_WARN("App", "RuntimeOrchestrator::InitializePenBridge", "BTMCU",
                 "Pen MCU device interface not found, continuing without BT event bridge.");
        return;
    }

    auto initRes = m_penCommandApi->Initialize(*devicePath);
    if (!initRes) {
        LOG_WARN("App", "RuntimeOrchestrator::InitializePenBridge", "BTMCU",
                 "PenCommandApi::Initialize failed, err={}", static_cast<int>(initRes.error()));
        return;
    }

    m_penCommandApi->SetEventCallback([this](const Himax::Pen::PenEvent& event) {
        OnPenEvent(event);
    });

    if (auto startRes = m_penCommandApi->Start(); !startRes) {
        LOG_WARN("App", "RuntimeOrchestrator::InitializePenBridge", "BTMCU",
                 "PenCommandApi::Start failed, err={}", static_cast<int>(startRes.error()));
        m_penCommandApi->SetEventCallback({});
        m_penCommandApi->Shutdown();
        return;
    }

    if (auto queryRes = m_penCommandApi->QueryBootstrapInfo(); !queryRes) {
        LOG_WARN("App", "RuntimeOrchestrator::InitializePenBridge", "BTMCU",
                 "QueryBootstrapInfo failed, err={}", static_cast<int>(queryRes.error()));
    }

    LOG_INFO("App", "RuntimeOrchestrator::InitializePenBridge", "BTMCU",
             "Pen MCU bridge ready.");
}

void RuntimeOrchestrator::ShutdownPenBridge() {
    if (!m_penCommandApi) {
        return;
    }

    m_penCommandApi->SetEventCallback({});
    m_penCommandApi->Stop();
    m_penCommandApi->Shutdown();
    m_penEraserToggleState.store(0u);
    m_penLastEventCode.store(0u);
}

void RuntimeOrchestrator::DispatchVhfReports(Engine::HeatmapFrame& frame) {
    if (!m_vhfReportingEnabled.load()) {
        return;
    }

    // Skip entirely when there is nothing to dispatch.
    const bool hasTouchPacket = frame.touchPackets[0].valid || frame.touchPackets[1].valid;
    const bool hasStylusPacket = frame.stylus.packet.valid;
    if (!hasTouchPacket && !hasStylusPacket) {
        // Send an explicit "all fingers up" packet (ContactCount=0) when transitioning
        // from having touches to none. Windows shell requires this to finalize tap/click.
        if (m_vhfHadTouchLastFrame.exchange(false)) {
            std::lock_guard<std::mutex> lock(m_vhfMutex);
            if (EnsureVhfDeviceOpenLocked()) {
                Engine::TouchPacket allUp{};
                allUp.bytes.fill(0);
                allUp.bytes[0] = 0x01;  // ReportID
                // byte[31] = ContactCount = 0  (already 0)
                WriteVhfPacketLocked(allUp.bytes.data(), allUp.length, "touch-all-up");
            }
        }
        return;
    }
    m_vhfHadTouchLastFrame.store(true);

    std::lock_guard<std::mutex> lock(m_vhfMutex);
    if (!EnsureVhfDeviceOpenLocked()) {
        return;
    }

    if (frame.touchPackets[0].valid) {
        const auto& packet = frame.touchPackets[0];
        WriteVhfPacketLocked(packet.bytes.data(), packet.length, "touch-packet-0");
    }
    if (frame.touchPackets[1].valid) {
        const auto& packet = frame.touchPackets[1];
        WriteVhfPacketLocked(packet.bytes.data(), packet.length, "touch-packet-1");
    }
    if (frame.stylus.packet.valid) {
        auto& packet = frame.stylus.packet;
        ApplyVhfStylusPostTransform(packet.bytes);
        WriteVhfPacketLocked(packet.bytes.data(), packet.length, "stylus-packet");
    }
}

bool RuntimeOrchestrator::EnsureVhfDeviceOpenLocked() {
    if (m_vhfDeviceHandle != INVALID_HANDLE_VALUE) {
        return true;
    }

    HDEVINFO devInfo = SetupDiGetClassDevsW(&VHF_DEVICE_INTERFACE_GUID,
                                            nullptr,
                                            nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool opened = false;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA ifData{};
        ifData.cbSize = sizeof(ifData);
        if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr, &VHF_DEVICE_INTERFACE_GUID, index, &ifData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }

        std::vector<uint8_t> detailBuffer(requiredSize, 0);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, requiredSize, nullptr, nullptr)) {
            continue;
        }

        HANDLE handle = CreateFileW(detail->DevicePath,
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    0,
                                    nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            m_vhfDeviceHandle = handle;
            opened = true;
            LOG_INFO("App", "RuntimeOrchestrator::EnsureVhfDeviceOpenLocked", "VHF",
                     "VHF device opened.");
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return opened;
}

void RuntimeOrchestrator::CloseVhfDeviceLocked() {
    if (m_vhfDeviceHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_vhfDeviceHandle);
        m_vhfDeviceHandle = INVALID_HANDLE_VALUE;
    }
}

void RuntimeOrchestrator::ReopenVhfDeviceLocked() {
    CloseVhfDeviceLocked();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EnsureVhfDeviceOpenLocked();
}

bool RuntimeOrchestrator::WriteVhfPacketLocked(const uint8_t* data, size_t length, const char* tag) {
    if (data == nullptr || length == 0) {
        return false;
    }

    if (!EnsureVhfDeviceOpenLocked()) {
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(m_vhfDeviceHandle,
                              data,
                              static_cast<DWORD>(length),
                              &written,
                              nullptr);
    if (!ok || written != length) {
        const DWORD err = GetLastError();
        LOG_WARN("App", "RuntimeOrchestrator::WriteVhfPacketLocked", "VHF",
                 "Write {} failed (len={}, written={}, err={}), trying reopen.",
                 tag,
                 static_cast<unsigned int>(length),
                 static_cast<unsigned int>(written),
                 static_cast<unsigned int>(err));
        ReopenVhfDeviceLocked();
        return false;
    }

    return true;
}



void RuntimeOrchestrator::ResetAutoAfeFreqShiftSyncState() {
    m_autoAfeFreqShiftEnabledSent = false;
    m_autoAfeFreqPairInitialized = false;
    m_autoAfeFreqIdx0Tx1 = 0;
    m_autoAfeFreqIdx0Tx2 = 0;
    m_autoAfeLastForcedFreqIdx = -1;
    m_autoAfeLastForceTs = std::chrono::steady_clock::time_point{};
}

void RuntimeOrchestrator::HandleAutoAfeFreqShiftSync(const Engine::StylusFrameData& stylus) {
    if (!m_autoAfeFreqShiftSyncEnabled.load()) {
        return;
    }
    if (!m_runtime) {
        return;
    }
    if (!stylus.slaveValid) {
        return;
    }

    // Keep firmware freq-shift feature on while auto-sync is enabled.
    if (!m_autoAfeFreqShiftEnabledSent) {
        m_runtime->SubmitCommand(command{AFE_Command::EnableFreqShift, 0},
                                 CommandSource::External, "AutoAfeSync:Enable");
        m_autoAfeFreqShiftEnabledSent = true;
        LOG_INFO("App", "RuntimeOrchestrator::HandleAutoAfeFreqShiftSync", "Engine",
                 "AutoSync: EnableFreqShift queued.");
    }

    // Learn the pair that corresponds to freq index 0 on this single device.
    if (!m_autoAfeFreqPairInitialized && stylus.tx1Freq != 0 && stylus.tx2Freq != 0 && stylus.tx1Freq != stylus.tx2Freq) {
        m_autoAfeFreqPairInitialized = true;
        m_autoAfeFreqIdx0Tx1 = stylus.tx1Freq;
        m_autoAfeFreqIdx0Tx2 = stylus.tx2Freq;
        m_autoAfeLastForcedFreqIdx = 0;
    }

    // No freq-shift request in this frame.
    if ((stylus.status & 0x40u) == 0u) {
        return;
    }
    if (!m_autoAfeFreqPairInitialized) {
        return;
    }

    int targetIdx = -1;
    if (stylus.nextTx1Freq == m_autoAfeFreqIdx0Tx1 && stylus.nextTx2Freq == m_autoAfeFreqIdx0Tx2) {
        targetIdx = 0;
    } else if (stylus.nextTx1Freq == m_autoAfeFreqIdx0Tx2 && stylus.nextTx2Freq == m_autoAfeFreqIdx0Tx1) {
        targetIdx = 1;
    } else {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (targetIdx == m_autoAfeLastForcedFreqIdx && m_autoAfeLastForceTs.time_since_epoch().count() != 0) {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_autoAfeLastForceTs).count();
        if (elapsedMs < m_autoAfeForceCooldownMs) {
            return;
        }
    }

    m_runtime->SubmitCommand(command{AFE_Command::ForceToFreqPoint,
                                      static_cast<uint8_t>(targetIdx)},
                             CommandSource::External, "AutoAfeSync:Force");

    m_autoAfeLastForcedFreqIdx = targetIdx;
    m_autoAfeLastForceTs = now;
    LOG_INFO("App", "RuntimeOrchestrator::HandleAutoAfeFreqShiftSync", "Engine",
             "AutoSync: ForceToFreqPoint idx={} queued for next pair [0x{:04X}, 0x{:04X}]",
             targetIdx,
             static_cast<unsigned int>(stylus.nextTx1Freq),
             static_cast<unsigned int>(stylus.nextTx2Freq));
}

void RuntimeOrchestrator::TriggerDVRExport(bool exportHeatmap, bool exportMasterStatus, bool exportSlaveStatus) {
    if (!m_dvrBuffer) {
        LOG_ERROR("App", "RuntimeOrchestrator::TriggerDVRExport", "Unknown", "DVR buffer is not initialized.");
        return;
    }

    auto snapshot = m_dvrBuffer->GetSnapshot();
    if (snapshot.empty()) {
        LOG_WARN("App", "RuntimeOrchestrator::TriggerDVRExport", "Unknown", "DVR buffer is empty, nothing to export.");
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm time_info;
    localtime_s(&time_info, &time_t_now);
    
    std::filesystem::path dir("exports/dvr");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR("App", "RuntimeOrchestrator::TriggerDVRExport", "Unknown", "Failed to create directory: exports/dvr");
    }

    char filename[128];
    sprintf_s(filename, "dvr_backtrack_%04d%02d%02d_%02d%02d%02d.csv",
              time_info.tm_year + 1900, time_info.tm_mon + 1, time_info.tm_mday,
              time_info.tm_hour, time_info.tm_min, time_info.tm_sec);
              
    std::filesystem::path fullPath = dir / filename;

    FILE* fp = nullptr;
    fopen_s(&fp, fullPath.string().c_str(), "w");
    if (!fp) {
        LOG_ERROR("App", "RuntimeOrchestrator::TriggerDVRExport", "Unknown", "Failed to create DVR export file: %s", fullPath.string().c_str());
        return;
    }

    LOG_INFO("App", "RuntimeOrchestrator::TriggerDVRExport", "Unknown", "Exporting %zu frames to %s...", snapshot.size(), fullPath.string().c_str());

    for (size_t i = 0; i < snapshot.size(); ++i) {
        const auto& f = snapshot[i];
        fprintf(fp, "--- Frame [%zu] --- TS: %llu\n", i, f.timestamp);
        
        // Contacts are always printed
        fprintf(fp, "Contacts: %zu\n", f.contacts.size());
        for (const auto& c : f.contacts) {
            fprintf(fp, "ID:%d, X:%.3f, Y:%.3f, State:%d, Area:%d, SigSum:%d, RptEvt:%d, Life:0x%X, RptFlg:0x%X\n",
                    c.id, c.x, c.y, c.state, c.area, c.signalSum, c.reportEvent,
                    static_cast<unsigned int>(c.lifeFlags),
                    static_cast<unsigned int>(c.reportFlags));
        }
        for (size_t p = 0; p < f.touchPackets.size(); ++p) {
            const auto& pkt = f.touchPackets[p];
            fprintf(fp, "TouchPacket[%zu]: %s RID=0x%02X Len=%u",
                    p,
                    pkt.valid ? "valid" : "invalid",
                    static_cast<unsigned int>(pkt.reportId),
                    static_cast<unsigned int>(pkt.length));
            if (pkt.valid) {
                for (size_t k = 0; k < pkt.bytes.size(); ++k) {
                    fprintf(fp, "%s%02X", (k == 0 ? " " : " "), static_cast<unsigned int>(pkt.bytes[k]));
                }
            }
            fprintf(fp, "\n");
        }

        const auto& s = f.stylus;
        fprintf(fp,
                "Stylus: SlaveValid=%d, Off=%u, Ck=0x%04X(%d), Tx1Valid=%d, Tx2Valid=%d, Status=0x%X, X=%.3f, Y=%.3f, "
                "RptX=%u, RptY=%u, Pressure=%u(raw=%u,map=%u), Peak1=%u, Peak2=%u, "
                "Tilt=%d/%d(pre=%d/%d,mag=%.2f,azi=%.1f), Conf=%.3f, Mode=%u, DT=%u, Res=%u, Recheck=%d/%d, Noise=%d/%d, Supp=%d[%u]\n",
                s.slaveValid ? 1 : 0,
                static_cast<unsigned int>(s.slaveWordOffset),
                static_cast<unsigned int>(s.checksum16),
                s.checksumOk ? 1 : 0,
                s.tx1BlockValid ? 1 : 0,
                s.tx2BlockValid ? 1 : 0,
                static_cast<unsigned int>(s.status),
                s.point.x,
                s.point.y,
                static_cast<unsigned int>(s.point.reportX),
                static_cast<unsigned int>(s.point.reportY),
                static_cast<unsigned int>(s.point.pressure),
                static_cast<unsigned int>(s.point.rawPressure),
                static_cast<unsigned int>(s.point.mappedPressure),
                static_cast<unsigned int>(s.point.peakTx1),
                static_cast<unsigned int>(s.point.peakTx2),
                static_cast<int>(s.point.tiltX),
                static_cast<int>(s.point.tiltY),
                static_cast<int>(s.point.preTiltX),
                static_cast<int>(s.point.preTiltY),
                s.point.tiltMagnitude,
                s.point.tiltAzimuthDeg,
                s.point.confidence,
                static_cast<unsigned int>(s.asaMode),
                static_cast<unsigned int>(s.dataType),
                static_cast<unsigned int>(s.processResult),
                s.recheckEnabled ? 1 : 0,
                s.recheckPassed ? 1 : 0,
                s.hpp3NoiseInvalid ? 1 : 0,
                s.hpp3NoiseDebounce ? 1 : 0,
                s.touchSuppressActive ? 1 : 0,
                static_cast<unsigned int>(s.touchSuppressFrames));
        if (s.packet.valid) {
            fprintf(fp, "StylusPacket:");
            for (size_t k = 0; k < s.packet.bytes.size(); ++k) {
                fprintf(fp, "%s%02X", (k == 0 ? " " : ""), static_cast<unsigned int>(s.packet.bytes[k]));
            }
            fprintf(fp, "\n");
        } else {
            fprintf(fp, "StylusPacket: invalid\n");
        }
        
        if (exportHeatmap) {
            fprintf(fp, "Heatmap:\n");
            for (int y = 0; y < 40; ++y) {
                for (int x = 0; x < 60; ++x) {
                    fprintf(fp, "%d%s", f.heatmapMatrix[y][x], (x == 59 ? "" : ","));
                }
                fprintf(fp, "\n");
            }
        }

        if (exportMasterStatus) {
            fprintf(fp, "Master Status Suffix:\n");
            if (f.rawData.size() >= 5063) {
                const uint8_t* ptr = f.rawData.data() + 4807;
                for (int j = 0; j < 128; ++j) {
                    uint16_t val = static_cast<uint16_t>(ptr[j * 2] | (ptr[j * 2 + 1] << 8));
                    fprintf(fp, "%d%s", val, (j == 127 ? "" : ","));
                }
                fprintf(fp, "\n");
            } else {
                fprintf(fp, "Data unavailable\n");
            }
        }

        if (exportSlaveStatus) {
            fprintf(fp, "Slave Status Suffix:\n");
            if (f.rawData.size() >= 5402) {
                const uint8_t* ptr = f.rawData.data() + 5070;
                for (int j = 0; j < 166; ++j) {
                    uint16_t val = static_cast<uint16_t>(ptr[j * 2] | (ptr[j * 2 + 1] << 8));
                    fprintf(fp, "%d%s", val, (j == 165 ? "" : ","));
                }
                fprintf(fp, "\n");
            } else {
                fprintf(fp, "Data unavailable\n");
            }
        }
        
        fprintf(fp, "\n");
    }

    fclose(fp);
    LOG_INFO("App", "RuntimeOrchestrator::TriggerDVRExport", "Unknown", "DVR Export Complete: %s", fullPath.string().c_str());
}

void RuntimeOrchestrator::SaveConfig() {
    std::ofstream out("config.ini");
    if (!out.is_open()) {
        LOG_ERROR("App", "RuntimeOrchestrator::SaveConfig", "System", "Failed to open config.ini for writing.");
        return;
    }
    out << "[Runtime]\n";
    out << "AutoAfeFreqShiftSyncEnabled=" << (m_autoAfeFreqShiftSyncEnabled.load() ? 1 : 0) << "\n";
    out << "MasterParserOnlyMode=" << (m_masterParserOnlyMode ? 1 : 0) << "\n";
    out << "VhfReportingEnabled=" << (m_vhfReportingEnabled.load() ? 1 : 0) << "\n";
    out << "VhfTransposeEnabled=" << (m_vhfTransposeEnabled.load() ? 1 : 0) << "\n\n";
    for (const auto& p : m_pipeline.GetProcessors()) {
        out << "[" << p->GetName() << "]\n";
        p->SaveConfig(out);
        out << "\n";
    }
    out.close();
    LOG_INFO("App", "RuntimeOrchestrator::SaveConfig", "System", "Successfully saved global parameters to config.ini");
}

void RuntimeOrchestrator::LoadConfig() {
    std::ifstream in("config.ini");
    if (!in.is_open()) {
        LOG_INFO("App", "RuntimeOrchestrator::LoadConfig", "System", "No config.ini found. Using default parameters.");
        return;
    }
    
    std::string line;
    std::string currentSection;
    Engine::IFrameProcessor* currentProcessor = nullptr;
    
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back(); // Handle Windows CRLF
        if (line.empty() || line[0] == ';') continue;
        
        if (line[0] == '[') {
            size_t endBracket = line.find(']');
            if (endBracket != std::string::npos) {
                currentSection = line.substr(1, endBracket - 1);
                std::string section = currentSection;
                if (section == "Touch Tracker (IDT/TE-lite)") {
                    section = "Touch Tracker (IDT/TS/TE-lite)";
                } else if (section == "Stylus Processor (Slave/HPP-lite)") {
                    section = "Stylus Processor (ASA/HPP2/HPP3-lite)";
                }
                if (section == "Runtime") {
                    currentProcessor = nullptr;
                    continue;
                }
                currentProcessor = nullptr;
                for (const auto& p : m_pipeline.GetProcessors()) {
                    if (p->GetName() == section) {
                        currentProcessor = p.get();
                        break;
                    }
                }
            }
            continue;
        }
        
        auto pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);

            if (currentSection == "Runtime") {
                const bool enabled = (val == "1" || val == "true");
                if (key == "AutoAfeFreqShiftSyncEnabled") {
                    SetAutoAfeFreqShiftSyncEnabled(enabled);
                } else if (key == "MasterParserOnlyMode") {
                    SetMasterParserOnlyMode(enabled);
                } else if (key == "VhfReportingEnabled") {
                    SetVhfReportingEnabled(enabled);
                } else if (key == "VhfTransposeEnabled") {
                    SetVhfTransposeEnabled(enabled);
                }
                continue;
            }

            if (!currentProcessor) {
                continue;
            }

            // Explicitly handle "Enabled" since it's in the base class IFrameProcessor
            if (key == "Enabled") {
                currentProcessor->SetEnabled(val == "1" || val == "true");
            } else {
                currentProcessor->LoadConfig(key, val);
            }
        }
    }
    in.close();
    LOG_INFO("App", "RuntimeOrchestrator::LoadConfig", "System", "Successfully loaded global parameters from config.ini");
}

void RuntimeOrchestrator::SetMasterParserOnlyMode(bool enabled) {
    if (enabled == m_masterParserOnlyMode) {
        return;
    }

    const auto& processors = m_pipeline.GetProcessors();
    if (processors.empty()) {
        m_masterParserOnlyMode = enabled;
        return;
    }

    constexpr const char* kMasterParserName = "Master Frame Parser";

    if (enabled) {
        m_savedProcessorEnabledStates.clear();
        m_savedProcessorEnabledStates.reserve(processors.size());

        for (const auto& processor : processors) {
            m_savedProcessorEnabledStates.push_back(processor->IsEnabled());
            const bool isMasterParser = (processor->GetName() == kMasterParserName);
            processor->SetEnabled(isMasterParser);
        }

        m_masterParserOnlyMode = true;
        LOG_INFO("App", "RuntimeOrchestrator::SetMasterParserOnlyMode", "UI",
                 "Master-parser-only mode enabled: all processors except '{}' are disabled.",
                 kMasterParserName);
        return;
    }

    if (m_savedProcessorEnabledStates.size() == processors.size()) {
        for (size_t i = 0; i < processors.size(); ++i) {
            processors[i]->SetEnabled(m_savedProcessorEnabledStates[i]);
        }
    } else {
        for (const auto& processor : processors) {
            processor->SetEnabled(true);
        }
    }

    m_savedProcessorEnabledStates.clear();
    m_masterParserOnlyMode = false;
    LOG_INFO("App", "RuntimeOrchestrator::SetMasterParserOnlyMode", "UI",
             "Master-parser-only mode disabled: processor enable states restored.");
}

} // namespace App
