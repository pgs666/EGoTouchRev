#include "FeatureExtractor.h"
#include "imgui.h"
#include <algorithm>

namespace Engine {

FeatureExtractor::FeatureExtractor() {
    m_touchZones.fill(0);
}

bool FeatureExtractor::Process(HeatmapFrame& frame) {
    if (!m_enabled) return true;

    // 4.1
    DetectPeaks(frame);
    
    // 4.2
    GenerateTouchZones(frame);
    
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

void FeatureExtractor::GenerateTouchZones(const HeatmapFrame& frame) {
    m_touchZones.fill(0);
    if (m_peaks.empty()) return;

    const int rows = 40;
    const int cols = 60;

    // Simple fixed array stack for flood fill
    struct Pos { int r, c; };
    std::vector<Pos> stack;
    stack.reserve(120);

    uint8_t currentZoneId = 1;
    // Four directions: Up, Down, Left, Right
    const int dr[] = {-1, 1, 0, 0};
    const int dc[] = {0, 0, -1, 1};

    for (const auto& peak : m_peaks) {
        // Skip if already consumed by a larger peak's zone
        if (m_touchZones[peak.r * cols + peak.c] != 0) continue;

        stack.push_back({peak.r, peak.c});
        m_touchZones[peak.r * cols + peak.c] = currentZoneId;
        
        // 4.1/4.2 Insight: Threshold is dynamically based on peak intensity
        int16_t dynamicEdgeThresh = static_cast<int16_t>(peak.z * m_zoneThreshRatio);

        while (!stack.empty()) {
            Pos p = stack.back();
            stack.pop_back();

            for (int i = 0; i < 4; ++i) {
                int nr = p.r + dr[i];
                int nc = p.c + dc[i];

                if (nr >= 0 && nr < rows && nc >= 0 && nc < cols) {
                    if (m_touchZones[nr * cols + nc] == 0) {
                        int16_t n_val = frame.heatmapMatrix[nr][nc];
                        if (n_val >= dynamicEdgeThresh) {
                            m_touchZones[nr * cols + nc] = currentZoneId;
                            stack.push_back({nr, nc});
                        }
                    }
                }
            }
        }
        currentZoneId++;
        if (currentZoneId >= 255) break;
    }
    
    // 4.2 Insight: Morphological Transform (Closing) to smooth borders and fill holes
    if (m_morphPasses > 0) {
        std::array<uint8_t, 2400> tempBuf = m_touchZones;
        // Step 1: Dilation (Grow 1 pixel)
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (m_touchZones[r * cols + c] == 0) {
                    for (int i = 0; i < 4; ++i) {
                        int nr = r + dr[i];
                        int nc = c + dc[i];
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
        
        // Step 2: Erosion (Shrink 1 pixel)
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (m_touchZones[r * cols + c] > 0) {
                    bool keep = true;
                    for (int i = 0; i < 4; ++i) {
                        int nr = r + dr[i];
                        int nc = c + dc[i];
                        if (nr >= 0 && nr < rows && nc >= 0 && nc < cols) {
                            if (m_touchZones[nr * cols + nc] == 0) {
                                keep = false; break;
                            }
                        } else {
                            // keep = false; break; // DO NOT erode on absolute screen boundaries
                        }
                    }
                    if (!keep) tempBuf[r * cols + c] = 0;
                }
            }
        }
        m_touchZones = tempBuf;
    }
}

void FeatureExtractor::CalculateCentroids(HeatmapFrame& frame) {
    frame.contacts.clear();
    const int rows = 40;
    const int cols = 60;
    
    int touchId = 1;
    for (size_t i = 0; i < m_peaks.size(); ++i) {
        uint8_t zoneId = i + 1;
        
        // TZ_CentroidCore: Full-Blob Center of Mass
        long long sum_v = 0;
        long long sum_vx = 0;
        long long sum_vy = 0;
        int area = 0;

        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (m_touchZones[r * cols + c] == zoneId) {
                    int val = std::max(0, (int)frame.heatmapMatrix[r][c]);
                    area++;
                    sum_v += val;
                    sum_vx += val * c;
                    sum_vy += val * r;
                }
            }
        }
        
        if (area < m_minAreaThreshold || sum_v == 0) continue; // Area Pre-filter & Div0 Protection
        
        // Match Q8.8 + 128 bias: sum_vx / sum_v + 0.5f 
        float outX = static_cast<float>(sum_vx) / sum_v + 0.5f;
        float outY = static_cast<float>(sum_vy) / sum_v + 0.5f;
        
        // CTD_ECProcess: Parametric Edge Compensation interpolation
        if (m_ecEnabled) {
            float maxX = static_cast<float>(cols - 1);
            float maxY = static_cast<float>(rows - 1);
            
            auto compensate = [&](float val, float maxVal) {
                if (val < m_ecMargin) {
                    float factor = 1.0f - (val / m_ecMargin); // 1.0 at edge, 0.0 at margin
                    return std::max(0.0f, val - (factor * m_ecMaxOffset));
                } else if (val > maxVal - m_ecMargin) {
                    float dist = maxVal - val;
                    float factor = 1.0f - (dist / m_ecMargin);
                    return std::min(maxVal, val + (factor * m_ecMaxOffset));
                }
                return val;
            };

            outX = compensate(outX, maxX);
            outY = compensate(outY, maxY);
        }

        Engine::TouchContact tc;
        tc.id = touchId++;
        tc.x = outX;
        tc.y = outY;
        tc.state = 0; // State machine (Target tracking) will overwrite this later if needed
        tc.area = area;
        frame.contacts.push_back(tc);
    }
}

