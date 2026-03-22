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