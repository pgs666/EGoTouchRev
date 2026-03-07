#pragma once

#include "IFrameProcessor.h"
#include <vector>
#include <array>
#include <cstdint>
#include <string>

namespace Engine {

class FeatureExtractor : public IFrameProcessor {
public:
    FeatureExtractor();
    ~FeatureExtractor() override = default;

    bool Process(HeatmapFrame& frame) override;
    std::string GetName() const override { return "Feature Extractor (4.1/4.2)"; }
    
    void DrawConfigUI() override;
    void SaveConfig(std::ostream& out) const override;
    void LoadConfig(const std::string& key, const std::string& value) override;

    struct Peak {
        int r, c;
        int16_t z; // Signal strength
    };

    const std::vector<Peak>& GetPeaks() const { return m_peaks; }
    const std::array<uint8_t, 2400>& GetTouchZones() const { return m_touchZones; }

private:
    int16_t m_baseThreshold = 30; // 4.1 Peak base threshold
    float m_zoneThreshRatio = 0.33f; // 4.2 Dynamic Edge Threshold (Peak * Ratio)
    int m_morphPasses = 1; // 4.2 Morphology Passes (Dilation -> Erosion)
    int m_minAreaThreshold = 4; // 4.3 Pre-Filter: Minimum area to be considered a touch
    bool m_edgeSuppression = false; // TSA_MSPeakFilter: Suppress weak ghost peaks on screen edges
    float m_edgeSuppressionRatio = 0.625f; // Threshold ratio: 0.625 = 5/8
    int m_edgeSuppressionMargin = 1; // Distance from absolute edge to search for peaks (1 = r:1, rows-2)

    bool m_ecEnabled = true; // CTD_ECProcess: Edge Compensation
    float m_ecMargin = 1.5f; // Distance from edge (pixels) to start applying compensation
    float m_ecMaxOffset = 0.5f; // Maximum pixel expansion push at the very edge

    std::vector<Peak> m_peaks;
    std::array<uint8_t, 2400> m_touchZones;

    void DetectPeaks(const HeatmapFrame& frame);
    void GenerateTouchZones(const HeatmapFrame& frame);
    void CalculateCentroids(HeatmapFrame& frame);
};

} // namespace Engine
