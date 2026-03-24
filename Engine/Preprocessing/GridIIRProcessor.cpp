#include "GridIIRProcessor.h"
#include "imgui.h"
#include <algorithm>
#include <cstring>
#include <cmath>

namespace Engine {

GridIIRProcessor::GridIIRProcessor() {
    std::memset(m_historyBuffer, 0, sizeof(m_historyBuffer));
}

bool GridIIRProcessor::Process(HeatmapFrame& frame) {
    if (!m_enabled) return true;

    if (!m_historyInitialized) {
        std::memcpy(m_historyBuffer, frame.heatmapMatrix, sizeof(m_historyBuffer));
        m_historyInitialized = true;
        return true;
    }

    // Compute per-frame dynamic threshold
    int16_t frameMax = 0;
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 60; ++x) {
            if (frame.heatmapMatrix[y][x] > frameMax) {
                frameMax = frame.heatmapMatrix[y][x];
            }
        }
    }
    const int16_t dynThreshold = static_cast<int16_t>(std::max(
        static_cast<int>(std::lround(frameMax * m_gateRatio)),
        m_gateStaticFloor));

    // Apply per-pixel
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 60; ++x) {
            int16_t current = frame.heatmapMatrix[y][x];
            int16_t history = m_historyBuffer[y][x];

            int16_t filtered = ApplyIIR(current, history, dynThreshold);

            frame.heatmapMatrix[y][x] = filtered;
            m_historyBuffer[y][x] = filtered;
        }
    }

    return true;
}

int16_t GridIIRProcessor::ApplyIIR(int16_t current, int16_t history,
                                    int16_t dynThreshold) {
    // Layer 1: High signal → bypass entirely (protect touch)
    if (current >= dynThreshold) {
        return current;
    }

    // Layer 2: Low signal → aggressive decay IIR
    int32_t val = (static_cast<int32_t>(m_decayWeight) * current
                 + (256 - static_cast<int32_t>(m_decayWeight)) * history) / 256;

    // Extra subtract to accelerate zeroing
    val = std::max(static_cast<int32_t>(0), val - m_decayStep);

    // Layer 3: Hard cutoff → dead black
    if (val < m_noiseFloorCutoff) {
        return 0;
    }
    return static_cast<int16_t>(val);
}

void GridIIRProcessor::DrawConfigUI() {
    ImGui::TextWrapped("Grid IIR v2: Dynamic Gate + Fast Decay");

    ImGui::SeparatorText("Dynamic Touch Gate");
    ImGui::SliderFloat("Gate Ratio (frameMax *)", &m_gateRatio, 0.02f, 0.30f, "%.2f");
    ImGui::SliderInt("Gate Static Floor", &m_gateStaticFloor, 50, 500);

    ImGui::SeparatorText("Low-Signal Decay");
    ImGui::SliderInt("Decay Weight (0-256)", &m_decayWeight, 1, 256);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Higher = more weight on current frame (less smoothing).");
    ImGui::SliderInt("Decay Step (per-frame)", &m_decayStep, 0, 200);
    ImGui::SliderInt("Noise Floor Cutoff", &m_noiseFloorCutoff, 0, 20);

    if (ImGui::Button("Reset History")) {
        m_historyInitialized = false;
    }
}

void GridIIRProcessor::SaveConfig(std::ostream& out) const {
    IFrameProcessor::SaveConfig(out);
    out << "GateRatio=" << m_gateRatio << "\n";
    out << "GateStaticFloor=" << m_gateStaticFloor << "\n";
    out << "DecayWeight=" << m_decayWeight << "\n";
    out << "DecayStep=" << m_decayStep << "\n";
    out << "NoiseFloorCutoff=" << m_noiseFloorCutoff << "\n";
}

void GridIIRProcessor::LoadConfig(const std::string& key, const std::string& value) {
    IFrameProcessor::LoadConfig(key, value);
    if (key == "GateRatio") m_gateRatio = std::stof(value);
    else if (key == "GateStaticFloor") m_gateStaticFloor = std::stoi(value);
    else if (key == "DecayWeight") m_decayWeight = std::stoi(value);
    else if (key == "DecayStep") m_decayStep = std::stoi(value);
    else if (key == "NoiseFloorCutoff") m_noiseFloorCutoff = std::stoi(value);
}

} // namespace Engine
