#include "FeatureExtractor.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace Engine {

FeatureExtractor::FeatureExtractor() {
    m_touchZones.fill(0);
    for (int i = 0; i < 4; ++i) {
        m_ecProfiles[i] = g_defaultECProfiles[i];
    }
}

bool FeatureExtractor::Process(HeatmapFrame& frame) {
    if (!m_enabled) return true;

    // 4.1
    DetectPeaks(frame);
    
    // 4.1.b Z8 Filter: remove isolated noise peaks
    if (m_z8FilterEnabled) {
        Z8FilterPeaks(frame);
    }
    
    // 4.1.c PressureDrift: remove uniform RX-line false peaks
    if (m_pressureDriftEnabled && !m_peaks.empty()) {
        m_peaks.erase(std::remove_if(m_peaks.begin(), m_peaks.end(),
            [&](const Peak& p) { return IsPressureDrift(frame, p); }), m_peaks.end());
    }
    
    // 4.1.d Prune Peaks (Suppress large area splitting)
    if (m_peakMergingEnabled) {
        PrunePeaks(frame);
    }
    
    // 4.2
    GenerateTouchZones(frame);

    // 4.2.b Gourd Shape Split: detect and split gourd-shaped single-peak zones
    if (m_gourdSplitEnabled) {
        GourdShapeSplit(frame);
    }
    
    // 4.3 (PreFilter & Centroid)
    CalculateCentroids(frame);
    
    return true;
}

void FeatureExtractor::DetectPeaks(const HeatmapFrame& frame) {
    m_peaks.clear();
    const int rows = 40;
    const int cols = 60;

    auto getVal = [&](int rr, int cc) -> int16_t {
        if (rr < 0 || rr >= rows || cc < 0 || cc >= cols) return 0; // Assume 0 energy outside screen
        return frame.heatmapMatrix[rr][cc];
    };

    auto checkPeak = [&](int r, int c, int16_t val) {
        if (val >= m_baseThreshold) {
            if (getVal(r, c+1) <= val &&
                getVal(r+1, c) <= val &&
                getVal(r+1, c+1) <= val &&
                getVal(r+1, c-1) <= val) 
            {
                if (getVal(r, c-1) < val &&
                    getVal(r-1, c) < val &&
                    getVal(r-1, c-1) < val &&
                    getVal(r-1, c+1) < val) 
                {
                    m_peaks.push_back({r, c, val});
                }
            }
        }
    };

    // 1. Detect Inner Peaks
    for (int r = 1; r < rows - 1; ++r) {
        for (int c = 1; c < cols - 1; ++c) {
            checkPeak(r, c, frame.heatmapMatrix[r][c]);
        }
    }

    // 2. Detect Edge Peaks Separately
    // Top & Bottom edges
    for (int c = 0; c < cols; ++c) {
        checkPeak(0, c, frame.heatmapMatrix[0][c]);
        checkPeak(rows - 1, c, frame.heatmapMatrix[rows - 1][c]);
    }
    // Left & Right edges (excluding corners already checked)
    for (int r = 1; r < rows - 1; ++r) {
        checkPeak(r, 0, frame.heatmapMatrix[r][0]);
        checkPeak(r, cols - 1, frame.heatmapMatrix[r][cols - 1]);
    }

    // Sort descending by signal strength
    std::sort(m_peaks.begin(), m_peaks.end(), [](const Peak& a, const Peak& b) {
        return a.z > b.z;
    });

    // TSA_MSPeakFilter: Edge Peak Suppression Workaround
    if (m_edgeSuppression && !m_peaks.empty()) {
        auto suppressEdge = [&](auto condition) {
            int max_z = 0;
            for (const auto& p : m_peaks) {
                if (condition(p)) max_z = std::max(max_z, (int)p.z);
            }
            if (max_z > 0) {
                int thresh = static_cast<int>(max_z * m_edgeSuppressionRatio);
                m_peaks.erase(std::remove_if(m_peaks.begin(), m_peaks.end(), 
                    [&](const Peak& p) { return condition(p) && p.z < thresh; }), m_peaks.end());
            }
        };
        suppressEdge([this](const Peak& p) { return p.r <= m_edgeSuppressionMargin; });
        suppressEdge([&](const Peak& p) { return p.r >= rows - 1 - m_edgeSuppressionMargin; });
        suppressEdge([this](const Peak& p) { return p.c <= m_edgeSuppressionMargin; });
        suppressEdge([&](const Peak& p) { return p.c >= cols - 1 - m_edgeSuppressionMargin; });
    }

    // 4.1 Insight: Cap the maximum number of peaks to 20 (0x14)
    if (m_peaks.size() > 20) {
        m_peaks.resize(20);
    }
}

void FeatureExtractor::Z8FilterPeaks(const HeatmapFrame& frame) {
    const int rows = 40;
    const int cols = 60;

    auto getVal = [&](int r, int c) -> int16_t {
        if (r < 0 || r >= rows || c < 0 || c >= cols) return 0;
        return frame.heatmapMatrix[r][c];
    };

    m_peaks.erase(std::remove_if(m_peaks.begin(), m_peaks.end(),
        [&](const Peak& p) {
            int neighborCount = 0;
            static const int dr[] = {-1,-1,-1, 0, 0, 1, 1, 1};
            static const int dc[] = {-1, 0, 1,-1, 1,-1, 0, 1};
            for (int d = 0; d < 8; ++d) {
                if (getVal(p.r + dr[d], p.c + dc[d]) > 0) {
                    neighborCount++;
                }
            }
            int threshold = p.z >> 5;  // signal / 32
            return neighborCount < threshold;
        }), m_peaks.end());
}

bool FeatureExtractor::IsPressureDrift(const HeatmapFrame& frame, const Peak& peak) {
    const int cols = 60;
    const int16_t sigLimit = static_cast<int16_t>(m_pressureDriftThreshold);
    const int16_t upperBound = static_cast<int16_t>(sigLimit * 3 / 4);
    const int16_t lowerBound = static_cast<int16_t>(sigLimit * 3 / 8);

    // Only check mid-range signals
    if (peak.z > upperBound || peak.z < lowerBound) return false;

    // Scan entire row
    int gradientSum = 0;
    int rowSignalSum = 0;
    for (int c = 1; c < cols - 1; ++c) {
        int16_t val = frame.heatmapMatrix[peak.r][c];
        int16_t prev = frame.heatmapMatrix[peak.r][c - 1];
        int gradient = std::abs(static_cast<int>(val) - static_cast<int>(prev));
        if (gradient > sigLimit / 3) {
            return false;  // Sharp local peak exists → not drift
        }
        gradientSum += gradient;
        if (val > 0) rowSignalSum += val;
    }

    // Drift: uniform signal across row
    if (rowSignalSum < (peak.z * 9) / 2) return false;  // Not enough total
    if (peak.z * 6 < gradientSum) return false;          // Too much variation
    return true;  // Uniform distribution → pressure drift
}

