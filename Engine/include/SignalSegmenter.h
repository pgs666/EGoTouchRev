#pragma once

#include "IFrameProcessor.h"
#include <vector>
#include <string>
#include <cstdint>
#include <utility>

namespace Engine {

class SignalSegmenter : public IFrameProcessor {
public:
    SignalSegmenter();
    virtual ~SignalSegmenter() = default;

    virtual bool Process(Engine::HeatmapFrame& frame) override;
    virtual std::string GetName() const override { return "Signal Segmenter (Phase 1)"; }
    virtual bool IsEnabled() const override { return m_enabled; }
    virtual void SetEnabled(bool enabled) override { m_enabled = enabled; }
    
    // UI configuration interface
    virtual void DrawConfigUI() override;
    
    void SaveConfig(std::ostream& out) const override {
        IFrameProcessor::SaveConfig(out);
        out << "BaseThreshold=" << m_baseThreshold << "\n";
        out << "HighSignalRatio=" << m_highSignalRatio << "\n";
        out << "MediumSignalRatio=" << m_mediumSignalRatio << "\n";
        out << "DampDist1=" << m_dampDist1 << "\n";
        out << "DampDistSqrt2=" << m_dampDistSqrt2 << "\n";
        out << "DampDist2=" << m_dampDist2 << "\n";
    }
    
    void LoadConfig(const std::string& key, const std::string& value) override {
        IFrameProcessor::LoadConfig(key, value);
        if (key == "BaseThreshold") m_baseThreshold = std::stoi(value);
        else if (key == "HighSignalRatio") m_highSignalRatio = std::stof(value);
        else if (key == "MediumSignalRatio") m_mediumSignalRatio = std::stof(value);
        else if (key == "DampDist1") m_dampDist1 = std::stof(value);
        else if (key == "DampDistSqrt2") m_dampDistSqrt2 = std::stof(value);
        else if (key == "DampDist2") m_dampDist2 = std::stof(value);
    }

    // Temporary accessor to allow UI visualization of found peaks
    const std::vector<std::pair<int, int>>& GetDebugPeaks() const { return m_debugPeaks; }

private:
    bool m_enabled = false; // Default off, so it doesn't conflict with existing until tested

    // Tunable Parameters
    int m_baseThreshold = 50;        // Minimum heatmap value to be considered a 'touch pixel'
    float m_highSignalRatio = 0.45f;  // Lowered to 0.45f to catch fainter secondary peaks
    float m_mediumSignalRatio = 0.4f;

    // Iterative Damping Parameters
    float m_dampDist1 = 0.8f;     // Subtraction ratio for immediate neighbors (Dist^2 = 1)
    float m_dampDistSqrt2 = 0.5f; // Subtraction ratio for diagonal neighbors (Dist^2 = 2)
    float m_dampDist2 = 0.2f;     // Subtraction ratio for R=2 neighbors (Dist^2 = 4)

    // Internal working structures
    struct Pixel {
        int r, c;
        int val;
    };
    
    struct Blob {
        std::vector<Pixel> pixels;
        int maxVal = 0;
    };

    // State for visualization
    std::vector<std::pair<int, int>> m_debugPeaks;

    // Helper functions
    void FindBlobs(const Engine::HeatmapFrame& frame, std::vector<Blob>& outBlobs);
    void FindPeaksInBlob(const Engine::HeatmapFrame& frame, const Blob& blob, std::vector<std::pair<int, int>>& outPeaks);
};

} // namespace Engine
