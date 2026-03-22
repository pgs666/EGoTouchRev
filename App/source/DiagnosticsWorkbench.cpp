#include "DiagnosticsWorkbench.h"
#include "himax/HimaxChip.h"
#include "MasterFrameParser.h"
#include "BaselineSubtraction.h"
#include "CMFProcessor.h"
#include "GridIIRProcessor.h"
#include "FeatureExtractor.h"
#include "StylusProcessor.h"
#include "TouchTracker.h"
#include "imgui.h"
#include "Logger.h"
#include "Logger.h"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <filesystem>
#include <algorithm>

namespace App {

namespace {

void DrawProcessorConfigBlock(Engine::IFrameProcessor* processor, int idBase) {
    if (processor == nullptr) {
        return;
    }
    ImGui::PushID(idBase);
    bool enabled = processor->IsEnabled();
    if (ImGui::Checkbox(processor->GetName().c_str(), &enabled)) {
        processor->SetEnabled(enabled);
    }
    if (enabled) {
        ImGui::Indent();
        processor->DrawConfigUI();
        ImGui::Unindent();
    }
    ImGui::PopID();
}

} // namespace

DiagnosticsWorkbench::DiagnosticsWorkbench(RuntimeOrchestrator* runtime_orchestrator) : m_runtimeOrchestrator(runtime_orchestrator) {
}

DiagnosticsWorkbench::~DiagnosticsWorkbench() {
}

// ... rest of the content up to DrawHeatmap
void DiagnosticsWorkbench::Render() {
    // 拉取最新的数据
    if (m_autoRefresh && m_renderVisualization && m_runtimeOrchestrator) {
        m_runtimeOrchestrator->GetLatestFrame(m_currentFrame);
    }

    // Keyboard Hotkeys
    if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
        m_fullscreen = !m_fullscreen;
    }
    if (m_fullscreen && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_fullscreen = false;
    }

    // 绘制控制面板和热力图窗口
    DrawControlPanel();
    DrawTouchSolverPanel();
    DrawTouchTrackingPanel();
    DrawStylusControlPanel();
    if (m_renderVisualization) {
        DrawHeatmap();
        if (m_showTouchDebugPanel) {
            DrawCoordinateTable();
        }
        if (m_showStylusDebugPanel) {
            DrawStylusPanel();
        }
        DrawMasterSuffixTable();
        DrawSlaveSuffixTable();
    }
}