void FeatureExtractor::PrunePeaks(const HeatmapFrame& frame) {
    if (m_peaks.size() < 2) return;

    bool changed = true;
    while (changed && m_peaks.size() >= 2) {
        changed = false;
        for (size_t i = 0; i + 1 < m_peaks.size(); ++i) {
            for (size_t j = i + 1; j < m_peaks.size(); ++j) {
                const auto& p1 = m_peaks[i];
                const auto& p2 = m_peaks[j];

                const float dr = static_cast<float>(p1.r - p2.r);
                const float dc = static_cast<float>(p1.c - p2.c);
                const float distSq = dr * dr + dc * dc;
                if (distSq >= (m_peakMergingDistThresh * m_peakMergingDistThresh)) {
                    continue;
                }

                if (IsClearSignalGap(frame, p1, p2)) {
                    continue;
                }

                // PeakFilterOnce-like behavior: remove one weaker peak and restart.
                const size_t removeIdx = (p1.z >= p2.z) ? j : i;
                m_peaks.erase(m_peaks.begin() + static_cast<std::ptrdiff_t>(removeIdx));
                changed = true;
                break;
            }
            if (changed) {
                break;
            }
        }
    }
}

bool FeatureExtractor::IsClearSignalGap(const HeatmapFrame& frame, const Peak& p1, const Peak& p2) {
    // Bresenham-like or simple linear sampling between p1 and p2
    const int steps = 5;
    int16_t maxVal = std::max(p1.z, p2.z);
    
    // Himax heuristic: if the minimum value along the path drops below 
    // a certain percentage of the peak energy, it's a clear gap.
    // TZ_IsClearSignalGapAlongDim1 uses: (Max - Min) <= (Max >> 2) -> Not a gap
    
    int16_t absoluteMinAlongPath = maxVal;

    for (int i = 1; i < steps; ++i) {
        float t = static_cast<float>(i) / steps;
        int r = static_cast<int>(p1.r + (p2.r - p1.r) * t + 0.5f);
        int c = static_cast<int>(p1.c + (p2.c - p1.c) * t + 0.5f);
        
        if (r >= 0 && r < 40 && c >= 0 && c < 60) {
            int16_t val = frame.heatmapMatrix[r][c];
            if (val < absoluteMinAlongPath) absoluteMinAlongPath = val;
        }
    }

    // If the deepest point in the valley between peaks is still quite high, it's NO GAP.
    // Threshold: if valley_depth (Max - Min) > (Max * Ratio), then it IS a gap.
    int16_t valleyDepth = maxVal - absoluteMinAlongPath;
    int16_t gapThreshold = static_cast<int16_t>(maxVal * m_peakMergingGapRatio);

    return (valleyDepth > gapThreshold);
}

