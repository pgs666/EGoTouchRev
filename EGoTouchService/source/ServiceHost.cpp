#include "ServiceHost.h"
#include "GuiLogSink.h"
#include "Logger.h"
#include "IpcProtocol.h"
#include "IFrameProcessor.h"
#include <fstream>
#include <string>

// Engine Pipeline Processors
#include "MasterFrameParser.h"
#include "BaselineSubtraction.h"
#include "CMFProcessor.h"
#include "GridIIRProcessor.h"
#include "FeatureExtractor.h"
#include "StylusProcessor.h"
#include "TouchTracker.h"
#include "CoordinateFilter.h"
#include "TouchGestureStateMachine.h"

namespace Service {

// ── 设备路径（与 App/RuntimeOrchestrator 共用同一组） ──
static const std::wstring kDevicePathMaster    = L"\\\\.\\Global\\SPBTESTTOOL_MASTER";
static const std::wstring kDevicePathSlave     = L"\\\\.\\Global\\SPBTESTTOOL_SLAVE";
static const std::wstring kDevicePathInterrupt = L"\\\\.\\Global\\SPBTESTTOOL_MASTER";

ServiceHost::~ServiceHost() {
    Stop();
}

bool ServiceHost::Start() {
    // ── 1. DeviceRuntime（先创建，后续模块依赖它） ─────────
    m_deviceRuntime = std::make_unique<DeviceRuntime>(
        kDevicePathMaster, kDevicePathSlave, kDevicePathInterrupt);
    m_deviceRuntime->SetAutoMode(true);
    BuildDefaultPipeline();

    if (!m_deviceRuntime->Start()) {
        LOG_ERROR("ServiceHost", "Start", "Boot",
                  "DeviceRuntime::Start() failed.");
        return false;
    }
    LOG_INFO("ServiceHost", "Start", "Boot",
             "DeviceRuntime started (auto mode).");

    // ── 2. SystemStateMonitor（事件 → DeviceRuntime 命令队列） ─
    m_sysMonitor = std::make_unique<Host::SystemStateMonitor>();
    bool monitorOk = m_sysMonitor->Start(
        [this](const Host::SystemStateEvent& ev) {
            LOG_INFO("ServiceHost", "SystemEventCb", "Event",
                     "System event: type={}",
                     Host::ToString(ev.type));
            m_deviceRuntime->IngestSystemEvent(ev);
        });

    if (!monitorOk) {
        LOG_WARN("ServiceHost", "Start", "Monitor",
                 "SystemStateMonitor failed to start; "
                 "running without system state detection.");
        m_sysMonitor.reset();
    } else {
        LOG_INFO("ServiceHost", "Start", "Monitor",
                 "SystemStateMonitor started.");
    }

    // ── 3. IPC Pipe Server ──────────────────────────────────
    m_configDirty.Open();
    m_ipcServer.SetCommandHandler(
        [this](const Ipc::IpcRequest& req) {
            return HandleIpcCommand(req);
        });
    m_ipcServer.Start();
    LOG_INFO("ServiceHost", "Start", "Boot",
             "IPC pipe server started.");

    LOG_INFO("ServiceHost", "Start", "Boot",
             "All modules started.");
    return true;
}

void ServiceHost::Stop() {
    // 逆序停止（后启动的先停止）

    m_ipcServer.Stop();
    m_frameWriter.Close();
    m_configDirty.Close();
    m_debugMode = false;

    if (m_sysMonitor) {
        m_sysMonitor->Stop();
        m_sysMonitor.reset();
        LOG_INFO("ServiceHost", "Stop", "Monitor",
                 "SystemStateMonitor stopped.");
    }

    if (m_deviceRuntime) {
        m_deviceRuntime->Stop();
        m_deviceRuntime.reset();
        LOG_INFO("ServiceHost", "Stop", "Device",
                 "DeviceRuntime stopped.");
    }

    LOG_INFO("ServiceHost", "Stop", "Shutdown",
             "All modules stopped.");
}

// ── Pipeline 构建 ──────────────────────────────
void ServiceHost::BuildDefaultPipeline() {
    auto& pl = m_deviceRuntime->GetPipeline();
    pl.AddProcessor(std::make_unique<Engine::MasterFrameParser>());
    pl.AddProcessor(std::make_unique<Engine::BaselineSubtraction>());
    pl.AddProcessor(std::make_unique<Engine::CMFProcessor>());
    pl.AddProcessor(std::make_unique<Engine::GridIIRProcessor>());
    pl.AddProcessor(std::make_unique<Engine::FeatureExtractor>());
    pl.AddProcessor(std::make_unique<Engine::StylusProcessor>());
    pl.AddProcessor(std::make_unique<Engine::TouchTracker>());
    pl.AddProcessor(std::make_unique<Engine::CoordinateFilter>());
    pl.AddProcessor(std::make_unique<Engine::TouchGestureStateMachine>());
    LOG_INFO("ServiceHost", "BuildDefaultPipeline", "Boot",
             "Registered {} pipeline processors.",
             pl.GetProcessors().size());

    // Load saved config from config.ini
    std::ifstream cfgIn("config.ini");
    if (cfgIn.is_open()) {
        std::string line, section;
        Engine::IFrameProcessor* cur = nullptr;
        while (std::getline(cfgIn, line)) {
            if (line.empty() || line[0] == ';') continue;
            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                cur = nullptr;
                for (auto& p : pl.GetProcessors()) {
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
        LOG_INFO("ServiceHost", "BuildDefaultPipeline", "Boot",
                 "Loaded config from config.ini.");
    }
}

// ── IPC Command Handler ──────────────────────────────
Ipc::IpcResponse ServiceHost::HandleIpcCommand(
        const Ipc::IpcRequest& req) {
    Ipc::IpcResponse resp{};
    switch (req.command) {
    case Ipc::IpcCommand::Ping:
        resp.success = true;
        break;

    case Ipc::IpcCommand::EnterDebugMode: {
        // param contains wchar_t shared memory name
        const wchar_t* shmName =
            reinterpret_cast<const wchar_t*>(req.param);
        if (m_frameWriter.Open(shmName)) {
            m_deviceRuntime->SetFramePushCallback(
                [this](const Engine::HeatmapFrame& f) {
                    m_frameWriter.Write(f);
                });
            m_debugMode = true;
            resp.success = true;
            LOG_INFO("ServiceHost", "HandleIpcCommand", "IPC",
                     "Entered debug mode.");
        }
        break;
    }

    case Ipc::IpcCommand::ExitDebugMode:
        m_deviceRuntime->SetFramePushCallback(nullptr);
        m_frameWriter.Close();
        m_debugMode = false;
        resp.success = true;
        LOG_INFO("ServiceHost", "HandleIpcCommand", "IPC",
                 "Exited debug mode.");
        break;

    case Ipc::IpcCommand::AfeCommand:
        if (req.paramLen >= 2 && m_deviceRuntime) {
            command cmd{};
            cmd.type = static_cast<AFE_Command>(req.param[0]);
            cmd.param = req.param[1];
            m_deviceRuntime->SubmitCommand(
                cmd, CommandSource::External, "IPC AFE");
            resp.success = true;
        }
        break;

    case Ipc::IpcCommand::StartRuntime:
        if (m_deviceRuntime) {
            resp.success = m_deviceRuntime->Start();
        }
        break;

    case Ipc::IpcCommand::StopRuntime:
        if (m_deviceRuntime) {
            m_deviceRuntime->Stop();
            resp.success = true;
        }
        break;

    case Ipc::IpcCommand::ReloadConfig:
        if (m_deviceRuntime) {
            auto& pl = m_deviceRuntime->GetPipeline();
            std::ifstream in("config.ini");
            if (in.is_open()) {
                std::string line, section;
                Engine::IFrameProcessor* cur = nullptr;
                while (std::getline(in, line)) {
                    if (line.empty() || line[0] == ';') continue;
                    if (line.front() == '[' && line.back() == ']') {
                        section = line.substr(1, line.size() - 2);
                        cur = nullptr;
                        for (auto& p : pl.GetProcessors()) {
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
                resp.success = true;
                LOG_INFO("ServiceHost", "HandleIpcCommand", "IPC",
                         "Config reloaded from config.ini.");
            }
        }
        break;

    case Ipc::IpcCommand::SaveConfig:
        if (m_deviceRuntime) {
            auto& pl = m_deviceRuntime->GetPipeline();
            std::ofstream out("config.ini");
            if (out.is_open()) {
                for (auto& p : pl.GetProcessors()) {
                    out << "[" << p->GetName() << "]\n";
                    p->SaveConfig(out);
                    out << "\n";
                }
                resp.success = true;
                LOG_INFO("ServiceHost", "HandleIpcCommand", "IPC",
                         "Config saved to config.ini.");
            }
        }
        break;

    case Ipc::IpcCommand::SetVhfEnabled:
        if (m_deviceRuntime && req.paramLen >= 1) {
            m_deviceRuntime->GetVhfReporter().SetEnabled(req.param[0] != 0);
            resp.success = true;
        }
        break;

    case Ipc::IpcCommand::SetVhfTranspose:
        if (m_deviceRuntime && req.paramLen >= 1) {
            m_deviceRuntime->GetVhfReporter().SetTransposeEnabled(req.param[0] != 0);
            resp.success = true;
        }
        break;

    case Ipc::IpcCommand::SetAutoAfeSync:
        resp.success = true; // placeholder — future DeviceRuntime integration
        break;

    case Ipc::IpcCommand::GetLogs: {
        auto lines = Common::GuiLogSink::Instance()->DrainNewLines();
        std::string packed;
        for (const auto& l : lines) {
            if (packed.size() + l.size() + 1 > sizeof(resp.data)) break;
            packed += l;
            packed += '\n';
        }
        resp.dataLen = static_cast<uint16_t>(
            std::min(packed.size(), sizeof(resp.data)));
        std::memcpy(resp.data, packed.data(), resp.dataLen);
        resp.success = true;
        break;
    }

    default:
        break;
    }
    return resp;
}

} // namespace Service
