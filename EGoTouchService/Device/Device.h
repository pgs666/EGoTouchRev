#pragma once
#include <expected>

enum class ChipError {
    Success = 0,
    CommunicationError,  // 底层驱动/总线通讯故障 (Handle, IOCTL, Read/Write)
    Timeout,             // 操作超时 (Interrupt, Reloading, Status polling)
    VerificationFailed,  // 逻辑校验失败 (Verify failed, FW Status error)
    InvalidOperation,    // 调用时机或参数非法 (Invalid param, State error, Not ready)
    InternalError        // 内部逻辑异常
};

// AFE 命令类型
enum class AFE_Command : uint8_t {
    ClearStatus = 0,       // 清除状态
    EnableFreqShift,       // 启用频移
    DisableFreqShift,      // 禁用频移
    StartCalibration,      // 开始校准
    EnterIdle,             // 进入空闲
    ForceExitIdle,         // 强制退出空闲
    ForceToFreqPoint,      // 强制切换到指定频点
    ForceToScanRate,       // 强制切换到指定扫描率
    InitStylus,            // 手写笔连接初始化（EnableFreqShift + 绑定笔频率对）
    DisconnectStylus,      // 手写笔断连清理（DisableFreqShift + 重置状态）
};

enum class THP_AFE_MODE {
    Normal = 200,
    Idle = 500,
    Turbo = 0,
};

// 命令结构体
struct command {
    AFE_Command type;
    uint8_t param = 0;
};

// 错误处理别名
template <typename T = void>
using ChipResult = std::expected<T, ChipError>;

// ── 手写笔频率命令对 ─────────────────────────────────────────────────────────
// 每支笔在芯片频率表中有两个频点命令（由 set_stylus_id 时绑定）
struct StylusFreqPair {
    uint16_t freq0_cmd = 0;   // 频点0命令（标准频率，默认扫描）
    uint16_t freq1_cmd = 0;   // 频点1命令（240Hz 高频扫描）
};

// ── 用户态驱动侧手写笔状态 ────────────────────────────────────────────────────
// 由 Chip::InitStylus / DisconnectStylus 维护，供 ProcessStylusStatus 使用
struct StylusState {
    bool           connected        = false; // 手写笔已通过 BTMCU 连接
    uint8_t        pen_id           = 0;     // 注册的笔 ID（0-3 主表，5=默认）
    StylusFreqPair freqPair;                 // 当前笔的频率命令对
    uint8_t        freqIdx          = 0;     // 当前使用的频点索引（0 或 1）
    uint8_t        switchPolicy     = 0;     // 0=禁用自动切换, 2=启用噪声触发切换
    uint8_t        switchTargetIdx  = 0;     // 目标频点（噪声判断后写入）
    bool           switchReqPending = false; // 切换请求待芯片 IPC 0xBA 确认
};