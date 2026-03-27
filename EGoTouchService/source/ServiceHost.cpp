#include "ServiceHost.h"
#include "Logger.h"

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

    // ── 3. TODO: ControlPipeServer ──────────────────────────
    // m_pipeServer = ...

    LOG_INFO("ServiceHost", "Start", "Boot",
             "All modules started.");
    return true;
}

void ServiceHost::Stop() {
    // 逆序停止（后启动的先停止）

    // TODO: ControlPipeServer
    // if (m_pipeServer) m_pipeServer->Stop();

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
}

} // namespace Service
