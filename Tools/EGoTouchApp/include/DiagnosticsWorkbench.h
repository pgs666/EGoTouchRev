#pragma once

#include "ServiceProxy.h"
#include <string>

namespace App {

class DiagnosticsWorkbench {
public:
    // 传入 ServiceProxy 指针以调用远程功能并获取最新热力图序列
    explicit DiagnosticsWorkbench(ServiceProxy* proxy);
    ~DiagnosticsWorkbench();

    // 在主循环中每帧调用此函数以绘制 ImGui 界面
    void Render();

private:
    void DrawControlPanel();
    void DrawTouchSolverPanel();
    void DrawTouchTrackingPanel();
    void DrawStylusControlPanel();
    void DrawHeatmap();
    void DrawCoordinateTable();
    void DrawStylusPanel();
    void DrawMasterSuffixTable();
    void DrawSlaveSuffixTable();

    void ExportCurrentFrameToCSV(bool isAutoCapture = false);

private:
    ServiceProxy* m_proxy;
    
    // 缓存的最新的热力图数据
    Engine::HeatmapFrame m_currentFrame;
    
    // GUI 内部状态
    bool m_autoRefresh = true;
    bool m_renderVisualization = true;
    bool m_showTouchDebugPanel = true;
    bool m_showStylusDebugPanel = true;
    bool m_showTouchSolverPanel = true;
    bool m_showTouchTrackingPanel = true;
    bool m_showStylusControlPanel = false;
    bool m_showMasterSuffixTable = true;
    bool m_showSlaveSuffixTable = true;
    bool m_fullscreen = false;
    int m_heatmapScale = 10;
    float m_colorRange = 1000.0f;

    // Export Options
    bool m_exportHeatmap = true;
    bool m_exportMasterStatus = false;
    bool m_exportSlaveStatus = false;
    
    int m_autoExportTargetPeaks = 0; // 0 = disabled, N = save frame when N fingers detected
    int m_lastPeakCount = 0;

    // AFE 手动控制参数（统一走 ServiceProxy::SwitchAfeMode）
    int m_afeIdleParam = 0;
    int m_afeCalibrationParam = 0;
    int m_afeClearStatusParam = 1;
    int m_afeForceFreqIdx = 0;
    int m_afeForceScanRateIdx = 0;
    bool m_scanRateIs240Hz = false; // 当前 ScanRate 目标（false=120Hz, true=240Hz）
    std::string m_lastAfeActionStatus = "No command sent";
};

} // namespace App
