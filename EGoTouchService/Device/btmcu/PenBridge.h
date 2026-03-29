#pragma once

#include <Windows.h>
#include "btmcu/PenUsbTransport.h"
#include "btmcu/PenUsbTypes.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace Himax::Pen {

/// PenBridge — BT MCU 双通道桥接器
///
/// 封装了 BtMcuTestTool 中所有经验证的协议逻辑：
///   - 事件通道 (col00)：自动设备发现 + 握手(0x7101/0x7701) + ACK + 0x7D01 回显
///   - 压力通道 (col01)：自动设备发现 + 连续读取 + 'U' 报文解析
///
/// 线程模型：
///   Start() → 新建 m_evtThread（事件读取 + ACK 响应，阻塞 ReadPacket）
///            → 新建 m_pressThread（压力数据读取，阻塞 ReadPacket）
///   Stop()  → 两个线程均 join()
///
/// 生命周期：
///   PenBridge bridge;
///   bridge.SetEventCallback([](const PenEvent& ev){ ... });
///   bridge.Start();   // 非阻塞，内部新开两条线程
///   ...
///   bridge.Stop();    // 阻塞直到两条线程退出

struct PenPressureStats {
    uint16_t press[4] = {0, 0, 0, 0};  ///< 四通道压力值
    uint8_t  freq1    = 0;              ///< BT 跳频频率1
    uint8_t  freq2    = 0;              ///< BT 跳频频率2
    uint8_t  reportType = 0;            ///< 报文类型（通常 'U' = 0x55）
};

class PenBridge {
public:
    PenBridge();
    ~PenBridge();

    PenBridge(const PenBridge&)            = delete;
    PenBridge& operator=(const PenBridge&) = delete;

    /// 启动 BT MCU 双通道（各新建一条专用线程）。
    /// 若设备未就绪，线程内部周期性重试直到 Stop() 被调用。
    void Start();

    /// 停止所有线程并关闭设备。阻塞直到线程退出。
    void Stop();

    bool IsRunning() const { return m_running.load(); }

    /// 设置 MCU 事件回调（线程安全，可在 Start 之前或之后调用）。
    /// 回调从事件读取线程发起，不得长时间阻塞。
    void SetEventCallback(PenEventCallback cb);

    /// 设置压感回调（每收到一个 'U' 报文触发，param = press[0] 即第一通道压感值）。
    using PressureCallback = std::function<void(uint16_t press)>;
    void SetPressureCallback(PressureCallback cb);

    /// 获取最新压力统计（原子读，线程安全）。
    PenPressureStats GetPressureStats() const;

    /// 手动触发握手（0x7101 + 0x7701），通常无需手动调用。
    void RunHandshake();

private:
    // ── 内部常量 ────────────────────────────────────────────────
    // 事件通道：THP_Service 注册的设备接口 GUID
    static const GUID kEventDeviceGuid;
    // 压力通道：VID/PID/MI/col 特征串
    static constexpr const wchar_t* kPressureHidMatch = L"vid_12d1&pid_10b8&mi_00&col01";

    // ── 设备发现 ────────────────────────────────────────────────
    static std::optional<std::wstring> FindEventDevicePath();
    static std::optional<std::wstring> FindPressureDevicePath();

    // ── 协议辅助 ────────────────────────────────────────────────
    static int GetAckCode(uint8_t eventCode);
    void SendRawPacket(const std::vector<uint8_t>& pkt);
    void SendAck(uint8_t ackCode);
    void SendInitParamEcho(const uint8_t* data, size_t len);
    void DispatchEvent(const std::vector<uint8_t>& rxBuf);

    // ── 线程函数 ────────────────────────────────────────────────
    void EventThreadFunc();    ///< 专用线程：事件通道读取 + ACK + 握手
    void PressureThreadFunc(); ///< 专用线程：压力通道连续读取

    // ── 成员 ────────────────────────────────────────────────────
    std::atomic<bool> m_running{false};

    std::unique_ptr<IPenUsbTransport> m_evtTransport;
    std::unique_ptr<IPenUsbTransport> m_pressTransport;

    std::thread m_evtThread;
    std::thread m_pressThread;

    mutable std::mutex    m_cbMutex;
    PenEventCallback      m_eventCallback;
    PressureCallback      m_pressureCallback;

    mutable std::mutex    m_statsMutex;
    PenPressureStats      m_stats;
};

} // namespace Himax::Pen