void FeatureExtractor::GenerateTouchZones(const HeatmapFrame& frame) {
    m_touchZones.fill(0);
    m_zoneCount = 0;
    if (m_peaks.empty()) {
        return;
    }

    const int rows = 40;
    const int cols = 60;

    static const int dr8[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    static const int dc8[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const int dr4[] = {-1, 1, 0, 0};
    static const int dc4[] = {0, 0, -1, 1};

    struct BlobComponent {
        std::vector<int> pixels;
        std::vector<int> peakIndices;
    };

    std::array<uint8_t, 2400> visited;
    visited.fill(0);
    std::array<uint16_t, 2400> componentId;
    componentId.fill(0);

    std::vector<BlobComponent> components;
    components.reserve(32);

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int idx = r * cols + c;
            if (visited[idx]) {
                continue;
            }
            visited[idx] = 1;
            if (frame.heatmapMatrix[r][c] < m_baseThreshold) {
                continue;
            }

            BlobComponent comp;
            std::queue<int> q;
            q.push(idx);
            componentId[idx] = static_cast<uint16_t>(components.size() + 1);

            while (!q.empty()) {
                const int curIdx = q.front();
                q.pop();
                comp.pixels.push_back(curIdx);

                const int cr = curIdx / cols;
                const int cc = curIdx % cols;

                for (int d = 0; d < 8; ++d) {
                    const int nr = cr + dr8[d];
                    const int nc = cc + dc8[d];
                    if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) {
                        continue;
                    }

                    const int nIdx = nr * cols + nc;
                    if (visited[nIdx]) {
                        continue;
                    }
                    visited[nIdx] = 1;
                    if (frame.heatmapMatrix[nr][nc] < m_baseThreshold) {
                        continue;
                    }

                    componentId[nIdx] = static_cast<uint16_t>(components.size() + 1);
                    q.push(nIdx);
                }
            }

            if (!comp.pixels.empty()) {
                components.push_back(std::move(comp));
            }
        }
    }

    for (size_t peakIdx = 0; peakIdx < m_peaks.size(); ++peakIdx) {
        const auto& peak = m_peaks[peakIdx];
        if (peak.r < 0 || peak.r >= rows || peak.c < 0 || peak.c >= cols) {
            continue;
        }
        const int idx = peak.r * cols + peak.c;
        const uint16_t cid = componentId[idx];
        if (cid == 0) {
            continue;
        }
        components[static_cast<size_t>(cid - 1)].peakIndices.push_back(static_cast<int>(peakIdx));
    }

    for (size_t compIdx = 0; compIdx < components.size(); ++compIdx) {
        auto& comp = components[compIdx];
        if (comp.peakIndices.empty()) {
            continue;
        }
        if (m_zoneCount >= 20) {
            break;
        }

        auto bestPeakIt = std::max_element(
            comp.peakIndices.begin(), comp.peakIndices.end(),
            [&](int a, int b) { return m_peaks[static_cast<size_t>(a)].z < m_peaks[static_cast<size_t>(b)].z; });
        const int strongestPeakIdx = *bestPeakIt;
        const auto& strongestPeak = m_peaks[static_cast<size_t>(strongestPeakIdx)];
        const int16_t strongestThreshold = static_cast<int16_t>(std::max(
            static_cast<int>(m_baseThreshold),
            static_cast<int>(std::min(static_cast<int>(strongestPeak.z),
                static_cast<int>(m_baseThreshold * 100)) *  // cap at sigThold-equivalent
                ((strongestPeak.r < m_tzEdgeMargin ||
                  strongestPeak.r >= 40 - m_tzEdgeMargin ||
                  strongestPeak.c < m_tzEdgeMargin ||
                  strongestPeak.c >= 60 - m_tzEdgeMargin)
                    ? m_tzEdgeCoeff : m_tzNormalCoeff))));

        const bool shouldSplit = m_subZoneSplitEnabled && comp.peakIndices.size() > 1;
        if (!shouldSplit) {
            const uint8_t zoneId = static_cast<uint8_t>(++m_zoneCount);
            for (const int idx : comp.pixels) {
                const int r = idx / cols;
                const int c = idx % cols;
                if (frame.heatmapMatrix[r][c] >= strongestThreshold) {
                    m_touchZones[idx] = zoneId;
                }
            }
            continue;
        }

        struct SubSeed {
            uint8_t localId = 0;
            int peakIndex = -1;
            int r = 0;
            int c = 0;
            int16_t threshold = 0;
            int16_t peakStrength = 0;
        };

        std::vector<SubSeed> seeds;
        seeds.reserve(comp.peakIndices.size());
        for (const int peakIndex : comp.peakIndices) {
            const auto& peak = m_peaks[static_cast<size_t>(peakIndex)];
            SubSeed seed;
            seed.peakIndex = peakIndex;
            seed.r = peak.r;
            seed.c = peak.c;
            seed.peakStrength = peak.z;
            seed.threshold = static_cast<int16_t>(std::max(
                static_cast<int>(m_baseThreshold),
                static_cast<int>(peak.z * m_zoneThreshRatio)));
            seeds.push_back(seed);
        }
        std::sort(seeds.begin(), seeds.end(), [](const SubSeed& a, const SubSeed& b) {
            return a.peakStrength > b.peakStrength;
        });
        for (size_t i = 0; i < seeds.size(); ++i) {
            seeds[i].localId = static_cast<uint8_t>(i + 1);
        }

        std::array<uint8_t, 2400> localAssign;
        localAssign.fill(0);
        std::array<int16_t, 2400> localStrength;  // Bottleneck path strength (min signal along path)
        localStrength.fill(std::numeric_limits<int16_t>::min());

        // Fix C: Priority queue (signal-descending) for deterministic water-fill.
        auto pqCmp = [&](int a, int b) {
            return frame.heatmapMatrix[a / cols][a % cols]
                 < frame.heatmapMatrix[b / cols][b % cols];
        };
        std::priority_queue<int, std::vector<int>, decltype(pqCmp)> q(pqCmp);
        const uint16_t currentCompId = static_cast<uint16_t>(compIdx + 1);
        for (const auto& seed : seeds) {
            const int seedIdx = seed.r * cols + seed.c;
            if (componentId[seedIdx] != currentCompId) {
                continue;
            }
            localAssign[seedIdx] = seed.localId;
            localStrength[seedIdx] = seed.peakStrength;
            q.push(seedIdx);
        }

        while (!q.empty()) {
            const int idx = q.top();
            q.pop();

            const uint8_t owner = localAssign[idx];
            if (owner == 0 || owner > seeds.size()) {
                continue;
            }

            const auto& ownerSeed = seeds[static_cast<size_t>(owner - 1)];
            const int r = idx / cols;
            const int c = idx % cols;

            for (int d = 0; d < 8; ++d) {
                const int nr = r + dr8[d];
                const int nc = c + dc8[d];
                if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) {
                    continue;
                }

                const int nIdx = nr * cols + nc;
                if (componentId[nIdx] != currentCompId) {
                    continue;
                }

                const int16_t nVal = frame.heatmapMatrix[nr][nc];
                if (nVal < ownerSeed.threshold) {
                    continue;
                }

                // Fix A: Bottleneck path strength = min(parent's path strength, this pixel)
                const int16_t newPathStrength = std::min(localStrength[idx], nVal);

                const uint8_t assigned = localAssign[nIdx];
                if (assigned == 0) {
                    localAssign[nIdx] = owner;
                    localStrength[nIdx] = newPathStrength;
                    q.push(nIdx);
                    continue;
                }
                if (assigned == owner) {
                    // Same owner re-visit: update if new path has higher bottleneck
                    if (newPathStrength > localStrength[nIdx]) {
                        localStrength[nIdx] = newPathStrength;
                        q.push(nIdx);
                    }
                    continue;
                }
                if (assigned > seeds.size()) {
                    continue;
                }

                // Fix B: Euclidean distance squared for ownership contest
                const auto& oldSeed = seeds[static_cast<size_t>(assigned - 1)];
                const int oldDistSq = (oldSeed.r - nr) * (oldSeed.r - nr) + (oldSeed.c - nc) * (oldSeed.c - nc);
                const int newDistSq = (ownerSeed.r - nr) * (ownerSeed.r - nr) + (ownerSeed.c - nc) * (ownerSeed.c - nc);

                const bool strongerPath = (newPathStrength > localStrength[nIdx]) && (newDistSq <= oldDistSq);
                const bool significantlyCloser = (newDistSq + 2) < oldDistSq;
                if (strongerPath || significantlyCloser) {
                    localAssign[nIdx] = owner;
                    localStrength[nIdx] = newPathStrength;
                    q.push(nIdx);
                }
            }
        }

        struct SubZoneStat {
            int area = 0;
            long long signal = 0;
            long long weightedX = 0;
            long long weightedY = 0;
            bool valid = false;
        };

        std::vector<SubZoneStat> stats(seeds.size());
        for (const int idx : comp.pixels) {
            const uint8_t owner = localAssign[idx];
            if (owner == 0 || owner > stats.size()) {
                continue;
            }
            const int r = idx / cols;
            const int c = idx % cols;
            const int value = std::max(0, static_cast<int>(frame.heatmapMatrix[r][c]));
            auto& st = stats[static_cast<size_t>(owner - 1)];
            st.area += 1;
            st.signal += value;
            st.weightedX += static_cast<long long>(value) * c;
            st.weightedY += static_cast<long long>(value) * r;
        }

        for (size_t i = 0; i < stats.size(); ++i) {
            stats[i].valid = (stats[i].area >= m_subZoneMinArea) &&
                             (stats[i].signal >= m_subZoneMinSignal);
        }

        const float minDistSq = m_subZoneMinCentroidDist * m_subZoneMinCentroidDist;
        for (size_t i = 0; i < stats.size(); ++i) {
            if (!stats[i].valid || stats[i].signal <= 0) {
                continue;
            }
            for (size_t j = i + 1; j < stats.size(); ++j) {
                if (!stats[j].valid || stats[j].signal <= 0) {
                    continue;
                }

                const float x1 = static_cast<float>(stats[i].weightedX) / static_cast<float>(stats[i].signal);
                const float y1 = static_cast<float>(stats[i].weightedY) / static_cast<float>(stats[i].signal);
                const float x2 = static_cast<float>(stats[j].weightedX) / static_cast<float>(stats[j].signal);
                const float y2 = static_cast<float>(stats[j].weightedY) / static_cast<float>(stats[j].signal);
                const float dx = x1 - x2;
                const float dy = y1 - y2;
                const float distSq = dx * dx + dy * dy;
                if (distSq < minDistSq) {
                    if (stats[i].signal >= stats[j].signal) {
                        stats[j].valid = false;
                    } else {
                        stats[i].valid = false;
                    }
                }
            }
        }

        size_t strongestLocal = 0;
        long long strongestScore = std::numeric_limits<long long>::min();
        for (size_t i = 0; i < stats.size(); ++i) {
            const long long score = (stats[i].signal > 0) ? stats[i].signal : seeds[i].peakStrength;
            if (score > strongestScore) {
                strongestScore = score;
                strongestLocal = i;
            }
        }

        int validCount = 0;
        for (const auto& st : stats) {
            if (st.valid) {
                validCount += 1;
            }
        }

        if (validCount < 2) {
            const uint8_t zoneId = static_cast<uint8_t>(++m_zoneCount);
            const int16_t fallbackThreshold = seeds[strongestLocal].threshold;
            for (const int idx : comp.pixels) {
                const int r = idx / cols;
                const int c = idx % cols;
                if (frame.heatmapMatrix[r][c] >= fallbackThreshold) {
                    m_touchZones[idx] = zoneId;
                }
            }
            continue;
        }

        std::vector<uint8_t> localToGlobal(stats.size(), 0);
        for (size_t i = 0; i < stats.size(); ++i) {
            if (!stats[i].valid) {
                continue;
            }
            if (m_zoneCount >= 20) {
                break;
            }
            localToGlobal[i] = static_cast<uint8_t>(++m_zoneCount);
        }

        size_t fallbackValid = strongestLocal;
        if (fallbackValid >= localToGlobal.size() || localToGlobal[fallbackValid] == 0) {
            fallbackValid = 0;
            for (size_t i = 0; i < localToGlobal.size(); ++i) {
                if (localToGlobal[i] != 0) {
                    fallbackValid = i;
                    break;
                }
            }
        }

        for (const int idx : comp.pixels) {
            const int r = idx / cols;
            const int c = idx % cols;
            const int16_t val = frame.heatmapMatrix[r][c];
            const uint8_t owner = localAssign[idx];
            if (owner > 0 && owner <= localToGlobal.size()) {
                const size_t ownerIdx = static_cast<size_t>(owner - 1);
                if (localToGlobal[ownerIdx] != 0 && val >= seeds[ownerIdx].threshold) {
                    m_touchZones[idx] = localToGlobal[ownerIdx];
                    continue;
                }
            }
            if (fallbackValid < localToGlobal.size() && localToGlobal[fallbackValid] != 0 &&
                val >= seeds[fallbackValid].threshold) {
                m_touchZones[idx] = localToGlobal[fallbackValid];
            }
        }
    }

    int maxZoneId = 0;
    for (const uint8_t zoneId : m_touchZones) {
        if (zoneId > maxZoneId) {
            maxZoneId = zoneId;
        }
    }
    m_zoneCount = maxZoneId;

    // 4.2 Insight: Morphological Transform (Closing) to smooth borders and fill holes
    for (int pass = 0; pass < m_morphPasses; ++pass) {
        std::array<uint8_t, 2400> tempBuf = m_touchZones;

        // Step 1: Dilation (Grow 1 pixel)
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (m_touchZones[r * cols + c] == 0) {
                    for (int i = 0; i < 4; ++i) {
                        int nr = r + dr4[i];
                        int nc = c + dc4[i];
                        if (nr >= 0 && nr < rows && nc >= 0 && nc < cols) {
                            if (m_touchZones[nr * cols + nc] > 0) {
                                tempBuf[r * cols + c] = m_touchZones[nr * cols + nc];
                                break;
                            }
                        }
                    }
                }
            }
        }
        m_touchZones = tempBuf;
        tempBuf = m_touchZones;

        // Step 2: Erosion (Shrink 1 pixel)
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (m_touchZones[r * cols + c] > 0) {
                    bool keep = true;
                    for (int i = 0; i < 4; ++i) {
                        int nr = r + dr4[i];
                        int nc = c + dc4[i];
                        if (nr >= 0 && nr < rows && nc >= 0 && nc < cols) {
                            if (m_touchZones[nr * cols + nc] == 0) {
                                keep = false;
                                break;
                            }
                        } else {
                            // keep = false; break; // DO NOT erode on absolute screen boundaries
                        }
                    }
                    if (!keep) {
                        tempBuf[r * cols + c] = 0;
                    }
                }
            }
        }
        m_touchZones = tempBuf;
    }
}