void FeatureExtractor::DrawConfigUI() {
    ImGui::TextWrapped("Unified 4.1/4.2 Feature Module");
    
    int p_thresh = m_baseThreshold;
    if (ImGui::SliderInt("4.1 Peak Threshold (Base)", &p_thresh, 5, 200)) {
        m_baseThreshold = static_cast<int16_t>(p_thresh);
    }
    
    ImGui::SliderFloat("4.2 Zone Thresh Ratio", &m_zoneThreshRatio, 0.1f, 0.9f, "%.2f");
    ImGui::SliderInt("4.2 Morph. Passes (Close)", &m_morphPasses, 0, 3);
    
    int min_area = m_minAreaThreshold;
    if (ImGui::SliderInt("4.3 Min Area Pre-Filter", &min_area, 1, 25)) {
        m_minAreaThreshold = min_area;
    }
    
    ImGui::Checkbox("Edge Peak Suppression (TSA_MSPeakFilter)", &m_edgeSuppression);
    if (m_edgeSuppression) {
        ImGui::SliderFloat("  Edge Suppress Ratio", &m_edgeSuppressionRatio, 0.1f, 1.0f, "%.2f");
        ImGui::SliderInt("  Edge Suppress Margin", &m_edgeSuppressionMargin, 0, 5);
    }
    
    ImGui::Checkbox("Edge Compensation (CTD_ECProcess)", &m_ecEnabled);
    if (m_ecEnabled) {
        ImGui::SliderFloat("  EC Activation Margin", &m_ecMargin, 0.5f, 5.0f, "%.2f px");
        ImGui::SliderFloat("  EC Max Outward Push", &m_ecMaxOffset, 0.1f, 1.5f, "%.2f px");
    }
}

void FeatureExtractor::SaveConfig(std::ostream& out) const {
    IFrameProcessor::SaveConfig(out);
    out << "PeakThreshold=" << m_baseThreshold << "\n";
    out << "ZoneThreshRatio=" << m_zoneThreshRatio << "\n";
    out << "MorphPasses=" << m_morphPasses << "\n";
    out << "MinAreaThreshold=" << m_minAreaThreshold << "\n";
    out << "EdgeSuppression=" << (m_edgeSuppression ? "1" : "0") << "\n";
    out << "EdgeSuppressionRatio=" << m_edgeSuppressionRatio << "\n";
    out << "EdgeSuppressionMargin=" << m_edgeSuppressionMargin << "\n";
    out << "ECEnabled=" << (m_ecEnabled ? "1" : "0") << "\n";
    out << "ECMargin=" << m_ecMargin << "\n";
    out << "ECMaxOffset=" << m_ecMaxOffset << "\n";
}

void FeatureExtractor::LoadConfig(const std::string& key, const std::string& value) {
    IFrameProcessor::LoadConfig(key, value);
    if (key == "PeakThreshold") m_baseThreshold = static_cast<int16_t>(std::stoi(value));
    else if (key == "ZoneThreshRatio") m_zoneThreshRatio = std::stof(value);
    else if (key == "MorphPasses") m_morphPasses = std::stoi(value);
    else if (key == "MinAreaThreshold") m_minAreaThreshold = std::stoi(value);
    else if (key == "EdgeSuppression") m_edgeSuppression = (value == "1");
    else if (key == "EdgeSuppressionRatio") m_edgeSuppressionRatio = std::stof(value);
    else if (key == "EdgeSuppressionMargin") m_edgeSuppressionMargin = std::stoi(value);
    else if (key == "ECEnabled") m_ecEnabled = (value == "1");
    else if (key == "ECMargin") m_ecMargin = std::stof(value);
    else if (key == "ECMaxOffset") m_ecMaxOffset = std::stof(value);
}

} // namespace Engine