void DiagnosticsWorkbench::DrawControlPanel() {
    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 800), ImGuiCond_FirstUseEver);
    ImGui::Begin("Device Control Panel");
    
    if (ImGui::Button("EXIT APPLICATION", ImVec2(-1, 30))) {
        exit(0);
    }
    ImGui::Separator();
    
    if (m_runtimeOrchestrator) {
        Himax::Chip* chip = m_runtimeOrchestrator->GetDevice();
        if (chip) {
            auto connState = chip->GetConnectionState();
            bool connected = (connState == Himax::ConnectionState::Connected);

            ImGui::Text("Connection Status: %s", connected ? "Connected" : "Unconnected");
            
            if (ImGui::Button("Chip::Init")) {
                if (!connected) {
                    LOG_INFO("App", "DiagnosticsWorkbench::DrawControlPanel", "UI", "Chip Init User Action");
                    [[maybe_unused]] auto _r = chip->Init();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Chip::Deinit")) { // Renamed Disconnect to Deinit for consistency
                if (connected) {
                    LOG_INFO("App", "DiagnosticsWorkbench::DrawControlPanel", "UI", "Chip Deinit User Action");
                    [[maybe_unused]] auto _r = chip->Deinit();
                }
            }
            
            ImGui::Separator();
            
            bool loopActive = m_runtimeOrchestrator->IsAcquisitionActive();
            if (!connected) ImGui::BeginDisabled();
            if (ImGui::Button(loopActive ? "Stop Reading Loop" : "Start Reading Loop")) {
                m_runtimeOrchestrator->SetAcquisitionActive(!loopActive);
                LOG_INFO("App", "DiagnosticsWorkbench::DrawControlPanel", "UI", "{} Reading Loop User Action", 
                    loopActive ? "Stop" : "Start");
            }
            if (!connected) ImGui::EndDisabled();

            ImGui::Separator();
            
            if (!connected) ImGui::BeginDisabled();

            auto clamp_u8 = [](int value) -> uint8_t {
                return static_cast<uint8_t>(std::clamp(value, 0, 255));
            };
            auto send_afe_command = [&](const char* actionName, AFE_Command cmd, int paramValue) {
                const uint8_t param = clamp_u8(paramValue);
                auto res = m_runtimeOrchestrator->SwitchAfeMode(cmd, param);
                if (res) {
                    m_lastAfeActionStatus = std::string(actionName) + " success (param=" +
                                            std::to_string(static_cast<unsigned int>(param)) + ")";
                    return;
                }

                m_lastAfeActionStatus = std::string(actionName) + " failed (param=" +
                                        std::to_string(static_cast<unsigned int>(param)) +
                                        ", err=" + std::to_string(static_cast<int>(res.error())) + ")";
            };

            ImGui::TextUnformatted("AFE Mode Control");
            bool autoFreqShiftSync = m_runtimeOrchestrator->IsAutoAfeFreqShiftSyncEnabled();
            if (ImGui::Checkbox("Auto FreqShift Sync (Engine->AFE)", &autoFreqShiftSync)) {
                m_runtimeOrchestrator->SetAutoAfeFreqShiftSyncEnabled(autoFreqShiftSync);
            }
            m_afeIdleParam = std::clamp(m_afeIdleParam, 0, 255);
            ImGui::InputInt("Idle Param", &m_afeIdleParam);
            if (ImGui::Button("Enter Idle")) {
                send_afe_command("EnterIdle", AFE_Command::EnterIdle, m_afeIdleParam);
            }
            ImGui::SameLine();
            if (ImGui::Button("Exit Idle")) {
                send_afe_command("ForceExitIdle", AFE_Command::ForceExitIdle, 0);
            }

            m_afeCalibrationParam = std::clamp(m_afeCalibrationParam, 0, 255);
            ImGui::InputInt("Calibration Param", &m_afeCalibrationParam);
            if (ImGui::Button("Start Calibration")) {
                send_afe_command("StartCalibration", AFE_Command::StartCalibration, m_afeCalibrationParam);
            }

            m_afeClearStatusParam = std::clamp(m_afeClearStatusParam, 0, 255);
            ImGui::InputInt("Clear Status Param", &m_afeClearStatusParam);
            if (ImGui::Button("Clear Status")) {
                send_afe_command("ClearStatus", AFE_Command::ClearStatus, m_afeClearStatusParam);
            }

            if (ImGui::Button("Enable Freq Shift")) {
                send_afe_command("EnableFreqShift", AFE_Command::EnableFreqShift, 0);
            }
            ImGui::SameLine();
            if (ImGui::Button("Disable Freq Shift")) {
                send_afe_command("DisableFreqShift", AFE_Command::DisableFreqShift, 0);
            }

            m_afeForceFreqIdx = std::clamp(m_afeForceFreqIdx, 0, 255);
            ImGui::InputInt("Force Freq Idx", &m_afeForceFreqIdx);
            if (ImGui::Button("Force To Freq Point")) {
                send_afe_command("ForceToFreqPoint", AFE_Command::ForceToFreqPoint, m_afeForceFreqIdx);
            }

            m_afeForceScanRateIdx = std::clamp(m_afeForceScanRateIdx, 0, 255);
            ImGui::InputInt("Force Scan Rate Idx", &m_afeForceScanRateIdx);
            if (ImGui::Button("Force To Scan Rate")) {
                send_afe_command("ForceToScanRate", AFE_Command::ForceToScanRate, m_afeForceScanRateIdx);
            }

            ImGui::TextWrapped("AFE Last Action: %s", m_lastAfeActionStatus.c_str());
            
            if (!connected) ImGui::EndDisabled();
        }
    }
    
    bool isActive = m_runtimeOrchestrator->IsAcquisitionActive();
    if (ImGui::Button(isActive ? "Stop AFE" : "Start AFE")) {
        m_runtimeOrchestrator->SetAcquisitionActive(!isActive);
    }
    
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    if (ImGui::Button("Save Global Parameters")) {
        m_runtimeOrchestrator->SaveConfig();
    }
    ImGui::PopStyleColor();

    bool masterParserOnly = (m_runtimeOrchestrator != nullptr) && m_runtimeOrchestrator->IsMasterParserOnlyMode();
    if (!m_runtimeOrchestrator) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Master Parser Only (Raw Heatmap)", &masterParserOnly) && m_runtimeOrchestrator) {
        m_runtimeOrchestrator->SetMasterParserOnlyMode(masterParserOnly);
    }
    if (!m_runtimeOrchestrator) ImGui::EndDisabled();
    if (masterParserOnly) {
        ImGui::TextUnformatted("Enabled: only Master Frame Parser is active.");
    }

    ImGui::Separator();
    bool vhfReportingEnabled = (m_runtimeOrchestrator != nullptr) && m_runtimeOrchestrator->IsVhfReportingEnabled();
    if (!m_runtimeOrchestrator) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Enable VHF Output", &vhfReportingEnabled) && m_runtimeOrchestrator) {
        m_runtimeOrchestrator->SetVhfReportingEnabled(vhfReportingEnabled);
    }
    if (m_runtimeOrchestrator) {
        ImGui::Text("VHF Device: %s", m_runtimeOrchestrator->IsVhfDeviceOpen() ? "Open" : "Closed");
        bool vhfTranspose = m_runtimeOrchestrator->IsVhfTransposeEnabled();
        if (ImGui::Checkbox("Flip VHF Orientation", &vhfTranspose)) {
            m_runtimeOrchestrator->SetVhfTransposeEnabled(vhfTranspose);
        }
    }
    if (!m_runtimeOrchestrator) ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextUnformatted("Debug Panels");
    ImGui::Checkbox("Touch Debug Panel", &m_showTouchDebugPanel);
    ImGui::Checkbox("Stylus Debug Panel", &m_showStylusDebugPanel);

    ImGui::Separator();
    
    ImGui::Text("Export Options");
    ImGui::Checkbox("Heatmap", &m_exportHeatmap);
    ImGui::SameLine();
    ImGui::Checkbox("Master Status", &m_exportMasterStatus);
    ImGui::SameLine();
    ImGui::Checkbox("Slave Status", &m_exportSlaveStatus);

    if (ImGui::Button("Export Frame to CSV")) {
        ExportCurrentFrameToCSV(false); // Manual
    }
    ImGui::SameLine();
    if (ImGui::Button("Export DVR (Last 480 Frames)")) {
        if (m_runtimeOrchestrator) {
            m_runtimeOrchestrator->TriggerDVRExport(m_exportHeatmap, m_exportMasterStatus, m_exportSlaveStatus);
        }
    }
    
    ImGui::Separator();
    ImGui::Text("Auto-Capture (Debug)");
    ImGui::SliderInt("Target Peaks", &m_autoExportTargetPeaks, 0, 5, m_autoExportTargetPeaks == 0 ? "Disabled" : "%d Peaks");
    
    if (m_runtimeOrchestrator) {
        ImGui::Separator();
        ImGui::TextUnformatted("Common Preprocess Pipeline");
        if (masterParserOnly) ImGui::BeginDisabled();
        int id = 1000;
        for (const auto& p : m_runtimeOrchestrator->GetPipeline().GetProcessors()) {
            if (dynamic_cast<Engine::MasterFrameParser*>(p.get()) ||
                dynamic_cast<Engine::BaselineSubtraction*>(p.get()) ||
                dynamic_cast<Engine::CMFProcessor*>(p.get()) ||
                dynamic_cast<Engine::GridIIRProcessor*>(p.get())) {
                DrawProcessorConfigBlock(p.get(), id++);
            }
        }
        if (masterParserOnly) ImGui::EndDisabled();
    }

    ImGui::Checkbox("Auto-refresh Heatmap", &m_autoRefresh);
    ImGui::Checkbox("Render Visualization", &m_renderVisualization);
    if (!m_renderVisualization) {
        ImGui::TextUnformatted("Visualization disabled: acquisition/processing threads keep running.");
    }
    ImGui::Checkbox("Fullscreen Heatmap", &m_fullscreen);
    ImGui::SliderInt("Heatmap Scale", &m_heatmapScale, 1, 30);
    ImGui::SliderFloat("Color Max Range", &m_colorRange, 100.0f, 10000.0f, "%.0f");
    
    ImGui::End();
}

