#include "btmcu/PenBridge.h"
#include "Logger.h"

#include <Windows.h>
#include <SetupAPI.h>
#include <hidsdi.h>
#include <algorithm>
#include <chrono>
#include <cwctype>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace Himax::Pen {

// ── 静态常量 ──────────────────────────────────────────────────────────────
// GUID 来自原厂 THP_Service 注册的设备接口（与 BtMcuTestTool 完全相同）
const GUID PenBridge::kEventDeviceGuid =
    {0xdd0ebedb, 0xf1d6, 0x4cfa, {0xac, 0xca, 0x71, 0xe6, 0x6d, 0x31, 0x78, 0xca}};

// ── 构造/析构 ──────────────────────────────────────────────────────────────
PenBridge::PenBridge()
    : m_evtTransport(CreatePenUsbTransportWin32()),
      m_pressTransport(CreatePenUsbTransportWin32())
{
}

PenBridge::~PenBridge() {
    Stop();
}

// ── Start / Stop ───────────────────────────────────────────────────────────
void PenBridge::Start() {
    if (m_running.exchange(true)) return; // 已在运行

    // 事件通道：新建专用线程（负责设备发现 + 握手 + ACK 响应）
    m_evtThread = std::thread(&PenBridge::EventThreadFunc, this);

    // 压力通道：新建专用线程（负责设备发现 + 连续读取）
    m_pressThread = std::thread(&PenBridge::PressureThreadFunc, this);

    LOG_INFO("PenBridge", "Start", "MCU",
             "BT MCU bridge started (2 worker threads launched).");
}

void PenBridge::Stop() {
    if (!m_running.exchange(false)) return;

    // 关闭通道句柄，使阻塞中的 ReadPacket 立即返回错误，线程可顺利退出
    if (m_evtTransport)   m_evtTransport->Close();
    if (m_pressTransport) m_pressTransport->Close();

    if (m_evtThread.joinable())   m_evtThread.join();
    if (m_pressThread.joinable()) m_pressThread.join();

    LOG_INFO("PenBridge", "Stop", "MCU",
             "BT MCU bridge stopped.");
}

// ── 回调设置 ───────────────────────────────────────────────────────────────
void PenBridge::SetEventCallback(PenEventCallback cb) {
    std::lock_guard<std::mutex> lk(m_cbMutex);
    m_eventCallback = std::move(cb);
}

void PenBridge::SetPressureCallback(PressureCallback cb) {
    std::lock_guard<std::mutex> lk(m_cbMutex);
    m_pressureCallback = std::move(cb);
}

PenPressureStats PenBridge::GetPressureStats() const {
    std::lock_guard<std::mutex> lk(m_statsMutex);
    return m_stats;
}

