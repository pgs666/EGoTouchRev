#pragma once

#include "ServiceProxy.h"
#include "GuiLogSink.h"
#include "SystemStateMonitor.h"
#include "btmcu/PenUsbTransport.h"
#include <string>
#include <vector>
#include <deque>
#include <mutex>

namespace App {

struct BtMcuLogEntry {
    std::string time;
    std::string direction;
    std::vector<uint8_t> data;
};

class DiagnosticsWorkbench {
public:
    explicit DiagnosticsWorkbench(ServiceProxy* proxy);
    ~DiagnosticsWorkbench();

    void Render();

private:
    // Layout
    void SetupDockLayout(unsigned int dockId);
    void DrawStatusBar();
    void DrawControlPanel();
    void DrawInspectorPanel();
    void DrawLogPanel();

    // Content panels (drawn inside Inspector tabs)
    void DrawTouchSolverPanel();
    void DrawTouchTrackingPanel();
    void DrawStylusControlPanel();
    void DrawHeatmap();
    void DrawCoordinateTable();
    void DrawStylusPanel();
    void DrawMasterSuffixTable();
    void DrawSlaveSuffixTable();

    // Tool panels (drawn inside Inspector tabs)
    void DrawBtMcuPanel();
    void DrawSystemEventsPanel();

    void ExportCurrentFrameToCSV(bool isAutoCapture = false);

    // BT MCU helpers
    void BtMcuSendPacket(uint16_t cmdId, const std::vector<uint8_t>& payload);
    void BtMcuAddLog(const std::string& dir, const std::vector<uint8_t>& data);

private:
    ServiceProxy* m_proxy;
    Engine::HeatmapFrame m_currentFrame;

    // GUI state
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

    // Docking layout
    bool m_dockLayoutApplied = false;
    int m_activeInspectorTab = 0;

    // Export
    bool m_exportHeatmap = true;
    bool m_exportMasterStatus = false;
    bool m_exportSlaveStatus = false;
    int m_autoExportTargetPeaks = 0;
    int m_lastPeakCount = 0;

    // AFE control
    int m_afeIdleParam = 0;
    int m_afeCalibrationParam = 0;
    int m_afeClearStatusParam = 1;
    int m_afeForceFreqIdx = 0;
    int m_afeForceScanRateIdx = 0;
    bool m_scanRateIs240Hz = false;
    std::string m_lastAfeActionStatus = "No command sent";

    // BT MCU state
    std::unique_ptr<Himax::Pen::IPenUsbTransport> m_btTransport;
    std::thread m_btReadThread;
    bool m_btKeepReading = false;
    std::mutex m_btLogMutex;
    std::deque<BtMcuLogEntry> m_btLogs;
    char m_btCmdBuf[16] = "7101";
    char m_btPayloadBuf[128] = "";
    char m_btAckBuf[16] = "00";
};

} // namespace App

