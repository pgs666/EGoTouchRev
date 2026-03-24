#include "ServiceHost.h"
#include "Logger.h"

namespace Service {

ServiceHost::~ServiceHost() {
    Stop();
}

bool ServiceHost::Start() {
    // ── 1. 系统状态监听模块 ──────────────────
    m_sysMonitor = std::make_unique<Host::SystemStateMonitor>();
    bool monitorOk = m_sysMonitor->Start(
        [](const Host::SystemStateEvent& ev) {
            // 当前仅记录日志；后续接入 DeviceRuntime 命令队列
            LOG_INFO("ServiceHost", "SystemEventCb", "Event",
                     "Received system event: type={}",
                     Host::ToString(ev.type));
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

    // ── 2. TODO: DeviceRuntime ───────────────
    // m_deviceRuntime = std::make_unique<DeviceRuntime>();
    // if (!m_deviceRuntime->Start()) return false;

    // ── 3. TODO: TouchEngine ─────────────────
    // m_touchEngine = std::make_unique<TouchEngine>(
    //     m_deviceRuntime->GetRingBuffer());
    // if (!m_touchEngine->Start()) return false;

    LOG_INFO("ServiceHost", "Start", "Boot",
             "All modules started.");
    return true;
}

void ServiceHost::Stop() {
    // 逆序停止（后启动的先停止）

    // TODO: TouchEngine
    // if (m_touchEngine)   m_touchEngine->Stop();

    // TODO: DeviceRuntime
    // if (m_deviceRuntime) m_deviceRuntime->Stop();

    if (m_sysMonitor) {
        m_sysMonitor->Stop();
        m_sysMonitor.reset();
        LOG_INFO("ServiceHost", "Stop", "Monitor",
                 "SystemStateMonitor stopped.");
    }

    LOG_INFO("ServiceHost", "Stop", "Shutdown",
             "All modules stopped.");
}

} // namespace Service
