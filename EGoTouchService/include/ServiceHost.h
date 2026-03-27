#pragma once

#include "SystemStateMonitor.h"
#include "runtime/DeviceRuntime.h"
#include "IpcPipeServer.h"
#include "SharedFrameBuffer.h"
#include "ConfigSync.h"
#include <memory>

namespace Service {

/// 模块加载器：负责创建、连接、启停所有子模块。
/// 不知道 SCM 的存在，可以独立测试。
class ServiceHost {
public:
    ServiceHost() = default;
    ~ServiceHost();

    ServiceHost(const ServiceHost&) = delete;
    ServiceHost& operator=(const ServiceHost&) = delete;

    /// 按依赖顺序启动所有模块
    bool Start();

    /// 逆序停止所有模块
    void Stop();

private:
    std::unique_ptr<DeviceRuntime> m_deviceRuntime;
    std::unique_ptr<Host::SystemStateMonitor> m_sysMonitor;

    // IPC
    Ipc::IpcPipeServer      m_ipcServer;
    Ipc::SharedFrameWriter  m_frameWriter;
    Ipc::ConfigDirtyFlag    m_configDirty;
    bool                    m_debugMode = false;

    void BuildDefaultPipeline();
    Ipc::IpcResponse HandleIpcCommand(const Ipc::IpcRequest& req);
};

} // namespace Service
