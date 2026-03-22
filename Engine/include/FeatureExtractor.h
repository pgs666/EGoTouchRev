#pragma once

#include "IFrameProcessor.h"
#include "EdgeCompensation.h"
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
    bool m_z8FilterEnabled = true;  // 4.1 Peak Z8: neighborCount < signal/32 → remove
    bool m_pressureDriftEnabled = false; // 4.1 PressureDrift: reject uniform RX-line peaks
    int m_pressureDriftThreshold = 220;  // sigTholdLimit equivalent
    float m_zoneThreshRatio = 0.33f; // 4.2 Dynamic Edge Threshold (Peak * Ratio)
    float m_tzNormalCoeff = 0.50f;   // 4.2 TZ threshold = min(sigThold,peak) * coeff (normal)
    float m_tzEdgeCoeff = 0.50f;     // 4.2 TZ threshold for edge peaks
    int m_tzEdgeMargin = 2;          // 4.2 Pixels from border to be considered "edge"
    int m_morphPasses = 1; // 4.2 Morphology Passes (Dilation -> Erosion)
    int m_minAreaThreshold = 4; // 4.3 Pre-Filter: Minimum area to be considered a touch
    int m_minSignalSum = 0; // 4.3 Pre-Filter: Minimum weighted sum in a zone
    bool m_touchPreFilterEnabled = true; // TSA_MSTouchPreFilter (lite)
    int m_sigSumReserveCount = 20; // SigSumFilter_ReserveTouch: keep top-N by signal sum
    bool m_rxLineFilterEnabled = false; // RxLineFilter IsZ8Failed core (disabled by default)
    int m_rxLineDelta = 0; // Same-line delta in RX dimension (rounded coordinate)
    float m_rxLineWeakRatio = 0.5f; // Remove if weak touch is below strong * ratio
    bool m_subZoneSplitEnabled = true; // SubTZ-like split for multi-peak blobs
    int m_subZoneMinArea = 3; // Regret threshold: too-small sub-zone merges back
    int m_subZoneMinSignal = 120; // Regret threshold: too-weak sub-zone merges back
    float m_subZoneMinCentroidDist = 1.8f; // Regret threshold: too-close centroids merge
    bool m_edgeSuppression = false; // TSA_MSPeakFilter: Suppress weak ghost peaks on screen edges
    float m_edgeSuppressionRatio = 0.625f; // Threshold ratio: 0.625 = 5/8
    int m_edgeSuppressionMargin = 1; // Distance from absolute edge to search for peaks

    // Gourd Shape Split: 葫芦形触点分裂
    bool m_gourdSplitEnabled = true;
    float m_gourdConstrictionRatio = 0.60f;  // 腰宽/最大宽 < 此值 → 判定为葫芦
    int m_gourdMinLobeArea = 3;              // 分裂后每瓣最小面积
    float m_gourdAnalysisRatio = 0.10f;      // 形态分析用低阈值 = peak * 此值

    // CTD_ECProcess: Official 1:1 LUT-based Edge Compensation
    bool m_ecEnabled = true;
    int m_ecEdgeWidth = 2; // How many boundary pixels define the "edge zone"
    // 4 edge profiles: [0]=Dim1Near(left), [1]=Dim1Far(right), [2]=Dim2Near(top), [3]=Dim2Far(bottom)
    ECProfile m_ecProfiles[4];

    // Peak Pruning (波峰剪枝) logic to prevent large area splitting
    bool m_peakMergingEnabled = true;
    float m_peakMergingDistThresh = 3.5f;
    float m_peakMergingGapRatio = 0.20f; // Drop required to be a 'gap' (1/4 as seen in Ghidra)

    std::vector<Peak> m_peaks;
    std::array<uint8_t, 2400> m_touchZones;
    int m_zoneCount = 0;

    void DetectPeaks(const HeatmapFrame& frame);
    void Z8FilterPeaks(const HeatmapFrame& frame);
    bool IsPressureDrift(const HeatmapFrame& frame, const Peak& peak);
    void PrunePeaks(const HeatmapFrame& frame);
    void GenerateTouchZones(const HeatmapFrame& frame);
    void CalculateCentroids(HeatmapFrame& frame);
    void ApplyTouchPreFilter(HeatmapFrame& frame);
    
    bool IsClearSignalGap(const HeatmapFrame& frame, const Peak& p1, const Peak& p2);
    void GourdShapeSplit(const HeatmapFrame& frame);
    
    // Official CTD_ECProcess 1:1 replica
    void ApplyEdgeCompensation(float& outX, float& outY, int cols, int rows);
};

} // namespace Engine
