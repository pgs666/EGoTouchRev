#pragma once

#include "SystemStateMonitor.h"
#include "runtime/DeviceRuntime.h"
#include "btmcu/PenBridge.h"
#include "IpcPipeServer.h"
#include "SharedFrameBuffer.h"
#include "ConfigSync.h"
#include <memory>
#include <string>

namespace Service {

/// 服务运行模式
enum class ServiceMode {
    Full,       ///< 触摸 + 手写笔（PenBridge + StylusPipeline 全部启用）
    TouchOnly,  ///< 仅触摸（跳过 PenBridge / StylusPipeline）
};

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

    ServiceMode GetMode() const { return m_mode; }

private:
    ServiceMode m_mode = ServiceMode::TouchOnly;

    std::unique_ptr<DeviceRuntime>              m_deviceRuntime;
    std::unique_ptr<Host::SystemStateMonitor>   m_sysMonitor;
    // BT MCU 双通道桥接（事件线程 + 压力线程，各独立）—— 仅 Full 模式启用
    std::unique_ptr<Himax::Pen::PenBridge>      m_penBridge;

    // IPC
    Ipc::IpcPipeServer      m_ipcServer;
    Ipc::SharedFrameWriter  m_frameWriter;
    Ipc::ConfigDirtyFlag    m_configDirty;
    bool                    m_debugMode = false;

    ServiceMode ParseServiceMode(const std::string& configPath) const;
    void BuildDefaultPipeline(const std::string& configPath);
    Ipc::IpcResponse HandleIpcCommand(const Ipc::IpcRequest& req);
};

} // namespace Service
