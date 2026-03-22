#pragma once

#include "himax/HimaxChip.h"
#include "btmcu/PenCommandApi.h"
#include "ConcurrentRingBuffer.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <array>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <chrono>

// Engine Includes
#include "EngineTypes.h"
#include "FramePipeline.h"

namespace App {

class RuntimeOrchestrator {
public:
    RuntimeOrchestrator();
    ~RuntimeOrchestrator();

    bool Start();
    void Stop();
    
    // GUI 交互接口，用于手动控制芯片
    Himax::Chip* GetDevice() { return m_device.get(); }
    ChipResult<> SwitchAfeMode(AFE_Command cmd, uint8_t param = 0);
    
    // 注入供 GUI 使用的最新热力图引用
    bool GetLatestFrame(Engine::HeatmapFrame& outFrame);

    // 获取数据处理管线，用于 GUI 动态配置
    Engine::FramePipeline& GetPipeline() { return m_pipeline; }

    // 数据采集循环控制
    void SetAcquisitionActive(bool active) { m_isAcquiring.store(active); }
    bool IsAcquisitionActive() const { return m_isAcquiring.load(); }

    // 自动将算法频移请求同步到 AFE 命令。
    void SetAutoAfeFreqShiftSyncEnabled(bool enabled) { m_autoAfeFreqShiftSyncEnabled.store(enabled); }
    bool IsAutoAfeFreqShiftSyncEnabled() const { return m_autoAfeFreqShiftSyncEnabled.load(); }

    // VHF/HID injector report output.
    void SetVhfReportingEnabled(bool enabled);
    bool IsVhfReportingEnabled() const { return m_vhfReportingEnabled.load(); }
    void SetVhfTransposeEnabled(bool enabled) { m_vhfTransposeEnabled.store(enabled); }
    bool IsVhfTransposeEnabled() const { return m_vhfTransposeEnabled.load(); }
    bool IsVhfDeviceOpen() const;

    // 算法压测辅助：仅保留 MasterFrameParser，其余模块全部禁用
    void SetMasterParserOnlyMode(bool enabled);
    bool IsMasterParserOnlyMode() const { return m_masterParserOnlyMode; }
    
    // 全局参数配置加载与保存
    void SaveConfig();
    void LoadConfig();

    /**
     * @brief 触发回放数据导出 (Replay/DVR Export)
     * 
     * 该功能实现“时间溯源”式的回放：
    * 系统在后台自动维护一个环形缓冲区（Rolling Buffer），循环记录最新的 480 帧（约 4 秒历史）。
     * 当用户发现特定异常场景时（如：断线、跳动），点击导出按钮可将缓存中的原始热力图及
     * 算法处理后的坐标点完整序列快照导出为 CSV 文件，用于离线算法回放与精度复现。
     */
    void TriggerDVRExport(bool exportHeatmap, bool exportMasterStatus, bool exportSlaveStatus);

private:
    void AcquisitionThreadFunc();
    void ProcessingThreadFunc();
    void SystemStateThreadFunc();
    void InitializePenBridge();
    void ShutdownPenBridge();
    std::optional<std::wstring> FindPenMcuDevicePath() const;
    void OnPenEvent(const Himax::Pen::PenEvent& event);
    static uint8_t MapPenEventToAck(uint8_t eventCode, bool* outKnown = nullptr);
    void ApplyVhfStylusPostTransform(std::array<uint8_t, 13>& packetBytes) const;
    void HandleAutoAfeFreqShiftSync(const Engine::StylusFrameData& stylus);
    void BuildTouchVhfReports(Engine::HeatmapFrame& frame) const;
    void DispatchVhfReports(Engine::HeatmapFrame& frame);
    void ResetAutoAfeFreqShiftSyncState();
    bool EnsureVhfDeviceOpenLocked();
    void CloseVhfDeviceLocked();
    void ReopenVhfDeviceLocked();
    bool WriteVhfPacketLocked(const uint8_t* data, size_t length, const char* tag);

private:
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_isAcquiring{false};
    
    // Modules
    std::unique_ptr<Himax::Chip> m_device;
    
    // Threads
    std::thread m_acquisitionThread;
    std::thread m_processingThread;
    std::thread m_systemStateThread;

    // Engine Pipeline
    Engine::FramePipeline m_pipeline;

    // Data flow
    RingBuffer<Engine::HeatmapFrame, 16> m_frameBuffer;
    
    // GUI needs the latest frame synchronously
    std::mutex m_latestFrameMutex;
    Engine::HeatmapFrame m_latestFrame;

    // Time Backtrack (DVR) rolling buffer
    // NOTE: Keep this on heap because HeatmapFrame is large and 480-capacity
    // ring buffer can otherwise make RuntimeOrchestrator too large for stack allocation.
    std::unique_ptr<RingBuffer<Engine::HeatmapFrame, 480>> m_dvrBuffer;

    // MasterParser-only mode state
    bool m_masterParserOnlyMode = false;
    std::vector<bool> m_savedProcessorEnabledStates;

    // Algorithm -> AFE frequency-shift bridge (minimal closed-loop).
    std::atomic<bool> m_autoAfeFreqShiftSyncEnabled{true};
    bool m_autoAfeFreqShiftEnabledSent = false;
    bool m_autoAfeFreqPairInitialized = false;
    uint16_t m_autoAfeFreqIdx0Tx1 = 0;
    uint16_t m_autoAfeFreqIdx0Tx2 = 0;
    int m_autoAfeLastForcedFreqIdx = -1;
    std::chrono::steady_clock::time_point m_autoAfeLastForceTs{};
    int m_autoAfeForceCooldownMs = 80;

    // BT MCU pen event bridge (mirrors key THP_Service state needed by VHF pen packet).
    std::unique_ptr<Himax::Pen::PenCommandApi> m_penCommandApi;
    std::atomic<uint8_t> m_penEraserToggleState{0};
    std::atomic<uint8_t> m_penLastEventCode{0};

    // VHF/HID injector state.
    std::atomic<bool> m_vhfReportingEnabled{true};
    std::atomic<bool> m_vhfTransposeEnabled{false};
    mutable std::mutex m_vhfMutex;
    HANDLE m_vhfDeviceHandle = INVALID_HANDLE_VALUE;
};

} // namespace App