void FeatureExtractor::GourdShapeSplit(const HeatmapFrame& frame) {
    const int rows = 40;
    const int cols = 60;
    static const int dr8[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    static const int dc8[] = {-1, 0, 1, -1, 1, -1, 0, 1};

    // Snapshot current zone count; only iterate zones that existed before this pass.
    const int originalZoneCount = m_zoneCount;

    for (int zoneId = 1; zoneId <= originalZoneCount; ++zoneId) {
        if (m_zoneCount >= 20) break;

        // 1a. Find the strongest pixel in this zone (the zone's "peak").
        int peakR = -1, peakC = -1;
        int16_t peakSig = 0;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (m_touchZones[r * cols + c] == static_cast<uint8_t>(zoneId)) {
                    int16_t sig = frame.heatmapMatrix[r][c];
                    if (sig > peakSig) {
                        peakSig = sig;
                        peakR = r;
                        peakC = c;
                    }
                }
            }
        }
        if (peakR < 0 || peakSig < m_baseThreshold * 2) continue;

        // 1b. BFS from peak at a LOW analysis threshold to get a larger footprint
        //     for shape analysis. This captures the full signal "hill" including
        //     areas that were excluded from the zone by the high zoneThreshRatio.
        const int16_t analysisThresh = static_cast<int16_t>(std::max(
            static_cast<int>(m_baseThreshold),
            static_cast<int>(peakSig * m_gourdAnalysisRatio)));

        struct ZonePixel { int r, c; };
        std::vector<ZonePixel> analysisPixels;
        analysisPixels.reserve(128);
        std::array<bool, 2400> visited{};
        
        std::queue<int> bfsQ;
        const int startIdx = peakR * cols + peakC;
        bfsQ.push(startIdx);
        visited[startIdx] = true;
        
        int minR = rows, maxR = -1, minC = cols, maxC = -1;
        while (!bfsQ.empty()) {
            const int idx = bfsQ.front();
            bfsQ.pop();
            const int r = idx / cols;
            const int c = idx % cols;
            
            analysisPixels.push_back({r, c});
            if (r < minR) minR = r;
            if (r > maxR) maxR = r;
            if (c < minC) minC = c;
            if (c > maxC) maxC = c;
            
            for (int d = 0; d < 8; ++d) {
                const int nr = r + dr8[d];
                const int nc = c + dc8[d];
                if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
                const int ni = nr * cols + nc;
                if (visited[ni]) continue;
                if (frame.heatmapMatrix[nr][nc] < analysisThresh) continue;
                visited[ni] = true;
                bfsQ.push(ni);
            }
        }

        if (analysisPixels.size() < static_cast<size_t>(m_gourdMinLobeArea * 2)) {
            continue; // Too small to be a gourd
        }

        // 2-6. Try BOTH horizontal and vertical scans, pick the one with best constriction.
        //      This handles diagonally-placed peak pairs where one axis misses the waist.
        const int spanR = maxR - minR + 1;
        const int spanC = maxC - minC + 1;

        int bestWaistPos = -1;
        bool bestScanH = true;
        float bestRatio = 1.0f;

        for (int scanDir = 0; scanDir < 2; ++scanDir) {
            const bool scanH = (scanDir == 0);
            const int axLen = scanH ? spanC : spanR;
            if (axLen < 3) continue;

            std::vector<int> wp(axLen, 0);
            for (const auto& px : analysisPixels) {
                const int si = scanH ? (px.c - minC) : (px.r - minR);
                wp[si]++;
            }

            int mxW = *std::max_element(wp.begin(), wp.end());
            if (mxW < 2) continue;

            // Find interior minimum
            for (int i = 1; i < axLen - 1; ++i) {
                float r = static_cast<float>(wp[i]) / static_cast<float>(mxW);
                if (r >= bestRatio) continue;

                // Verify wide-narrow-wide: both sides must have width > wp[i]
                int mxBefore = *std::max_element(wp.begin(), wp.begin() + i);
                int mxAfter = *std::max_element(wp.begin() + i + 1, wp.end());
                if (mxBefore <= wp[i] || mxAfter <= wp[i]) continue;

                bestRatio = r;
                bestWaistPos = i;
                bestScanH = scanH;
            }
        }

        if (bestWaistPos < 0 || bestRatio > m_gourdConstrictionRatio) {
            continue; // No constriction found in either direction
        }

        const bool scanHorizontal = bestScanH;
        const int waistPos = bestWaistPos;

        // 7. Peak was already found in Step 1a (peakR, peakC, peakSig).
        //    Determine which side of the waist it falls on.
        const int peakSlice = scanHorizontal ? (peakC - minC) : (peakR - minR);
        const bool peakOnBeforeSide = (peakSlice < waistPos);

        // 8. Collect ZONE pixels (from m_touchZones) on the far side of the waist.
        //    Shape was detected on analysis pixels, but we split actual zone pixels.
        struct SplitPixel { int r, c; };
        std::vector<SplitPixel> farSidePixels;
        farSidePixels.reserve(32);
        int16_t newPeakSig = 0;
        int newPeakR = -1, newPeakC = -1;

        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (m_touchZones[r * cols + c] != static_cast<uint8_t>(zoneId)) continue;
                const int sliceIdx = scanHorizontal ? (c - minC) : (r - minR);
                const bool onFarSide = peakOnBeforeSide ? (sliceIdx > waistPos) : (sliceIdx < waistPos);
                if (onFarSide) {
                    farSidePixels.push_back({r, c});
                    int16_t sig = frame.heatmapMatrix[r][c];
                    if (sig > newPeakSig) {
                        newPeakSig = sig;
                        newPeakR = r;
                        newPeakC = c;
                    }
                }
            }
        }

        // 9. Regret: if no zone pixels on far side, try to assign from analysis pixels.
        if (farSidePixels.empty()) {
            // Zone was entirely on one side; assign analysis far-side pixels as new zone.
            for (const auto& px : analysisPixels) {
                const int sliceIdx = scanHorizontal ? (px.c - minC) : (px.r - minR);
                const bool onFarSide = peakOnBeforeSide ? (sliceIdx > waistPos) : (sliceIdx < waistPos);
                if (onFarSide && m_touchZones[px.r * cols + px.c] == 0) {
                    farSidePixels.push_back({px.r, px.c});
                    int16_t sig = frame.heatmapMatrix[px.r][px.c];
                    if (sig > newPeakSig) {
                        newPeakSig = sig;
                        newPeakR = px.r;
                        newPeakC = px.c;
                    }
                }
            }
        }

        if (static_cast<int>(farSidePixels.size()) < m_gourdMinLobeArea) continue;
        if (newPeakR < 0) continue;

        // 10. Commit the split: reassign far-side pixels to a new zone.
        const uint8_t newZoneId = static_cast<uint8_t>(++m_zoneCount);
        for (const auto& px : farSidePixels) {
            m_touchZones[px.r * cols + px.c] = newZoneId;
        }

        // Register the new peak.
        m_peaks.push_back({newPeakR, newPeakC, newPeakSig});
    }
}
void FeatureExtractor::CalculateCentroids(HeatmapFrame& frame) {
    frame.contacts.clear();
    const int rows = 40;
    const int cols = 60;
    
    int touchId = 1;
    for (int zoneId = 1; zoneId <= m_zoneCount; ++zoneId) {
        
        // TZ_CentroidCore: Full-Blob Center of Mass
        long long sum_v = 0;
        long long sum_vx = 0;
        long long sum_vy = 0;
        int area = 0;

        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (m_touchZones[r * cols + c] == static_cast<uint8_t>(zoneId)) {
                    int val = std::max(0, (int)frame.heatmapMatrix[r][c]);
                    area++;
                    sum_v += val;
                    sum_vx += val * c;
                    sum_vy += val * r;
                }
            }
        }
        
        if (area < m_minAreaThreshold || sum_v == 0) continue; // Area Pre-filter & Div0 Protection
        if (sum_v < m_minSignalSum) continue; // Touch pre-filter aligned with TSACore gating
        
        // Match Q8.8 + 128 bias: sum_vx / sum_v + 0.5f 
        float outX = static_cast<float>(sum_vx) / sum_v + 0.5f;
        float outY = static_cast<float>(sum_vy) / sum_v + 0.5f;
        
        // CTD_ECProcess: Official 1:1 LUT-based Edge Compensation
        if (m_ecEnabled) {
            ApplyEdgeCompensation(outX, outY, cols, rows);
        }

        Engine::TouchContact tc;
        tc.id = touchId++;
        tc.x = outX;
        tc.y = outY;
        tc.state = 0; // State machine (Target tracking) will overwrite this later if needed
        tc.area = area;
        tc.signalSum = static_cast<int>(sum_v);
        tc.sizeMm = std::sqrt(static_cast<float>(std::max(area, 1)));
        tc.isEdge = (outX < m_ecEdgeWidth) ||
                    (outX > static_cast<float>(cols - m_ecEdgeWidth)) ||
                    (outY < m_ecEdgeWidth) ||
                    (outY > static_cast<float>(rows - m_ecEdgeWidth));
        tc.isReported = true;
        tc.prevIndex = -1;
        tc.debugFlags = 0;
        tc.lifeFlags = 0;
        tc.reportFlags = 0;
        tc.reportEvent = 0;
        frame.contacts.push_back(tc);
    }

    ApplyTouchPreFilter(frame);
}