void DiagnosticsWorkbench::DrawTouchSolverPanel() {
    ImGui::SetNextWindowPos(ImVec2(520, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430, 520), ImGuiCond_FirstUseEver);
    ImGui::Begin("Touch Solver Panel (Feature Extraction)");

    if (!m_runtimeOrchestrator) {
        ImGui::TextUnformatted("RuntimeOrchestrator unavailable.");
        ImGui::End();
        return;
    }

    const bool masterParserOnly = m_runtimeOrchestrator->IsMasterParserOnlyMode();
    if (masterParserOnly) {
        ImGui::TextUnformatted("Master Parser Only is enabled.");
        ImGui::BeginDisabled();
    }

    bool foundAny = false;
    int id = 2000;
    for (const auto& p : m_runtimeOrchestrator->GetPipeline().GetProcessors()) {
        if (dynamic_cast<Engine::FeatureExtractor*>(p.get())) {
            DrawProcessorConfigBlock(p.get(), id++);
            foundAny = true;
        }
    }
    if (!foundAny) {
        ImGui::TextUnformatted("No FeatureExtractor found.");
    }

    if (masterParserOnly) {
        ImGui::EndDisabled();
    }

    ImGui::End();
}

void DiagnosticsWorkbench::DrawTouchTrackingPanel() {
    ImGui::SetNextWindowPos(ImVec2(520, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430, 520), ImGuiCond_FirstUseEver);
    ImGui::Begin("Touch Tracking/Report Panel");

    if (!m_runtimeOrchestrator) {
        ImGui::TextUnformatted("RuntimeOrchestrator unavailable.");
        ImGui::End();
        return;
    }

    const bool masterParserOnly = m_runtimeOrchestrator->IsMasterParserOnlyMode();
    if (masterParserOnly) {
        ImGui::TextUnformatted("Master Parser Only is enabled.");
        ImGui::BeginDisabled();
    }

    bool foundAny = false;
    int id = 2500;
    for (const auto& p : m_runtimeOrchestrator->GetPipeline().GetProcessors()) {
        if (dynamic_cast<Engine::TouchTracker*>(p.get())) {
            DrawProcessorConfigBlock(p.get(), id++);
            foundAny = true;
        }
    }
    if (!foundAny) {
        ImGui::TextUnformatted("No TouchTracker found.");
    }

    if (masterParserOnly) {
        ImGui::EndDisabled();
    }

    ImGui::End();
}

void DiagnosticsWorkbench::DrawStylusControlPanel() {
    ImGui::SetNextWindowPos(ImVec2(950, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460, 640), ImGuiCond_FirstUseEver);
    ImGui::Begin("Stylus Pipeline Panel");

    if (!m_runtimeOrchestrator) {
        ImGui::TextUnformatted("RuntimeOrchestrator unavailable.");
        ImGui::End();
        return;
    }

    const bool masterParserOnly = m_runtimeOrchestrator->IsMasterParserOnlyMode();
    if (masterParserOnly) {
        ImGui::TextUnformatted("Master Parser Only is enabled.");
        ImGui::BeginDisabled();
    }

    bool foundAny = false;
    int id = 3000;
    Engine::TouchTracker* touchTracker = nullptr;
    for (const auto& p : m_runtimeOrchestrator->GetPipeline().GetProcessors()) {
        if (dynamic_cast<Engine::StylusProcessor*>(p.get())) {
            DrawProcessorConfigBlock(p.get(), id++);
            foundAny = true;
        } else if (!touchTracker) {
            touchTracker = dynamic_cast<Engine::TouchTracker*>(p.get());
        }
    }
    if (touchTracker != nullptr) {
        ImGui::Separator();
        ImGui::TextUnformatted("Touch<->Stylus Interop (TouchTracker)");
        bool trackerEnabled = touchTracker->IsEnabled();
        if (!trackerEnabled) {
            ImGui::TextUnformatted("TouchTracker is disabled.");
            ImGui::BeginDisabled();
        }
        touchTracker->DrawStylusInteropConfigUI();
        if (!trackerEnabled) {
            ImGui::EndDisabled();
        }
        foundAny = true;
    }
    if (!foundAny) {
        ImGui::TextUnformatted("No stylus processors found.");
    }

    if (masterParserOnly) {
        ImGui::EndDisabled();
    }

    ImGui::End();
}

