#pragma once

#include "FramePipeline.h"
#include <string>
#include <vector>

namespace Engine {

// Grid IIR Processor
// Replicates TSA's GridIIR_Process as a Temporal Low-pass Filter.
// Uses an Infinite Impulse Response (IIR) approach on the 2D grid data
// to smooth out noise across consecutive frames.
class GridIIRProcessor : public IFrameProcessor {
public:
    GridIIRProcessor();
    ~GridIIRProcessor() override = default;

    bool Process(HeatmapFrame& frame) override;
    std::string GetName() const override { return "Grid IIR Processor"; }

    void DrawConfigUI() override;
    
    void SaveConfig(std::ostream& out) const override {
        IFrameProcessor::SaveConfig(out);
        out << "IIRWeight=" << m_weight << "\n";
        out << "AdaptiveThreshold=" << m_adaptiveThreshold << "\n";
    }
    
    void LoadConfig(const std::string& key, const std::string& value) override {
        IFrameProcessor::LoadConfig(key, value);
        if (key == "IIRWeight") m_weight = std::stoi(value);
        if (key == "AdaptiveThreshold") m_adaptiveThreshold = std::stoi(value);
    }

private:
    int m_weight = 120; // 0-256 scale. Equivalent to alpha. Low = high inertia/smoothing.
    int m_adaptiveThreshold = 15; // If |current - history| > threshold, bypass IIR.
    
    // History buffer for the temporal filter
    bool m_historyInitialized = false;
    int16_t m_historyBuffer[40][60];

    // Core Adaptive IIR math from TSA's AdaptiveIIR_Process
    int16_t ApplyIIR(int16_t current, int16_t history);
};

} // namespace Engine