void FeatureExtractor::ApplyTouchPreFilter(HeatmapFrame& frame) {
    if (!m_touchPreFilterEnabled || frame.contacts.empty()) {
        return;
    }

    // Match TSA_MSTouchPreFilter order:
    //   1) RxLineFilter_Process
    //   2) SigSumFilter_ReserveTouch
    // RxLineFilter IsZ8Failed-like core:
    // If there is a stronger touch on nearly the same RX line, and the current
    // touch is weaker than configured Q7 ratio threshold, remove it.
    if (m_rxLineFilterEnabled && frame.contacts.size() > 1) {
        // Keep UI semantics: m_rxLineDelta=0 means "same line only".
        const int lineDeltaStrictLess = std::max(1, m_rxLineDelta + 1);
        const int weakRatioQ7 = std::clamp(static_cast<int>(std::lround(m_rxLineWeakRatio * 128.0f)), 1, 255);

        auto isZ8Failed = [&](size_t curIdx) -> bool {
            const auto& cur = frame.contacts[curIdx];
            const int curLine = static_cast<int>(std::lround(cur.y));
            int strongestSignal = cur.signalSum;
            size_t strongestIdx = curIdx;

            for (size_t otherIdx = 0; otherIdx < frame.contacts.size(); ++otherIdx) {
                if (otherIdx == curIdx) {
                    continue;
                }
                const auto& other = frame.contacts[otherIdx];
                const int otherLine = static_cast<int>(std::lround(other.y));
                const int lineAbs = std::abs(curLine - otherLine);
                if (lineAbs >= lineDeltaStrictLess) {
                    continue;
                }
                if (other.signalSum > strongestSignal) {
                    strongestSignal = other.signalSum;
                    strongestIdx = otherIdx;
                }
            }

            if (strongestIdx == curIdx) {
                return false;
            }
            const int curSignal = std::max(0, cur.signalSum);
            return (curSignal << 7) < (strongestSignal * weakRatioQ7);
        };

        for (int i = static_cast<int>(frame.contacts.size()) - 1; i >= 0; --i) {
            if (isZ8Failed(static_cast<size_t>(i))) {
                frame.contacts.erase(frame.contacts.begin() + i);
            }
        }
    }

    // SigSumFilter_ReserveTouch-like: sort descending by signal and keep top-N.
    std::stable_sort(frame.contacts.begin(), frame.contacts.end(),
                     [](const TouchContact& a, const TouchContact& b) {
                         if (a.signalSum != b.signalSum) {
                             return a.signalSum > b.signalSum;
                         }
                         return a.area > b.area;
                     });

    if (m_sigSumReserveCount > 0 &&
        frame.contacts.size() > static_cast<size_t>(m_sigSumReserveCount)) {
        frame.contacts.resize(static_cast<size_t>(m_sigSumReserveCount));
    }

    for (size_t i = 0; i < frame.contacts.size(); ++i) {
        frame.contacts[i].id = static_cast<int>(i + 1);
    }
}

