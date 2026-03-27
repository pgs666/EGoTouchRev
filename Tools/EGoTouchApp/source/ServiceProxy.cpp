#include "ServiceProxy.h"
#include "Logger.h"
#include "IFrameProcessor.h"
#include "IpcProtocol.h"
#include <chrono>
#include <fstream>
#include <string>
#include <iomanip>
#include <sstream>
#include <filesystem>

namespace App {

ServiceProxy::ServiceProxy()
    : m_dvrBuffer(std::make_unique<RingBuffer<Engine::HeatmapFrame, 480>>()) {}

ServiceProxy::~ServiceProxy() {
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
    LOG_INFO("App", "ServiceProxy::Disconnect", "IPC",
             "Disconnected.");
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
    m_configDirty.SetDirty();
    m_client.SaveConfig();
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
    Engine::HeatmapFrame tempFrame;
    int frameCount = 0;
    auto lastFpsTick = std::chrono::steady_clock::now();
    while (m_polling.load()) {
        if (m_frameReader.Read(tempFrame)) {
            // DVR recording
            if (m_dvrBuffer) m_dvrBuffer->PushOverwriting(tempFrame);
            {
                std::lock_guard<std::mutex> lk(m_frameMutex);
                m_latestFrame = tempFrame;
                m_hasNewFrame.store(true);
            }
            ++frameCount;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<
                std::chrono::milliseconds>(now - lastFpsTick);
            if (elapsed.count() >= 1000) {
                m_fps.store(frameCount);
                frameCount = 0;
                lastFpsTick = now;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
}

} // namespace App
