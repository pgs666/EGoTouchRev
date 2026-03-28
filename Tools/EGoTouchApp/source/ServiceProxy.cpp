#include "ServiceProxy.h"
#include "Logger.h"
#include "GuiLogSink.h"
#include "IFrameProcessor.h"
#include "IpcProtocol.h"
#include <sstream>

// Pipeline Processors (local copy for GUI parameter editing)
#include "MasterFrameParser.h"
#include "BaselineSubtraction.h"
#include "CMFProcessor.h"
#include "GridIIRProcessor.h"
#include "FeatureExtractor.h"
#include "StylusProcessor.h"
#include "TouchTracker.h"
#include "CoordinateFilter.h"
#include "TouchGestureStateMachine.h"
#include <chrono>
#include <fstream>
#include <string>
#include <iomanip>
#include <sstream>
#include <filesystem>

namespace App {

ServiceProxy::ServiceProxy()
    : m_dvrBuffer(std::make_unique<RingBuffer<Engine::HeatmapFrame, 480>>()) {
    // Build local pipeline (mirrors Service pipeline for GUI config editing)
    m_pipeline.AddProcessor(std::make_unique<Engine::MasterFrameParser>());
    m_pipeline.AddProcessor(std::make_unique<Engine::BaselineSubtraction>());
    m_pipeline.AddProcessor(std::make_unique<Engine::CMFProcessor>());
    m_pipeline.AddProcessor(std::make_unique<Engine::GridIIRProcessor>());
    m_pipeline.AddProcessor(std::make_unique<Engine::FeatureExtractor>());
    m_pipeline.AddProcessor(std::make_unique<Engine::StylusProcessor>());
    m_pipeline.AddProcessor(std::make_unique<Engine::TouchTracker>());
    m_pipeline.AddProcessor(std::make_unique<Engine::CoordinateFilter>());
    m_pipeline.AddProcessor(std::make_unique<Engine::TouchGestureStateMachine>());
    LoadConfig();
}

ServiceProxy::~ServiceProxy() {
    StopAutoDiscovery();
    Disconnect();
}

bool ServiceProxy::Connect() {
    // 1. Create shared memory (App owns it)
    if (!m_frameReader.Create(kSharedMemName)) {
        LOG_ERROR("App", "ServiceProxy::Connect", "IPC",
                  "Failed to create shared memory.");
        return false;
    }
    // 2. Open config dirty flag
    m_configDirty.Open();

    // 3. Connect pipe to Service
    if (!m_client.Connect(3000)) {
        LOG_ERROR("App", "ServiceProxy::Connect", "IPC",
                  "Pipe connection failed.");
        m_frameReader.Close();
        return false;
    }
    // 4. Tell Service to enter debug mode
    auto resp = m_client.EnterDebugMode(kSharedMemName);
    if (!resp.success) {
        LOG_ERROR("App", "ServiceProxy::Connect", "IPC",
                  "EnterDebugMode rejected.");
        m_client.Disconnect();
        m_frameReader.Close();
        return false;
    }
    // 5. Start polling thread
    m_polling.store(true);
    m_pollThread = std::thread(&ServiceProxy::PollLoop, this);

    LOG_INFO("App", "ServiceProxy::Connect", "IPC",
             "Connected to EGoTouchService.");
    return true;
}

void ServiceProxy::Disconnect() {
    // Stop polling
    m_polling.store(false);
    if (m_pollThread.joinable()) m_pollThread.join();

    // Tell Service to exit debug mode
    if (m_client.IsConnected()) {
        m_client.ExitDebugMode();
        m_client.Disconnect();
    }
    m_frameReader.Close();
    m_configDirty.Close();
    m_fps.store(0);
    LOG_INFO("App", "ServiceProxy::Disconnect", "IPC",
             "Disconnected.");
}

// ── Auto-discovery ──
void ServiceProxy::StartAutoDiscovery(int intervalMs) {
    if (m_discovering.load()) return;
    m_discoveryIntervalMs = intervalMs;
    m_discovering.store(true);
    m_discoveryThread = std::thread(&ServiceProxy::DiscoveryLoop, this);
    LOG_INFO("App", "ServiceProxy::StartAutoDiscovery", "IPC",
             "Auto-discovery started (interval={}ms).", intervalMs);
}

void ServiceProxy::StopAutoDiscovery() {
    m_discovering.store(false);
    if (m_discoveryThread.joinable()) m_discoveryThread.join();
}

bool ServiceProxy::TryConnect() {
    if (IsConnected()) return true;
    return Connect();
}

void ServiceProxy::DiscoveryLoop() {
    while (m_discovering.load()) {
        if (!IsConnected()) {
            if (Connect()) {
                LOG_INFO("App", "ServiceProxy::DiscoveryLoop", "IPC",
                         "Service discovered and connected.");
            }
        }
        // Sleep in small increments so we can stop quickly
        for (int i = 0; i < m_discoveryIntervalMs / 100 && m_discovering.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

bool ServiceProxy::IsConnected() const {
    return m_client.IsConnected();
}

bool ServiceProxy::GetLatestFrame(Engine::HeatmapFrame& out) {
    if (!m_hasNewFrame.load()) return false;
    std::lock_guard<std::mutex> lk(m_frameMutex);
    out = m_latestFrame;
    m_hasNewFrame.store(false);
    return true;
}

bool ServiceProxy::SwitchAfeMode(uint8_t afeCmd, uint8_t param) {
    auto resp = m_client.SendAfeCommand(afeCmd, param);
    return resp.success;
}

bool ServiceProxy::StartRemoteRuntime() {
    return m_client.StartRuntime().success;
}

bool ServiceProxy::StopRemoteRuntime() {
    return m_client.StopRuntime().success;
}

void ServiceProxy::SaveConfig() {
    std::ofstream out("config.ini");
    if (!out.is_open()) return;
    for (auto& p : m_pipeline.GetProcessors()) {
        out << "[" << p->GetName() << "]\n";
        p->SaveConfig(out);
        out << "\n";
    }
    out.close();
    // Notify Service to reload from config.ini
    m_configDirty.SetDirty();
    m_client.ReloadConfig();
    LOG_INFO("App", "ServiceProxy::SaveConfig", "Config",
             "Config saved and Service notified to reload.");
}

void ServiceProxy::LoadConfig() {
    std::ifstream in("config.ini");
    if (!in.is_open()) return;
    std::string line, section;
    Engine::IFrameProcessor* cur = nullptr;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            cur = nullptr;
            for (auto& p : m_pipeline.GetProcessors()) {
                if (p->GetName() == section) {
                    cur = p.get(); break;
                }
            }
        } else if (cur) {
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                cur->LoadConfig(line.substr(0, eq),
                                line.substr(eq + 1));
            }
        }
    }
}

void ServiceProxy::NotifyConfigDirty() {
    m_configDirty.SetDirty();
}

// ── VHF control ──
bool ServiceProxy::SetVhfEnabled(bool enabled) {
    Ipc::IpcRequest req{};
    req.command = Ipc::IpcCommand::SetVhfEnabled;
    req.param[0] = enabled ? 1 : 0; req.paramLen = 1;
    bool ok = m_client.Send(req).success;
    if (ok) m_vhfEnabled.store(enabled);
    return ok;
}

bool ServiceProxy::SetVhfTranspose(bool enabled) {
    Ipc::IpcRequest req{};
    req.command = Ipc::IpcCommand::SetVhfTranspose;
    req.param[0] = enabled ? 1 : 0; req.paramLen = 1;
    bool ok = m_client.Send(req).success;
    if (ok) m_vhfTranspose.store(enabled);
    return ok;
}

bool ServiceProxy::SetAutoAfeSync(bool enabled) {
    Ipc::IpcRequest req{};
    req.command = Ipc::IpcCommand::SetAutoAfeSync;
    req.param[0] = enabled ? 1 : 0; req.paramLen = 1;
    bool ok = m_client.Send(req).success;
    if (ok) m_autoAfeSync.store(enabled);
    return ok;
}

// ── MasterParser-only mode (local) ──
void ServiceProxy::SetMasterParserOnlyMode(bool enabled) {
    auto& procs = m_pipeline.GetProcessors();
    if (enabled && !m_masterParserOnly) {
        m_savedProcessorStates.clear();
        for (size_t i = 0; i < procs.size(); ++i) {
            m_savedProcessorStates.push_back(procs[i]->IsEnabled());
            if (i > 0) procs[i]->SetEnabled(false);
        }
    } else if (!enabled && m_masterParserOnly) {
        for (size_t i = 0; i < procs.size() &&
             i < m_savedProcessorStates.size(); ++i) {
            procs[i]->SetEnabled(m_savedProcessorStates[i]);
        }
    }
    m_masterParserOnly = enabled;
}

// ── DVR export (local) ──
void ServiceProxy::TriggerDVRExport(bool heatmap, bool master, bool slave) {
    if (!m_dvrBuffer) return;
    namespace fs = std::filesystem;
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{}; localtime_s(&tm, &t);
    std::ostringstream ts;
    ts << std::put_time(&tm, "%Y%m%d_%H%M%S");
    std::string dir = "dvr_" + ts.str();
    fs::create_directories(dir);
    // Snapshot frames from ring buffer
    auto frames = m_dvrBuffer->GetSnapshot();
    for (size_t i = 0; i < frames.size(); ++i) {
        if (heatmap) {
            std::ofstream f(dir + "/frame_" + std::to_string(i) + ".csv");
            for (int r = 0; r < 40; ++r) {
                for (int c = 0; c < 60; ++c) {
                    if (c) f << ',';
                    f << frames[i].heatmapMatrix[r][c];
                }
                f << '\n';
            }
        }
    }
    LOG_INFO("App", "ServiceProxy::TriggerDVRExport", "DVR",
             "Exported {} frames to {}/", frames.size(), dir);
}

// ── Poll loop with FPS measurement ──
void ServiceProxy::PollLoop() {
    m_latestFrame.rawData.reserve(5402);

    uint64_t lastFpsFrameId = m_frameReader.LastFrameId();
    auto lastFpsTick = std::chrono::steady_clock::now();
    auto lastLogPoll = std::chrono::steady_clock::now();
    while (m_polling.load()) {
        bool gotFrame = false;
        {
            std::lock_guard<std::mutex> lk(m_frameMutex);
            if (m_frameReader.Read(m_latestFrame)) {
                m_hasNewFrame.store(true, std::memory_order_release);
                gotFrame = true;
            }
        }
        if (gotFrame) {
            if (m_dvrBuffer) {
                m_dvrBuffer->PushOverwriting(m_latestFrame);
            }
        }
        auto now = std::chrono::steady_clock::now();
        // FPS counter
        auto fpsElapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(now - lastFpsTick);
        if (fpsElapsed.count() >= 1000) {
            uint64_t currentId = m_frameReader.LastFrameId();
            m_fps.store(static_cast<int>(currentId - lastFpsFrameId));
            lastFpsFrameId = currentId;
            lastFpsTick = now;
        }
        // Service log polling (~every 1s)
        auto logElapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(now - lastLogPoll);
        if (logElapsed.count() >= 1000 && m_client.IsConnected()) {
            Ipc::IpcRequest req{};
            req.command = Ipc::IpcCommand::GetLogs;
            auto resp = m_client.Send(req);
            if (resp.success && resp.dataLen > 0) {
                std::string packed(
                    reinterpret_cast<const char*>(resp.data), resp.dataLen);
                std::istringstream iss(packed);
                std::string line;
                while (std::getline(iss, line)) {
                    if (!line.empty()) {
                        LOG_INFO("Service", "RemoteLog", "IPC", "{}", line);
                    }
                }
            }
            lastLogPoll = now;
        }
        if (!gotFrame) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

} // namespace App