void FeatureExtractor::ApplyEdgeCompensation(float& outX, float& outY, int cols, int rows) {
    // Dim1 = X logic
    if (outX < m_ecEdgeWidth) {
        // Dim1Near (Left edge)
        float normalizedDist = outX / static_cast<float>(m_ecEdgeWidth);
        uint8_t subpixelIdx = 255 - static_cast<uint8_t>(std::clamp(normalizedDist, 0.0f, 1.0f) * 255.0f);
        uint8_t edgeWidth = 120; // Default nominal edge width for segment selection
        int ctdOffset = CTD_ECGetOffset(subpixelIdx, edgeWidth, m_ecProfiles[0]);
        int rawDist = static_cast<int>(outX * 256.0f);
        int finalOffset = CTD_ECGetFinalOffset(rawDist, 256 - ctdOffset);
        outX = static_cast<float>(finalOffset) / 256.0f;
    } 
    else if (outX > (cols - m_ecEdgeWidth)) {
        // Dim1Far (Right edge)
        float distFromRight = static_cast<float>(cols) - outX;
        float normalizedDist = distFromRight / static_cast<float>(m_ecEdgeWidth);
        uint8_t subpixelIdx = 255 - static_cast<uint8_t>(std::clamp(normalizedDist, 0.0f, 1.0f) * 255.0f);
        uint8_t edgeWidth = 120;
        int ctdOffset = CTD_ECGetOffset(subpixelIdx, edgeWidth, m_ecProfiles[1]);
        int rawDist = static_cast<int>(distFromRight * 256.0f);
        int finalOffset = CTD_ECGetFinalOffset(rawDist, 256 - ctdOffset);
        outX = static_cast<float>(cols) - (static_cast<float>(finalOffset) / 256.0f);
    }

    // Dim2 = Y logic
    if (outY < m_ecEdgeWidth) {
        // Dim2Near (Top edge)
        float normalizedDist = outY / static_cast<float>(m_ecEdgeWidth);
        uint8_t subpixelIdx = 255 - static_cast<uint8_t>(std::clamp(normalizedDist, 0.0f, 1.0f) * 255.0f);
        uint8_t edgeWidth = 120; 
        int ctdOffset = CTD_ECGetOffset(subpixelIdx, edgeWidth, m_ecProfiles[2]);
        int rawDist = static_cast<int>(outY * 256.0f);
        int finalOffset = CTD_ECGetFinalOffset(rawDist, 256 - ctdOffset);
        outY = static_cast<float>(finalOffset) / 256.0f;
    }
    else if (outY > (rows - m_ecEdgeWidth)) {
        // Dim2Far (Bottom edge)
        float distFromBottom = static_cast<float>(rows) - outY;
        float normalizedDist = distFromBottom / static_cast<float>(m_ecEdgeWidth);
        uint8_t subpixelIdx = 255 - static_cast<uint8_t>(std::clamp(normalizedDist, 0.0f, 1.0f) * 255.0f);
        uint8_t edgeWidth = 120;
        int ctdOffset = CTD_ECGetOffset(subpixelIdx, edgeWidth, m_ecProfiles[3]);
        int rawDist = static_cast<int>(distFromBottom * 256.0f);
        int finalOffset = CTD_ECGetFinalOffset(rawDist, 256 - ctdOffset);
        outY = static_cast<float>(rows) - (static_cast<float>(finalOffset) / 256.0f);
    }
}