// ── 设备路径发现 ───────────────────────────────────────────────────────────
std::optional<std::wstring> PenBridge::FindEventDevicePath() {
    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &kEventDeviceGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return std::nullopt;

    std::optional<std::wstring> result;
    for (DWORD i = 0; ; ++i) {
        SP_DEVICE_INTERFACE_DATA ifData{};
        ifData.cbSize = sizeof(ifData);
        if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr,
                                         &kEventDeviceGuid, i, &ifData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        if (reqSize < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<uint8_t> buf(reqSize, 0);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, det, reqSize,
                                             nullptr, nullptr)) {
            result = det->DevicePath;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

std::optional<std::wstring> PenBridge::FindPressureDevicePath() {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return std::nullopt;

    auto containsCI = [](const std::wstring& hay, const wchar_t* needle) {
        std::wstring lo = hay;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::towlower);
        std::wstring nl = needle;
        std::transform(nl.begin(), nl.end(), nl.begin(), ::towlower);
        return lo.find(nl) != std::wstring::npos;
    };

    std::optional<std::wstring> result;
    for (DWORD i = 0; ; ++i) {
        SP_DEVICE_INTERFACE_DATA ifData{};
        ifData.cbSize = sizeof(ifData);
        if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        if (reqSize < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<uint8_t> buf(reqSize, 0);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, det, reqSize,
                                              nullptr, nullptr)) continue;
        std::wstring path = det->DevicePath;
        if (containsCI(path, kPressureHidMatch)) {
            result = path;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

// ── 协议辅助 ───────────────────────────────────────────────────────────────
// ACK code 映射表（与 BtMcuTestTool::GetAckCodeForEvent 完全一致）
int PenBridge::GetAckCode(uint8_t eventCode) {
    switch (eventCode) {
    case 0x70: return 0;    case 0x71: return 1;    case 0x72: return 2;
    case 0x73: return 0xD;  case 0x74: return 3;    case 0x75: return 4;
    case 0x76: return 5;    case 0x77: return 6;    case 0x78: return 7;
    case 0x79: return 8;    case 0x7B: return 0xA;  case 0x7C: return 0xC;
    case 0x7F: return 9;    case 0x2F: return 0xB;
    default:   return -1;
    }
}

void PenBridge::SendRawPacket(const std::vector<uint8_t>& pkt) {
    if (!m_evtTransport || !m_evtTransport->IsOpen()) return;
    (void)m_evtTransport->WritePacket(pkt);
}

void PenBridge::SendAck(uint8_t ackCode) {
    // 格式：[07 01 02 00] [01 80] [11 20] [ackCode]
    const std::vector<uint8_t> pkt = {
        0x07, 0x01, 0x02, 0x00,
        0x01, 0x80, 0x11, 0x20, ackCode
    };
    SendRawPacket(pkt);
    LOG_INFO("PenBridge", "SendAck", "MCU",
             "ACK sent: 0x{:02X}", ackCode);
}

void PenBridge::SendInitParamEcho(const uint8_t* data, size_t len) {
    // 0x7D01 回显：将 MCU 发来的初始化参数原样回传，防止 MCU 超时断流
    size_t payloadLen = std::min(len, size_t(32));
    std::vector<uint8_t> pkt(8 + payloadLen, 0);
    pkt[0] = 0x07; pkt[1] = 0x01; pkt[2] = 0x02; pkt[3] = 0x00;
    pkt[4] = 0x01; pkt[5] = 0x7D;  // cmd = 0x7D01 (LE)
    pkt[6] = 0x11; pkt[7] = 0x20;
    std::copy(data, data + payloadLen, pkt.begin() + 8);
    SendRawPacket(pkt);
    LOG_INFO("PenBridge", "SendInitParamEcho", "MCU",
             "0x7D01 InitParam echo sent ({} bytes payload).", payloadLen);
}

/// 解析收到的事件帧，触发回调，并自动 ACK
void PenBridge::DispatchEvent(const std::vector<uint8_t>& rxBuf) {
    if (rxBuf.size() < 7) return;

    const uint8_t eventCode = rxBuf[5];

    // 1. 自动 ACK
    int ackCode = GetAckCode(eventCode);
    if (ackCode >= 0) {
        SendAck(static_cast<uint8_t>(ackCode));
    }

    // 2. PEN_REP_PARAM (0x7B) → 额外发送 0x7D01 InitParam 回显
    if (eventCode == 0x7B && rxBuf.size() > 8) {
        SendInitParamEcho(rxBuf.data() + 8, rxBuf.size() - 8);
    }

    // 3. 触发上层回调
    PenEventCallback cbCopy;
    {
        std::lock_guard<std::mutex> lk(m_cbMutex);
        cbCopy = m_eventCallback;
    }
    if (cbCopy) {
        PenEvent ev;
        ev.code     = static_cast<PenUsbEventCode>(eventCode);
        ev.payload  = std::vector<uint8_t>(rxBuf.begin() + 8, rxBuf.end());
        ev.receivedAt = std::chrono::steady_clock::now();
        cbCopy(ev);
    }
}

// ── 握手 ──────────────────────────────────────────────────────────────────
void PenBridge::RunHandshake() {
    if (!m_evtTransport || !m_evtTransport->IsOpen()) return;
    LOG_INFO("PenBridge", "RunHandshake", "MCU", "Sending 0x7101 + 0x7701 handshake.");

    // 0x7101 Query Pen Status
    const std::vector<uint8_t> q1 = {0x07,0x00,0x02,0x00, 0x01,0x71, 0x11,0x00};
    SendRawPacket(q1);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 0x7701 Query MCU Info
    const std::vector<uint8_t> q2 = {0x07,0x00,0x02,0x00, 0x01,0x77, 0x11,0x00};
    SendRawPacket(q2);

    LOG_INFO("PenBridge", "RunHandshake", "MCU", "Handshake complete.");
}

// ── 事件线程 ──────────────────────────────────────────────────────────────
/// 专用线程：负责事件通道的设备发现、初始握手、循环读取和 ACK 响应。
/// ReadPacket 以 1000ms 超时阻塞，Stop() 关闭句柄使其立即返回。
void PenBridge::EventThreadFunc() {
    LOG_INFO("PenBridge", "EventThreadFunc", "MCU",
             "[EvtThread] Started.");

    // 自动设备发现（重试直到成功或 Stop()）
    while (m_running.load()) {
        auto path = FindEventDevicePath();
        if (path) {
            auto res = m_evtTransport->Open(*path);
            if (res) {
                LOG_INFO("PenBridge", "EventThreadFunc", "MCU",
                         "[EvtThread] Event channel opened.");
                break;
            }
        }
        LOG_WARN("PenBridge", "EventThreadFunc", "MCU",
                 "[EvtThread] Event device not found, retry in 2s...");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // 初始握手（在单独 detached 线程中，避免阻塞读取循环）
    if (m_running.load()) {
        std::thread([this]{ RunHandshake(); }).detach();
    }

    // 循环读取事件帧
    while (m_running.load()) {
        std::vector<uint8_t> rxBuf;
        auto res = m_evtTransport->ReadPacket(rxBuf, 1000 /*ms*/);
        if (!res || rxBuf.empty()) continue;

        DispatchEvent(rxBuf);
    }

    LOG_INFO("PenBridge", "EventThreadFunc", "MCU",
             "[EvtThread] Exited.");
}

// ── 压力线程 ──────────────────────────────────────────────────────────────
/// 专用线程：负责压力通道的设备发现与连续读取。
/// 解析 'U' (0x55) 报文并更新 m_stats，供上层 GetPressureStats() 查询。
void PenBridge::PressureThreadFunc() {
    LOG_INFO("PenBridge", "PressureThreadFunc", "MCU",
             "[PressThread] Started.");

    // 自动设备发现（重试直到成功或 Stop()）
    while (m_running.load()) {
        auto path = FindPressureDevicePath();
        if (path) {
            auto res = m_pressTransport->Open(*path);
            if (res) {
                LOG_INFO("PenBridge", "PressureThreadFunc", "MCU",
                         "[PressThread] Pressure channel opened.");
                break;
            }
        }
        LOG_WARN("PenBridge", "PressureThreadFunc", "MCU",
                 "[PressThread] Pressure device not found, retry in 2s...");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // 循环读取压力帧
    while (m_running.load()) {
        std::vector<uint8_t> rxBuf;
        auto res = m_pressTransport->ReadPacket(rxBuf, 1000 /*ms*/);
        if (!res || rxBuf.empty()) continue;

        // 解析 'U' 报文: [0x55][freq1][freq2][p0L][p0H][p1L][p1H][p2L][p2H][p3L][p3H]...
        if (rxBuf.size() >= 11 && rxBuf[0] == 0x55) {
            PenPressureStats s;
            s.reportType = rxBuf[0];
            s.freq1      = rxBuf[1];
            s.freq2      = rxBuf[2];
            for (int k = 0; k < 4; ++k) {
                s.press[k] = static_cast<uint16_t>(rxBuf[3 + k * 2]) |
                             (static_cast<uint16_t>(rxBuf[4 + k * 2]) << 8);
            }
            {
                std::lock_guard<std::mutex> lk(m_statsMutex);
                m_stats = s;
            }
            // 压感回调：将第一通道压感实时转发给 StylusPipeline
            {
                std::lock_guard<std::mutex> lk(m_cbMutex);
                if (m_pressureCallback) m_pressureCallback(s.press[0]);
            }
        }
    }

    LOG_INFO("PenBridge", "PressureThreadFunc", "MCU",
             "[PressThread] Exited.");
}

} // namespace Himax::Pen
