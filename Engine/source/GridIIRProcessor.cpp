#include "GridIIRProcessor.h"
#include "imgui.h"
#include <algorithm>
#include <cstring>

namespace Engine {

GridIIRProcessor::GridIIRProcessor() {
    std::memset(m_historyBuffer, 0, sizeof(m_historyBuffer));
}

bool GridIIRProcessor::Process(HeatmapFrame& frame) {
    if (!m_enabled) return true;

    if (!m_historyInitialized) {
        // First frame: Copy to history and skip filtering
        std::memcpy(m_historyBuffer, frame.heatmapMatrix, sizeof(m_historyBuffer));
        m_historyInitialized = true;
        return true;
    }

    // Apply temporal IIR pixel-by-pixel
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 60; ++x) {
            int16_t current = frame.heatmapMatrix[y][x];
            int16_t history = m_historyBuffer[y][x];
            
            int16_t filtered = ApplyIIR(current, history);
            
            // Output filtered result to current frame and update history
            frame.heatmapMatrix[y][x] = filtered;
            m_historyBuffer[y][x] = filtered;
        }
    }

    return true;
}

int16_t GridIIRProcessor::ApplyIIR(int16_t current, int16_t history) {
    // Exact logic from TSA TSACore's AdaptiveIIR_Process
    // 1. Check if the signal change is "small enough" to be considered noise
    int32_t delta = std::abs(static_cast<int32_t>(current) - static_cast<int32_t>(history));
    
    if (delta >= m_adaptiveThreshold) {
        // Large change (e.g. finger moving): Bypass filter to avoid ghosting
        return current;
    }

    // 2. Perform IIR: IIR_Val = (Weight * Current + (256 - Weight) * History) / 256
    int32_t val = (static_cast<int32_t>(m_weight) * current + 
                   (256 - static_cast<int32_t>(m_weight)) * history) / 256;
    
    // Applying the rounding/convergence adjustment found in original assembly
    int16_t adj = 0;
    if (val < current) {
        adj = 1;
    } else if (current < val) {
        adj = -1;
    }
    
    return static_cast<int16_t>(val + adj);
}

void GridIIRProcessor::DrawConfigUI() {
    ImGui::TextWrapped("Adaptive Grid IIR Temporal Filter");
    ImGui::TextWrapped("Filters small jitters while preserving fast movement.");
    
    ImGui::SliderInt("IIR Weight", &m_weight, 1, 256);
    if (ImGui::IsItemHovered()) 
        ImGui::SetTooltip("Lower weight = More smoothing (but more lag for small signals).");

    ImGui::SliderInt("Adaptive Threshold", &m_adaptiveThreshold, 0, 500);
    if (ImGui::IsItemHovered()) 
        ImGui::SetTooltip("If signal change > Threshold, IIR is bypassed. Keeps fingers crisp.");

    if (ImGui::Button("Reset History")) {
        m_historyInitialized = false;
    }
}

} // namespace Engine