void FeatureExtractor::DrawConfigUI() {
    ImGui::TextWrapped("Unified 4.1/4.2 Feature Module");
    
    int p_thresh = m_baseThreshold;
    if (ImGui::SliderInt("4.1 Peak Threshold (Base)", &p_thresh, 5, 200)) {
        m_baseThreshold = static_cast<int16_t>(p_thresh);
    }
    ImGui::Checkbox("4.1 Z8 Neighbor Filter", &m_z8FilterEnabled);
    ImGui::Checkbox("4.1 Pressure Drift Reject", &m_pressureDriftEnabled);
    if (m_pressureDriftEnabled) {
        ImGui::SliderInt("  Drift Threshold (sigTholdLimit)", &m_pressureDriftThreshold, 50, 1000);
    }
    
    ImGui::SliderFloat("4.2 Zone Thresh Ratio (legacy)", &m_zoneThreshRatio, 0.1f, 0.9f, "%.2f");
    ImGui::SliderFloat("4.2 TZ Normal Coeff", &m_tzNormalCoeff, 0.1f, 1.0f, "%.2f");
    ImGui::SliderFloat("4.2 TZ Edge Coeff", &m_tzEdgeCoeff, 0.1f, 1.0f, "%.2f");
    ImGui::SliderInt("4.2 TZ Edge Margin", &m_tzEdgeMargin, 0, 5);
    ImGui::SliderInt("4.2 Morph. Passes (Close)", &m_morphPasses, 0, 3);
    ImGui::Checkbox("4.2 SubTZ Split", &m_subZoneSplitEnabled);
    if (m_subZoneSplitEnabled) {
        ImGui::SliderInt("  SubTZ Min Area", &m_subZoneMinArea, 1, 20);
        ImGui::SliderInt("  SubTZ Min Signal", &m_subZoneMinSignal, 0, 4000);
        ImGui::SliderFloat("  SubTZ Min Dist", &m_subZoneMinCentroidDist, 0.5f, 4.0f, "%.2f");
    }
    
    int min_area = m_minAreaThreshold;
    if (ImGui::SliderInt("4.3 Min Area Pre-Filter", &min_area, 1, 25)) {
        m_minAreaThreshold = min_area;
    }
    ImGui::SliderInt("4.3 Min Signal Sum", &m_minSignalSum, 0, 6000);
    ImGui::Checkbox("4.3 Touch PreFilter (TSA_MSTouchPreFilter)", &m_touchPreFilterEnabled);
    if (m_touchPreFilterEnabled) {
        ImGui::SliderInt("  SigSum Reserve Count", &m_sigSumReserveCount, 1, 20);
        ImGui::Checkbox("  RxLine Filter (Z8 core)", &m_rxLineFilterEnabled);
        if (m_rxLineFilterEnabled) {
            ImGui::SliderInt("    Rx Line Delta", &m_rxLineDelta, 0, 3);
            ImGui::SliderFloat("    Weak Ratio", &m_rxLineWeakRatio, 0.1f, 0.95f, "%.2f");
        }
    }
    
    ImGui::Checkbox("Edge Peak Suppression (TSA_MSPeakFilter)", &m_edgeSuppression);
    if (m_edgeSuppression) {
        ImGui::SliderFloat("  Edge Suppress Ratio", &m_edgeSuppressionRatio, 0.1f, 1.0f, "%.2f");
        ImGui::SliderInt("  Edge Suppress Margin", &m_edgeSuppressionMargin, 0, 5);
    }
    
    ImGui::Checkbox("Edge Compensation (1:1 LUT CTD_EC)", &m_ecEnabled);
    if (m_ecEnabled) {
        ImGui::SliderInt("  EC Action Width (Tiles)", &m_ecEdgeWidth, 1, 5);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "  Uses firmware-extracted g_ctd256Ln[256]\n  LUT piecewise interpolation arrays.");
    }

    ImGui::Separator();
    ImGui::Text("Fragmentation Control (Large Area)");
    ImGui::Checkbox("Peak Pruning (波峰剪枝)", &m_peakMergingEnabled);
    if (m_peakMergingEnabled) {
        ImGui::SliderFloat("  Merge Dist Thresh", &m_peakMergingDistThresh, 1.0f, 6.0f, "%.1f");
        ImGui::SliderFloat("  Gap Depth Ratio", &m_peakMergingGapRatio, 0.05f, 0.5f, "%.2f");
    }
    ImGui::Checkbox("Gourd Shape Split (葫芦分裂)", &m_gourdSplitEnabled);
    if (m_gourdSplitEnabled) {
        ImGui::SliderFloat("  Constriction Ratio", &m_gourdConstrictionRatio, 0.3f, 0.8f, "%.2f");
        ImGui::SliderInt("  Min Lobe Area", &m_gourdMinLobeArea, 1, 10);
        ImGui::SliderFloat("  Analysis Ratio", &m_gourdAnalysisRatio, 0.05f, 0.30f, "%.2f");
    }
}