void DiagnosticsWorkbench::DrawHeatmap() {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar;

    if (m_fullscreen) {
        // True Fullscreen: Set window to cover the PRIMARY monitor (usually at 0,0)
        // This avoids the issue where GetMainViewport() is at (-10000, -10000) because the main window is hidden.
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        
        // Use PlatformIO to get actual monitor size if possible, otherwise fall back to large enough size
        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        if (platform_io.Monitors.Size > 0) {
            ImGui::SetNextWindowSize(platform_io.Monitors[0].MainSize);
        } else {
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        }
        
        window_flags |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    } else {
        ImGui::SetNextWindowPos(ImVec2(550, 100), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(600, 800), ImGuiCond_FirstUseEver);
    }

    ImGui::Begin("Raw Heatmap Visualization (40x60)", nullptr, window_flags);
    
    // In multi-viewport mode, if the window is outside, GetContentRegionAvail might refer to its own OS window
    ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    ImVec2 canvas_p = ImGui::GetCursorScreenPos();

    const int rows = 40; // TX 
    const int cols = 60; // RX 

    // 计算动态缩放比例
    float cell_w, cell_h;
    if (m_fullscreen || canvas_sz.x > 800) { // If fullscreen or manually expanded
        cell_w = canvas_sz.x / cols;
        cell_h = canvas_sz.y / rows;
    } else {
        cell_w = (float)m_heatmapScale;
        cell_h = (float)m_heatmapScale;
    }

    if (!m_fullscreen) {
        ImGui::Text("Timestamp: %llu | Cell Size: %.1fx%.1f", (unsigned long long)m_currentFrame.timestamp, cell_w, cell_h);
    }
    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            // Mirror Matrix: Left becomes Right, Top becomes Bottom
            int16_t val = m_currentFrame.heatmapMatrix[rows - 1 - y][cols - 1 - x];
            
            // Bipolar mapping (Diverging Color Scale)
            // positive: black -> blue -> green -> yellow -> red
            // negative: black -> purple -> cyan
            float normalized = std::clamp(val / m_colorRange, -1.0f, 1.0f);
            
            ImVec4 colorVec;
            if (normalized == 0.0f) {
                colorVec = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
            } else if (normalized > 0.0f) {
                // Positive signal: Jet-like (warm)
                float v = normalized * 4.0f;
                float r = std::clamp(std::min(v - 1.5f, -v + 4.5f), 0.0f, 1.0f);
                float g = std::clamp(std::min(v - 0.5f, -v + 3.5f), 0.0f, 1.0f);
                float b = std::clamp(std::min(v + 0.5f, -v + 2.5f), 0.0f, 1.0f);
                colorVec = ImVec4(r, g, b, 1.0f);
            } else {
                // Negative signal: Cold colors
                float v = (-normalized) * 2.0f; // Scale to [0, 2]
                float r = std::clamp(v * 0.5f, 0.0f, 0.5f); // subtle purple/dark red
                float g = std::clamp(v - 1.0f, 0.0f, 1.0f); // cyan mix
                float b = std::clamp(v, 0.0f, 1.0f);        // blue
                colorVec = ImVec4(r, g, b, 1.0f);
            }
            
            ImU32 color = ImGui::ColorConvertFloat4ToU32(colorVec);
            
            ImVec2 p_min = ImVec2(canvas_p.x + x * cell_w, canvas_p.y + y * cell_h);
            ImVec2 p_max = ImVec2(p_min.x + cell_w, p_min.y + cell_h);
            
            draw_list->AddRectFilled(p_min, p_max, color);
            
            // 画边线 (全屏模式下为了 1:1 视觉效果可以考虑减淡或者移除边线)
            draw_list->AddRect(p_min, p_max, IM_COL32(50, 50, 50, m_fullscreen ? 50 : 255));
        }
    }
    
    // 绘制 FeatureExtractor (Phase 4.1/4.2) 提取到的 Peaks 和 Zones
    if (m_runtimeOrchestrator) {
        const auto& processors = m_runtimeOrchestrator->GetPipeline().GetProcessors();
        for (const auto& proc : processors) {
            // Find our FeatureExtractor in the pipeline
            if (auto extractor = dynamic_cast<Engine::FeatureExtractor*>(proc.get())) {
                if (extractor->IsEnabled()) {
                    const auto& peaks = extractor->GetPeaks();
                    const auto& zones = extractor->GetTouchZones();
                    
                    int currentPeakCount = peaks.size();
                    // Auto-Capture logic for N fingers
                    if (m_autoExportTargetPeaks > 0 && 
                        currentPeakCount == m_autoExportTargetPeaks && 
                        m_lastPeakCount != m_autoExportTargetPeaks) {
                        ExportCurrentFrameToCSV(true); // Auto
                    }
                    m_lastPeakCount = currentPeakCount;

                    // Draw Touch Zones (Color Outlines)
                    for (int r = 0; r < rows; ++r) {
                        for (int c = 0; c < cols; ++c) {
                            uint8_t zoneId = zones[r * cols + c];
                            if (zoneId > 0) {
                                int mirrored_r = rows - 1 - r;
                                int mirrored_c = cols - 1 - c;
                                
                                ImVec2 p_min = ImVec2(canvas_p.x + mirrored_c * cell_w, canvas_p.y + mirrored_r * cell_h);
                                ImVec2 p_max = ImVec2(p_min.x + cell_w, p_min.y + cell_h);
                                
                                // Color based on zone ID (cycle through bright colors)
                                ImVec4 outlineCol;
                                switch (zoneId % 6) {
                                    case 1: outlineCol = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); break; // Red
                                    case 2: outlineCol = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); break; // Green
                                    case 3: outlineCol = ImVec4(0.0f, 0.5f, 1.0f, 1.0f); break; // Light Blue
                                    case 4: outlineCol = ImVec4(1.0f, 0.0f, 1.0f, 1.0f); break; // Magenta
                                    case 5: outlineCol = ImVec4(0.0f, 1.0f, 1.0f, 1.0f); break; // Cyan
                                    case 0: outlineCol = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); break; // Orange
                                }
                                
                                ImU32 colU32 = ImGui::ColorConvertFloat4ToU32(outlineCol);
                                
                                // Subtle transparent fill
                                draw_list->AddRectFilled(p_min, p_max, ImGui::ColorConvertFloat4ToU32(ImVec4(outlineCol.x, outlineCol.y, outlineCol.z, 0.2f)));

                                // Draw boundaries only on edges facing a different zone
                                // Matrix mapping -> Visual mapping Check
                                bool v_diff_top    = (r == rows - 1) || (zones[(r + 1) * cols + c] != zoneId);
                                bool v_diff_bottom = (r == 0)        || (zones[(r - 1) * cols + c] != zoneId);
                                bool v_diff_left   = (c == cols - 1) || (zones[r * cols + (c + 1)] != zoneId);
                                bool v_diff_right  = (c == 0)        || (zones[r * cols + (c - 1)] != zoneId);

                                float border_thickness = 2.0f;
                                if (v_diff_top)    draw_list->AddLine(ImVec2(p_min.x, p_min.y), ImVec2(p_max.x, p_min.y), colU32, border_thickness);
                                if (v_diff_bottom) draw_list->AddLine(ImVec2(p_min.x, p_max.y), ImVec2(p_max.x, p_max.y), colU32, border_thickness);
                                if (v_diff_left)   draw_list->AddLine(ImVec2(p_min.x, p_min.y), ImVec2(p_min.x, p_max.y), colU32, border_thickness);
                                if (v_diff_right)  draw_list->AddLine(ImVec2(p_max.x, p_min.y), ImVec2(p_max.x, p_max.y), colU32, border_thickness);

                            }
                        }
                    }

                    // Draw Peaks (Crosshairs)
                    for (const auto& peak : peaks) {
                        int pr = peak.r;
                        int pc = peak.c;
                        int16_t peakValue = peak.z;
                        
                        // Mirror matrix coordinates exactly like the heatmap loop
                        int mirrored_r = rows - 1 - pr;
                        int mirrored_c = cols - 1 - pc;
                        
                        // Calculate center of the cell
                        float cx = canvas_p.x + mirrored_c * cell_w + cell_w * 0.5f;
                        float cy = canvas_p.y + mirrored_r * cell_h + cell_h * 0.5f;
                        
                        // Draw a prominent Yellow Cross marker
                        ImU32 markerColor = IM_COL32(255, 255, 0, 255); // Yellow
                        float crossSize = std::min(cell_w, cell_h) * 0.6f; // 120% of cell size to stick out
                        
                        draw_list->AddLine(ImVec2(cx - crossSize, cy), ImVec2(cx + crossSize, cy), markerColor, 2.0f);
                        draw_list->AddLine(ImVec2(cx, cy - crossSize), ImVec2(cx, cy + crossSize), markerColor, 2.0f);
                        
                        // Draw the pressure value text
                        char label[32];
                        snprintf(label, sizeof(label), "%d", peakValue);
                        ImU32 textColor = IM_COL32(255, 255, 255, 255); // White text
                        ImU32 outlineColor = IM_COL32(0, 0, 0, 255);    // Black outline

                        ImVec2 textPos(cx + 4.0f, cy + 4.0f);
                        
                        // Draw text outline (shadow)
                        draw_list->AddText(ImVec2(textPos.x - 1, textPos.y - 1), outlineColor, label);
                        draw_list->AddText(ImVec2(textPos.x + 1, textPos.y - 1), outlineColor, label);
                        draw_list->AddText(ImVec2(textPos.x - 1, textPos.y + 1), outlineColor, label);
                        draw_list->AddText(ImVec2(textPos.x + 1, textPos.y + 1), outlineColor, label);
                        
                        // Draw main text
                        draw_list->AddText(textPos, textColor, label);
                    }
                }
                break; // Found it, no need to keep looking
            }
        }
    }
    
    if (!m_fullscreen) {
        // 根据格子占用空间，手动往下推 ImGui 的 Cursor
        ImGui::Dummy(ImVec2(cols * cell_w, rows * cell_h));
    }
    
    ImGui::End();
}