void FeatureExtractor::SaveConfig(std::ostream& out) const {
    IFrameProcessor::SaveConfig(out);
    out << "PeakThreshold=" << m_baseThreshold << "\n";
    out << "ZoneThreshRatio=" << m_zoneThreshRatio << "\n";
    out << "MorphPasses=" << m_morphPasses << "\n";
    out << "SubZoneSplitEnabled=" << (m_subZoneSplitEnabled ? "1" : "0") << "\n";
    out << "SubZoneMinArea=" << m_subZoneMinArea << "\n";
    out << "SubZoneMinSignal=" << m_subZoneMinSignal << "\n";
    out << "SubZoneMinCentroidDist=" << m_subZoneMinCentroidDist << "\n";
    out << "MinAreaThreshold=" << m_minAreaThreshold << "\n";
    out << "MinSignalSum=" << m_minSignalSum << "\n";
    out << "TouchPreFilterEnabled=" << (m_touchPreFilterEnabled ? "1" : "0") << "\n";
    out << "SigSumReserveCount=" << m_sigSumReserveCount << "\n";
    out << "RxLineFilterEnabled=" << (m_rxLineFilterEnabled ? "1" : "0") << "\n";
    out << "RxLineDelta=" << m_rxLineDelta << "\n";
    out << "RxLineWeakRatio=" << m_rxLineWeakRatio << "\n";
    out << "EdgeSuppression=" << (m_edgeSuppression ? "1" : "0") << "\n";
    out << "EdgeSuppressionRatio=" << m_edgeSuppressionRatio << "\n";
    out << "EdgeSuppressionMargin=" << m_edgeSuppressionMargin << "\n";
    out << "ECEnabled=" << (m_ecEnabled ? "1" : "0") << "\n";
    out << "ECEdgeWidth=" << m_ecEdgeWidth << "\n";
    out << "PeakMergingEnabled=" << (m_peakMergingEnabled ? "1" : "0") << "\n";
    out << "PeakMergingDist=" << m_peakMergingDistThresh << "\n";
    out << "PeakMergingRatio=" << m_peakMergingGapRatio << "\n";
    out << "GourdSplitEnabled=" << (m_gourdSplitEnabled ? "1" : "0") << "\n";
    out << "GourdConstrictionRatio=" << m_gourdConstrictionRatio << "\n";
    out << "GourdMinLobeArea=" << m_gourdMinLobeArea << "\n";
    out << "GourdAnalysisRatio=" << m_gourdAnalysisRatio << "\n";
    out << "Z8FilterEnabled=" << (m_z8FilterEnabled ? "1" : "0") << "\n";
    out << "PressureDriftEnabled=" << (m_pressureDriftEnabled ? "1" : "0") << "\n";
    out << "PressureDriftThreshold=" << m_pressureDriftThreshold << "\n";
    out << "TZNormalCoeff=" << m_tzNormalCoeff << "\n";
    out << "TZEdgeCoeff=" << m_tzEdgeCoeff << "\n";
    out << "TZEdgeMargin=" << m_tzEdgeMargin << "\n";
}

void FeatureExtractor::LoadConfig(const std::string& key, const std::string& value) {
    IFrameProcessor::LoadConfig(key, value);
    if (key == "PeakThreshold") m_baseThreshold = static_cast<int16_t>(std::stoi(value));
    else if (key == "ZoneThreshRatio") m_zoneThreshRatio = std::stof(value);
    else if (key == "MorphPasses") m_morphPasses = std::stoi(value);
    else if (key == "SubZoneSplitEnabled") m_subZoneSplitEnabled = (value == "1");
    else if (key == "SubZoneMinArea") m_subZoneMinArea = std::stoi(value);
    else if (key == "SubZoneMinSignal") m_subZoneMinSignal = std::stoi(value);
    else if (key == "SubZoneMinCentroidDist") m_subZoneMinCentroidDist = std::stof(value);
    else if (key == "MinAreaThreshold") m_minAreaThreshold = std::stoi(value);
    else if (key == "MinSignalSum") m_minSignalSum = std::stoi(value);
    else if (key == "TouchPreFilterEnabled") m_touchPreFilterEnabled = (value == "1");
    else if (key == "SigSumReserveCount") m_sigSumReserveCount = std::stoi(value);
    else if (key == "RxLineFilterEnabled") m_rxLineFilterEnabled = (value == "1");
    else if (key == "RxLineDelta") m_rxLineDelta = std::stoi(value);
    else if (key == "RxLineWeakRatio") m_rxLineWeakRatio = std::stof(value);
    else if (key == "EdgeSuppression") m_edgeSuppression = (value == "1");
    else if (key == "EdgeSuppressionRatio") m_edgeSuppressionRatio = std::stof(value);
    else if (key == "EdgeSuppressionMargin") m_edgeSuppressionMargin = std::stoi(value);
    else if (key == "ECEnabled") m_ecEnabled = (value == "1");
    else if (key == "ECEdgeWidth") m_ecEdgeWidth = std::stoi(value);
    else if (key == "PeakMergingEnabled") m_peakMergingEnabled = (value == "1");
    else if (key == "PeakMergingDist") m_peakMergingDistThresh = std::stof(value);
    else if (key == "PeakMergingRatio") m_peakMergingGapRatio = std::stof(value);
    else if (key == "GourdSplitEnabled") m_gourdSplitEnabled = (value == "1");
    else if (key == "GourdConstrictionRatio") m_gourdConstrictionRatio = std::stof(value);
    else if (key == "GourdMinLobeArea") m_gourdMinLobeArea = std::stoi(value);
    else if (key == "GourdAnalysisRatio") m_gourdAnalysisRatio = std::stof(value);
    else if (key == "Z8FilterEnabled") m_z8FilterEnabled = (value == "1");
    else if (key == "PressureDriftEnabled") m_pressureDriftEnabled = (value == "1");
    else if (key == "PressureDriftThreshold") m_pressureDriftThreshold = std::stoi(value);
    else if (key == "TZNormalCoeff") m_tzNormalCoeff = std::stof(value);
    else if (key == "TZEdgeCoeff") m_tzEdgeCoeff = std::stof(value);
    else if (key == "TZEdgeMargin") m_tzEdgeMargin = std::stoi(value);
}

} // namespace Engine