void DiagnosticsWorkbench::DrawCoordinateTable() {
    ImGui::SetNextWindowPos(ImVec2(550, 930), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620, 300), ImGuiCond_FirstUseEver);
    ImGui::Begin("Touch Debug (Parsed Coordinates)");

    if (m_currentFrame.contacts.empty()) {
        ImGui::Text("No touches detected.");
    } else {
        // Keep a stable UI order by touch ID, so rows do not jump when X/Y changes.
        std::vector<const Engine::TouchContact*> orderedContacts;
        orderedContacts.reserve(m_currentFrame.contacts.size());
        for (const auto& c : m_currentFrame.contacts) {
            orderedContacts.push_back(&c);
        }
        std::stable_sort(orderedContacts.begin(), orderedContacts.end(),
                         [](const Engine::TouchContact* a, const Engine::TouchContact* b) {
                             return a->id < b->id;
                         });

        if (ImGui::BeginTable("ContactsTable", 12, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("X (Sub-pixel)", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Y (Sub-pixel)", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Area", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("SigSum", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Size(mm)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Reported", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("RptEvt", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("LifeFlg", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("RptFlg", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Dbg", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();

            for (const auto* contactPtr : orderedContacts) {
                const auto& contact = *contactPtr;
                ImGui::TableNextRow();
                
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", contact.id);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.4f", contact.x);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.4f", contact.y);

                ImGui::TableSetColumnIndex(3);
                const char* stateStr = "UNK";
                if (contact.state == 0) stateStr = "Down";
                else if (contact.state == 1) stateStr = "Move";
                else if (contact.state == 2) stateStr = "Up";
                ImGui::Text("%s", stateStr);

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%d", contact.area);

                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%d", contact.signalSum);

                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%.2f", contact.sizeMm);

                ImGui::TableSetColumnIndex(7);
                ImGui::Text("%s", contact.isReported ? "Y" : "N");

                ImGui::TableSetColumnIndex(8);
                const char* reportEventStr = "UNK";
                if (contact.reportEvent == Engine::TouchReportIdle) reportEventStr = "Idle";
                else if (contact.reportEvent == Engine::TouchReportDown) reportEventStr = "Down";
                else if (contact.reportEvent == Engine::TouchReportMove) reportEventStr = "Move";
                else if (contact.reportEvent == Engine::TouchReportUp) reportEventStr = "Up";
                ImGui::Text("%s", reportEventStr);

                ImGui::TableSetColumnIndex(9);
                ImGui::Text("0x%X", static_cast<unsigned int>(contact.lifeFlags));

                ImGui::TableSetColumnIndex(10);
                ImGui::Text("0x%X", static_cast<unsigned int>(contact.reportFlags));

                ImGui::TableSetColumnIndex(11);
                ImGui::Text("0x%X", contact.debugFlags);
            }
            ImGui::EndTable();
        }
    }

    ImGui::Separator();
    auto drawTouchPacket = [&](const char* label, const Engine::TouchPacket& packet) {
        ImGui::Text("%s: %s (RID=0x%02X Len=%u)", label, packet.valid ? "Valid" : "Invalid",
                    packet.reportId, packet.length);
        if (!packet.valid) {
            return;
        }
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (size_t i = 0; i < packet.bytes.size(); ++i) {
            oss << std::setw(2) << static_cast<unsigned int>(packet.bytes[i]);
            if (i + 1 < packet.bytes.size()) {
                oss << " ";
            }
        }
        ImGui::TextUnformatted(oss.str().c_str());
    };
    drawTouchPacket("TouchPacket[0]", m_currentFrame.touchPackets[0]);
    drawTouchPacket("TouchPacket[1]", m_currentFrame.touchPackets[1]);

    ImGui::End();
}

void DiagnosticsWorkbench::DrawStylusPanel() {
    ImGui::SetNextWindowPos(ImVec2(1180, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 540), ImGuiCond_FirstUseEver);
    ImGui::Begin("Stylus Debug (ASA/HPP2/HPP3-lite)");

    const auto& stylus = m_currentFrame.stylus;
    ImGui::Text("Slave Valid: %s", stylus.slaveValid ? "Y" : "N");
    ImGui::Text("Slave Offset: %u  Checksum16: 0x%04X (%s)",
                static_cast<unsigned int>(stylus.slaveWordOffset),
                static_cast<unsigned int>(stylus.checksum16),
                stylus.checksumOk ? "OK" : "FAIL");
    ImGui::Text("TX1 Block: %s | TX2 Block: %s",
                stylus.tx1BlockValid ? "Y" : "N",
                stylus.tx2BlockValid ? "Y" : "N");
    ImGui::Text("MasterMeta: %s Base=%d Tx1/Tx2=%u/%u Press=%u Btn=0x%08X St=0x%08X",
                stylus.masterMetaValid ? "Y" : "N",
                stylus.masterMetaValid ? static_cast<int>(stylus.masterMetaBaseWord) : -1,
                static_cast<unsigned int>(stylus.masterMetaTx1Freq),
                static_cast<unsigned int>(stylus.masterMetaTx2Freq),
                static_cast<unsigned int>(stylus.masterMetaPressure),
                static_cast<unsigned int>(stylus.masterMetaButton),
                static_cast<unsigned int>(stylus.masterMetaStatus));
    ImGui::Text("Status: 0x%08X", static_cast<unsigned int>(stylus.status));
    ImGui::Text("ASA Mode/DataType: %u / %u  Result:%u  Valid:%s",
                static_cast<unsigned int>(stylus.asaMode),
                static_cast<unsigned int>(stylus.dataType),
                static_cast<unsigned int>(stylus.processResult),
                stylus.validJudgmentPassed ? "Y" : "N");
    ImGui::Text("Recheck: En=%s Pass=%s Overlap=%s Th=%u",
                stylus.recheckEnabled ? "Y" : "N",
                stylus.recheckPassed ? "Y" : "N",
                stylus.recheckOverlap ? "Y" : "N",
                static_cast<unsigned int>(stylus.recheckThreshold));
    ImGui::Text("HPP3 Noise: Invalid=%s Debounce=%s",
                stylus.hpp3NoiseInvalid ? "Y" : "N",
                stylus.hpp3NoiseDebounce ? "Y" : "N");
    ImGui::Text("HPP3 SigValid D1/D2: %s / %s  RatioWarn X/Y: %u / %u",
                stylus.hpp3Dim1SignalValid ? "Y" : "N",
                stylus.hpp3Dim2SignalValid ? "Y" : "N",
                static_cast<unsigned int>(stylus.hpp3RatioWarnCountX),
                static_cast<unsigned int>(stylus.hpp3RatioWarnCountY));
    ImGui::Text("HPP3 SigAvg X/Y: %u / %u  Samples: %u",
                static_cast<unsigned int>(stylus.hpp3SignalAvgX),
                static_cast<unsigned int>(stylus.hpp3SignalAvgY),
                static_cast<unsigned int>(stylus.hpp3SignalSampleCount));
    ImGui::Text("TouchSuppress: Active=%s NullLike=%s Remain=%u",
                stylus.touchSuppressActive ? "Y" : "N",
                stylus.touchNullLike ? "Y" : "N",
                static_cast<unsigned int>(stylus.touchSuppressFrames));
    ImGui::Text("Freq TX1/TX2: 0x%04X / 0x%04X", stylus.tx1Freq, stylus.tx2Freq);
    ImGui::Text("Next Freq TX1/TX2: 0x%04X / 0x%04X", stylus.nextTx1Freq, stylus.nextTx2Freq);
    ImGui::Text("Pressure: %u (Raw:%u Mapped:%u)  Button: %u (Raw:0x%X Src:%u)",
                stylus.pressure,
                static_cast<unsigned int>(stylus.point.rawPressure),
                static_cast<unsigned int>(stylus.point.mappedPressure),
                static_cast<unsigned int>(stylus.button),
                static_cast<unsigned int>(stylus.rawButton),
                static_cast<unsigned int>(stylus.buttonSource));
    ImGui::Text("Signal X/Y: %u / %u  MaxPeak: %u  NoPressInk:%s",
                static_cast<unsigned int>(stylus.signalX),
                static_cast<unsigned int>(stylus.signalY),
                static_cast<unsigned int>(stylus.maxRawPeak),
                stylus.noPressInkActive ? "Y" : "N");

    if (stylus.point.valid) {
        ImGui::Text("Point: X=%.3f  Y=%.3f  Confidence=%.3f",
                    stylus.point.x,
                    stylus.point.y,
                    stylus.point.confidence);
        ImGui::Text("Report Coord: X=%u  Y=%u",
                    static_cast<unsigned int>(stylus.point.reportX),
                    static_cast<unsigned int>(stylus.point.reportY));
        ImGui::Text("TX1/TX2 Coord: (%.3f,%.3f) / (%.3f,%.3f)",
                    stylus.point.tx1X,
                    stylus.point.tx1Y,
                    stylus.point.tx2X,
                    stylus.point.tx2Y);
        ImGui::Text("Peak TX1/TX2: %u / %u", stylus.point.peakTx1, stylus.point.peakTx2);
        ImGui::Text("Tilt: Valid=%s Pre(%d,%d) Out(%d,%d) |Mag|=%.2f Az=%.1f",
                    stylus.point.tiltValid ? "Y" : "N",
                    static_cast<int>(stylus.point.preTiltX),
                    static_cast<int>(stylus.point.preTiltY),
                    static_cast<int>(stylus.point.tiltX),
                    static_cast<int>(stylus.point.tiltY),
                    stylus.point.tiltMagnitude,
                    stylus.point.tiltAzimuthDeg);
    } else {
        ImGui::TextUnformatted("Point: Invalid");
    }

    if (stylus.packet.valid) {
        ImGui::Separator();
        ImGui::Text("Packet (RID=0x%02X, Len=%u):", stylus.packet.reportId, stylus.packet.length);
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (size_t i = 0; i < stylus.packet.bytes.size(); ++i) {
            oss << std::setw(2) << static_cast<unsigned int>(stylus.packet.bytes[i]);
            if (i + 1 < stylus.packet.bytes.size()) {
                oss << " ";
            }
        }
        ImGui::TextUnformatted(oss.str().c_str());
    } else {
        ImGui::TextUnformatted("Packet: Invalid");
    }

    ImGui::End();
}

void DiagnosticsWorkbench::DrawMasterSuffixTable() {
    ImGui::Begin("Master Frame Suffix (128 words)");
    if (m_currentFrame.rawData.size() >= 5063) {
        if (ImGui::BeginTable("MasterSuffixTable", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            const uint8_t* ptr = m_currentFrame.rawData.data() + 4807;
            for (int i = 0; i < 128; ++i) {
                if (i % 8 == 0) ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(i % 8);
                uint16_t val = static_cast<uint16_t>(ptr[i * 2] | (ptr[i * 2 + 1] << 8));
                ImGui::Text("[%03d]: %04X (%5d)", i, val, val);
            }
            ImGui::EndTable();
        }
    } else {
        ImGui::Text("Insufficient frame data length.");
    }
    ImGui::End();
}

void DiagnosticsWorkbench::DrawSlaveSuffixTable() {
    ImGui::Begin("Slave Frame Suffix (166 words)");
    if (m_currentFrame.rawData.size() >= 5402) { // 5063 + 339 = 5402
        if (ImGui::BeginTable("SlaveSuffixTable", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            const uint8_t* ptr = m_currentFrame.rawData.data() + 5070; // 5063 + 7
            for (int i = 0; i < 166; ++i) {
                if (i % 8 == 0) ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(i % 8);
                uint16_t val = static_cast<uint16_t>(ptr[i * 2] | (ptr[i * 2 + 1] << 8));
                ImGui::Text("[%03d]: %04X (%5d)", i, val, val);
            }
            ImGui::EndTable();
        }
    } else {
        ImGui::Text("Insufficient slave overlay data length.");
    }
    ImGui::End();
}

void DiagnosticsWorkbench::ExportCurrentFrameToCSV(bool isAutoCapture) {
    auto now = std::chrono::system_clock::now();
    std::time_t time_now = std::chrono::system_clock::to_time_t(now);
    std::tm* tm_now = std::localtime(&time_now);
    
    // Create folders
    std::string baseFolder = "exports";
    std::string subFolder = isAutoCapture ? "auto" : "manual";
    std::filesystem::path dir(baseFolder);
    dir /= subFolder;
    
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR("App", "DiagnosticsWorkbench::ExportCurrentFrameToCSV", "System", "Failed to create directory: {}", dir.string());
    }
    
    // We add a high-resolution counter parameter or thread ID to absolutely guarantee no overwrites
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    static uint32_t captureCounter = 0;
    
    std::ostringstream filename;
    filename << "heatmap_" 
             << std::put_time(tm_now, "%Y%m%d_%H%M%S") << "_" 
             << std::setfill('0') << std::setw(3) << ms.count() << "_"
             << captureCounter++
             << ".csv";
             
    std::filesystem::path fullPath = dir / filename.str();

    std::ofstream out(fullPath.string());
    if (!out.is_open()) {
        LOG_INFO("App", "DiagnosticsWorkbench::ExportCurrentFrameToCSV", "Error", "Failed to open file for writing: {}", fullPath.string());
        return;
    }

    out << "--- EGoTouch Frame Export ---\n";
    out << "Timestamp: " << m_currentFrame.timestamp << "\n\n";
    out << "--- Contacts ---\n";
    out << "Count," << m_currentFrame.contacts.size() << "\n";
    out << "ID,X,Y,State,Area,SignalSum,SizeMm,Reported,ReportEvent,LifeFlags,ReportFlags,DebugFlags\n";
    for (const auto& c : m_currentFrame.contacts) {
        out << c.id << ","
            << c.x << ","
            << c.y << ","
            << c.state << ","
            << c.area << ","
            << c.signalSum << ","
            << c.sizeMm << ","
            << (c.isReported ? 1 : 0) << ","
            << c.reportEvent << ","
            << c.lifeFlags << ","
            << c.reportFlags << ","
            << c.debugFlags << "\n";
    }
    out << "\n";
    out << "--- Touch Packets (VHF 0x20) ---\n";
    out << "Packet0Valid," << (m_currentFrame.touchPackets[0].valid ? 1 : 0) << "\n";
    out << "Packet0Hex,";
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < m_currentFrame.touchPackets[0].bytes.size(); ++i) {
        out << std::setw(2) << static_cast<unsigned int>(m_currentFrame.touchPackets[0].bytes[i]);
        if (i + 1 < m_currentFrame.touchPackets[0].bytes.size()) {
            out << " ";
        }
    }
    out << std::dec << "\n";
    out << "Packet1Valid," << (m_currentFrame.touchPackets[1].valid ? 1 : 0) << "\n";
    out << "Packet1Hex,";
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < m_currentFrame.touchPackets[1].bytes.size(); ++i) {
        out << std::setw(2) << static_cast<unsigned int>(m_currentFrame.touchPackets[1].bytes[i]);
        if (i + 1 < m_currentFrame.touchPackets[1].bytes.size()) {
            out << " ";
        }
    }
    out << std::dec << "\n\n";

    out << "--- Stylus ---\n";
    out << "SlaveValid," << (m_currentFrame.stylus.slaveValid ? 1 : 0) << "\n";
    out << "SlaveWordOffset," << static_cast<unsigned int>(m_currentFrame.stylus.slaveWordOffset) << "\n";
    out << "Checksum16," << m_currentFrame.stylus.checksum16 << "\n";
    out << "ChecksumOK," << (m_currentFrame.stylus.checksumOk ? 1 : 0) << "\n";
    out << "Tx1BlockValid," << (m_currentFrame.stylus.tx1BlockValid ? 1 : 0) << "\n";
    out << "Tx2BlockValid," << (m_currentFrame.stylus.tx2BlockValid ? 1 : 0) << "\n";
    out << "MasterMetaValid," << (m_currentFrame.stylus.masterMetaValid ? 1 : 0) << "\n";
    out << "MasterMetaBaseWord," << static_cast<unsigned int>(m_currentFrame.stylus.masterMetaBaseWord) << "\n";
    out << "MasterMetaTx1Freq," << m_currentFrame.stylus.masterMetaTx1Freq << "\n";
    out << "MasterMetaTx2Freq," << m_currentFrame.stylus.masterMetaTx2Freq << "\n";
    out << "MasterMetaPressure," << m_currentFrame.stylus.masterMetaPressure << "\n";
    out << "MasterMetaButton," << m_currentFrame.stylus.masterMetaButton << "\n";
    out << "MasterMetaStatus," << m_currentFrame.stylus.masterMetaStatus << "\n";
    out << "Status," << m_currentFrame.stylus.status << "\n";
    out << "AsaMode," << static_cast<unsigned int>(m_currentFrame.stylus.asaMode) << "\n";
    out << "DataType," << static_cast<unsigned int>(m_currentFrame.stylus.dataType) << "\n";
    out << "ProcessResult," << static_cast<unsigned int>(m_currentFrame.stylus.processResult) << "\n";
    out << "ValidJudgmentPassed," << (m_currentFrame.stylus.validJudgmentPassed ? 1 : 0) << "\n";
    out << "RecheckEnabled," << (m_currentFrame.stylus.recheckEnabled ? 1 : 0) << "\n";
    out << "RecheckPassed," << (m_currentFrame.stylus.recheckPassed ? 1 : 0) << "\n";
    out << "RecheckOverlap," << (m_currentFrame.stylus.recheckOverlap ? 1 : 0) << "\n";
    out << "RecheckThreshold," << m_currentFrame.stylus.recheckThreshold << "\n";
    out << "Hpp3NoiseInvalid," << (m_currentFrame.stylus.hpp3NoiseInvalid ? 1 : 0) << "\n";
    out << "Hpp3NoiseDebounce," << (m_currentFrame.stylus.hpp3NoiseDebounce ? 1 : 0) << "\n";
    out << "Hpp3Dim1SignalValid," << (m_currentFrame.stylus.hpp3Dim1SignalValid ? 1 : 0) << "\n";
    out << "Hpp3Dim2SignalValid," << (m_currentFrame.stylus.hpp3Dim2SignalValid ? 1 : 0) << "\n";
    out << "Hpp3RatioWarnCountX," << static_cast<unsigned int>(m_currentFrame.stylus.hpp3RatioWarnCountX) << "\n";
    out << "Hpp3RatioWarnCountY," << static_cast<unsigned int>(m_currentFrame.stylus.hpp3RatioWarnCountY) << "\n";
    out << "Hpp3SignalAvgX," << m_currentFrame.stylus.hpp3SignalAvgX << "\n";
    out << "Hpp3SignalAvgY," << m_currentFrame.stylus.hpp3SignalAvgY << "\n";
    out << "Hpp3SignalSampleCount," << static_cast<unsigned int>(m_currentFrame.stylus.hpp3SignalSampleCount) << "\n";
    out << "TouchNullLike," << (m_currentFrame.stylus.touchNullLike ? 1 : 0) << "\n";
    out << "TouchSuppressActive," << (m_currentFrame.stylus.touchSuppressActive ? 1 : 0) << "\n";
    out << "TouchSuppressFrames," << static_cast<unsigned int>(m_currentFrame.stylus.touchSuppressFrames) << "\n";
    out << "SignalX," << m_currentFrame.stylus.signalX << "\n";
    out << "SignalY," << m_currentFrame.stylus.signalY << "\n";
    out << "MaxRawPeak," << m_currentFrame.stylus.maxRawPeak << "\n";
    out << "NoPressInkActive," << (m_currentFrame.stylus.noPressInkActive ? 1 : 0) << "\n";
    out << "Tx1Freq," << m_currentFrame.stylus.tx1Freq << "\n";
    out << "Tx2Freq," << m_currentFrame.stylus.tx2Freq << "\n";
    out << "NextTx1Freq," << m_currentFrame.stylus.nextTx1Freq << "\n";
    out << "NextTx2Freq," << m_currentFrame.stylus.nextTx2Freq << "\n";
    out << "Pressure," << m_currentFrame.stylus.pressure << "\n";
    out << "Button," << m_currentFrame.stylus.button << "\n";
    out << "RawButton," << m_currentFrame.stylus.rawButton << "\n";
    out << "ButtonSource," << static_cast<unsigned int>(m_currentFrame.stylus.buttonSource) << "\n";
    out << "PointValid," << (m_currentFrame.stylus.point.valid ? 1 : 0) << "\n";
    out << "PointX," << m_currentFrame.stylus.point.x << "\n";
    out << "PointY," << m_currentFrame.stylus.point.y << "\n";
    out << "ReportX," << m_currentFrame.stylus.point.reportX << "\n";
    out << "ReportY," << m_currentFrame.stylus.point.reportY << "\n";
    out << "PointConfidence," << m_currentFrame.stylus.point.confidence << "\n";
    out << "RawPressure," << m_currentFrame.stylus.point.rawPressure << "\n";
    out << "MappedPressure," << m_currentFrame.stylus.point.mappedPressure << "\n";
    out << "PeakTx1," << m_currentFrame.stylus.point.peakTx1 << "\n";
    out << "PeakTx2," << m_currentFrame.stylus.point.peakTx2 << "\n";
    out << "Tx1X," << m_currentFrame.stylus.point.tx1X << "\n";
    out << "Tx1Y," << m_currentFrame.stylus.point.tx1Y << "\n";
    out << "Tx2X," << m_currentFrame.stylus.point.tx2X << "\n";
    out << "Tx2Y," << m_currentFrame.stylus.point.tx2Y << "\n";
    out << "TiltValid," << (m_currentFrame.stylus.point.tiltValid ? 1 : 0) << "\n";
    out << "PreTiltX," << m_currentFrame.stylus.point.preTiltX << "\n";
    out << "PreTiltY," << m_currentFrame.stylus.point.preTiltY << "\n";
    out << "TiltX," << m_currentFrame.stylus.point.tiltX << "\n";
    out << "TiltY," << m_currentFrame.stylus.point.tiltY << "\n";
    out << "TiltMagnitude," << m_currentFrame.stylus.point.tiltMagnitude << "\n";
    out << "TiltAzimuthDeg," << m_currentFrame.stylus.point.tiltAzimuthDeg << "\n";
    out << "PacketValid," << (m_currentFrame.stylus.packet.valid ? 1 : 0) << "\n";
    out << "PacketHex,";
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < m_currentFrame.stylus.packet.bytes.size(); ++i) {
        out << std::setw(2) << static_cast<unsigned int>(m_currentFrame.stylus.packet.bytes[i]);
        if (i + 1 < m_currentFrame.stylus.packet.bytes.size()) {
            out << " ";
        }
    }
    out << std::dec << "\n\n";

    if (m_exportHeatmap) {
        out << "--- Heatmap (40 rows x 60 cols) ---\n";
        for (int y = 0; y < 40; ++y) {
            for (int x = 0; x < 60; ++x) {
                out << m_currentFrame.heatmapMatrix[y][x];
                if (x < 59) out << ",";
            }
            out << "\n";
        }
    }

    if (m_exportMasterStatus) {
        out << "\n--- Master Frame Suffix (128 words) ---\n";
        if (m_currentFrame.rawData.size() >= 5063) {
            const uint8_t* ptr = m_currentFrame.rawData.data() + 4807;
            for (int i = 0; i < 128; ++i) {
                uint16_t val = static_cast<uint16_t>(ptr[i * 2] | (ptr[i * 2 + 1] << 8));
                out << val;
                if (i < 127) {
                    out << (((i + 1) % 16 == 0) ? "\n" : ",");
                }
            }
            out << "\n";
        } else {
            out << "Data unavailable\n";
        }
    }

    if (m_exportSlaveStatus) {
        out << "\n--- Slave Frame Suffix (166 words) ---\n";
        if (m_currentFrame.rawData.size() >= 5402) {
            const uint8_t* ptr = m_currentFrame.rawData.data() + 5070;
            for (int i = 0; i < 166; ++i) {
                uint16_t val = static_cast<uint16_t>(ptr[i * 2] | (ptr[i * 2 + 1] << 8));
                out << val;
                if (i < 165) {
                    out << (((i + 1) % 16 == 0) ? "\n" : ",");
                }
            }
            out << "\n";
        } else {
            out << "Data unavailable\n";
        }
    }

    out.close();
    LOG_INFO("App", "DiagnosticsWorkbench::ExportCurrentFrameToCSV", "UI", "Frame exported to {}", fullPath.string());
}

} // namespace App
