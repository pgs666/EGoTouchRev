#include "CentroidExtractor.h"
#include "imgui.h"
#include <cmath>
#include <algorithm>
#include <vector>
#include <utility>

namespace Engine {

// 注意：请确保在 CentroidExtractor.h 中将此函数的签名更新为：
// static std::vector<FingerCenter> analyze_and_segment_blob(const std::vector<TouchPoint>& blob, const int16_t global_grid[40][60], int depth = 0);
std::vector<FingerCenter> TouchSegmenter::analyze_and_segment_blob(
    const std::vector<TouchPoint>& blob, 
    const int16_t global_grid[40][60],
    int depth)
{
    if (blob.empty()) return {};
    
    // 1. Basic Centroid
    float sum_weight = 0.f, sum_x = 0.f, sum_y = 0.f;
    for (const auto& p : blob) {
        sum_weight += p.weight;
        sum_x += p.x * p.weight; 
        sum_y += p.y * p.weight;
    }
    // 防止除零保护
    if (sum_weight <= 0.f) return {};
    
    float mean_x = sum_x / sum_weight; 
    float mean_y = sum_y / sum_weight;

    // 像素点过少，没有切分价值，直接返回
    if (blob.size() < 4) return { {mean_x, mean_y, sum_weight} };

    // 2. PCA Covariance Matrix
    float c_xx = 0.f, c_xy = 0.f, c_yy = 0.f;
    for (const auto& p : blob) {
        float dx = p.x - mean_x; 
        float dy = p.y - mean_y;
        c_xx += p.weight * dx * dx; 
        c_xy += p.weight * dx * dy; 
        c_yy += p.weight * dy * dy;
    }
    c_xx /= sum_weight; c_xy /= sum_weight; c_yy /= sum_weight;

    // Eigenvalues calculation
    float b = c_xx + c_yy;
    float c = c_xx * c_yy - c_xy * c_xy;
    float delta = std::max(0.f, b * b - 4 * c);
    float lambda1 = (b + std::sqrt(delta)) / 2.f; 
    float lambda2 = (b - std::sqrt(delta)) / 2.f; 
    if (lambda2 < 0.001f) lambda2 = 0.001f;

    float aspect_ratio = lambda1 / lambda2;

    // --- Defense 1: Fat Thumb Check ---
    // 如果短轴方差极大，说明这是一个大面积压下的单指（如大拇指指腹），拒绝切分
    if (lambda2 > MAX_MINOR_AXIS_VARIANCE) {
        return { {mean_x, mean_y, sum_weight} }; 
    }

    // --- Decision Engine: 决定是否需要切分 ---
    // 条件1: 形状像花生米 (aspect_ratio > 1.9)
    // 条件2: 这是一个巨无霸热力斑块，不管形状如何，肯定包含多指 (sum_weight > 7000)
    bool is_merged = (aspect_ratio > ASPECT_RATIO_THRESHOLD) || (sum_weight > HUGE_WEIGHT_THRESHOLD);
    
    // 如果不满足切分条件，或者已经达到了最大递归深度 (2层足够分出4根手指)，则输出当前质心
    if (!is_merged || depth >= 2) {
        return { {mean_x, mean_y, sum_weight} }; 
    }

    // --- Initialize K-Means centers along the major axis ---
    float vx = c_xy, vy = lambda1 - c_xx;
    float norm = std::sqrt(vx*vx + vy*vy);
    if (norm > 1e-5f) { vx /= norm; vy /= norm; } else { vx = 1.0f; vy = 0.0f; }
    
    float spread = std::sqrt(lambda1);
    FingerCenter center1 { mean_x + 0.4f * spread * vx, mean_y + 0.4f * spread * vy, 0.f };
    FingerCenter center2 { mean_x - 0.4f * spread * vx, mean_y - 0.4f * spread * vy, 0.f };

    // K-Means Iteration (4 times for fast convergence)
    for (int iter = 0; iter < 4; ++iter) {
        float sum_w1 = 0.f, sum_x1 = 0.f, sum_y1 = 0.f;
        float sum_w2 = 0.f, sum_x2 = 0.f, sum_y2 = 0.f;
        
        for (const auto& p : blob) {
            float d1 = (p.x - center1.x)*(p.x - center1.x) + (p.y - center1.y)*(p.y - center1.y);
            float d2 = (p.x - center2.x)*(p.x - center2.x) + (p.y - center2.y)*(p.y - center2.y);
            if (d1 < d2) {
                sum_w1 += p.weight; sum_x1 += p.x * p.weight; sum_y1 += p.y * p.weight;
            } else {
                sum_w2 += p.weight; sum_x2 += p.x * p.weight; sum_y2 += p.y * p.weight;
            }
        }
        
        if (sum_w1 > 0) { center1.x = sum_x1 / sum_w1; center1.y = sum_y1 / sum_w1; center1.total_weight = sum_w1; }
        if (sum_w2 > 0) { center2.x = sum_x2 / sum_w2; center2.y = sum_y2 / sum_w2; center2.total_weight = sum_w2; }
    }

    // --- Defense 2: Physical Bone-Distance Check ---
    // 如果切开的两个质心距离不符合人类手指物理极限，说明误切了单指，合并退回
    float final_dist = std::sqrt(std::pow(center1.x - center2.x, 2) + std::pow(center1.y - center2.y, 2));
    if (final_dist < MIN_PHYSICAL_DISTANCE) {
        return { {mean_x, mean_y, sum_weight} }; 
    }

    // --- Defense 3: Valley Profile Check (谷底特征与深度比检测) ---
    int mid_x = std::clamp((int)std::round((center1.x + center2.x) / 2.0f), 0, 59);
    int mid_y = std::clamp((int)std::round((center1.y + center2.y) / 2.0f), 0, 39);
    
    int cy1 = std::clamp((int)std::round(center1.y), 0, 39);
    int cx1 = std::clamp((int)std::round(center1.x), 0, 59);
    int cy2 = std::clamp((int)std::round(center2.y), 0, 39);
    int cx2 = std::clamp((int)std::round(center2.x), 0, 59);

    int c1_val = global_grid[cy1][cx1];
    int c2_val = global_grid[cy2][cx2];
    int mid_val = global_grid[mid_y][mid_x];

    // 获取两端较小的那个峰值
    int min_peak = std::min(c1_val, c2_val);

    // 1. 如果中点隆起（完全没有凹陷），通常判定为单指平压。
    // （保留之前的肩部融合豁免：如果重量极大，允许强行劈开）
    if (mid_val >= min_peak) {
        if (sum_weight < HUGE_WEIGHT_THRESHOLD) {
            return { {mean_x, mean_y, sum_weight} };
        }
    } 
    // 2. 【新增防线：谷底深度比】如果虽然有凹陷，但凹陷极其微弱（比如大于较小峰值的 85%）
    // 这说明这是由于单根手指的骨骼（指尖+关节）造成的微弱起伏，并非两指缝隙！
    else if (mid_val > min_peak * 0.75f) {
        // 拒绝切分，将其视为一根平放的手指
        return { {mean_x, mean_y, sum_weight} };
    }

    // ==========================================
    // --- Recursive Splitting (应对3指、4指连体) ---
    // ==========================================
    std::vector<TouchPoint> sub_blob1;
    std::vector<TouchPoint> sub_blob2;

    for (const auto& p : blob) {
        float d1 = (p.x - center1.x)*(p.x - center1.x) + (p.y - center1.y)*(p.y - center1.y);
        float d2 = (p.x - center2.x)*(p.x - center2.x) + (p.y - center2.y)*(p.y - center2.y);
        if (d1 < d2) {
            sub_blob1.push_back(p);
        } else {
            sub_blob2.push_back(p);
        }
    }

    // 对分割出来的两个子热力斑块分别进行递归诊断
    std::vector<FingerCenter> result1 = analyze_and_segment_blob(sub_blob1, global_grid, depth + 1);
    std::vector<FingerCenter> result2 = analyze_and_segment_blob(sub_blob2, global_grid, depth + 1);

    // 汇总收集所有的叶子节点（独立的单指）
    result1.insert(result1.end(), result2.begin(), result2.end());
    return result1;
}

CentroidExtractor::CentroidExtractor() {}
CentroidExtractor::~CentroidExtractor() {}

bool CentroidExtractor::Process(HeatmapFrame& frame) {
    if (!m_enabled) return true;

    frame.contacts.clear();
    const int numRows = 40;
    const int numCols = 60;
    
    int touchId = 1;

    // 1. Connected Component Labeling (BFS) to gather Blobs
    std::vector<bool> visited(numRows * numCols, false);
    std::vector<std::vector<TouchPoint>> blobs;

    for (int y = 0; y < numRows; ++y) {
        for (int x = 0; x < numCols; ++x) {
            float val = static_cast<float>(frame.heatmapMatrix[y][x]);
            if (!visited[y * numCols + x] && val >= m_peakThreshold) {
                // Found a new unvisited pixel above threshold, start BFS
                std::vector<TouchPoint> currentBlob;
                std::vector<std::pair<int, int>> queue;
                queue.push_back({x, y});
                visited[y * numCols + x] = true;
                
                size_t head = 0;
                while (head < queue.size()) {
                    auto p = queue[head++];
                    int cx = p.first;
                    int cy = p.second;
                    currentBlob.push_back({static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(frame.heatmapMatrix[cy][cx])});
                    
                    // 8-connectivity to boldly merge diagonally touching pixels into the same blob
                    static const int dxs[] = {-1, 0, 1, -1, 1, -1, 0, 1};
                    static const int dys[] = {-1, -1, -1, 0, 0, 1, 1, 1};
                    for (int i = 0; i < 8; ++i) {
                        int nx = cx + dxs[i];
                        int ny = cy + dys[i];
                        if (nx >= 0 && nx < numCols && ny >= 0 && ny < numRows) {
                            int nIdx = ny * numCols + nx;
                            if (!visited[nIdx] && frame.heatmapMatrix[ny][nx] >= m_peakThreshold) {
                                visited[nIdx] = true;
                                queue.push_back({nx, ny});
                            }
                        }
                    }
                }
                if (!currentBlob.empty()) {
                    blobs.push_back(std::move(currentBlob));
                }
            }
        }
    }

    // 2. Segment each blob (Using Recursive Hierarchical PCA-KMeans)
    for (const auto& blob : blobs) {
        // depth 默认从 0 开始
        std::vector<FingerCenter> centers = TouchSegmenter::analyze_and_segment_blob(blob, frame.heatmapMatrix, 0);
        
        for (const auto& c : centers) {
            TouchContact tc;
            tc.id = touchId++;
            // CalculateGaussianParaboloid handles subpixel resolution. 
            float outY = c.y;
            float outX = m_algorithm == 1 ? CalculateGaussianParaboloid(frame, static_cast<int>(std::round(c.x)), static_cast<int>(std::round(c.y)), outY) : c.x;
            
            tc.x = m_algorithm == 1 ? outX : c.x;
            tc.y = m_algorithm == 1 ? outY : c.y;
            tc.state = 0; 
            tc.area = c.total_weight;  // Approximated Area
            frame.contacts.push_back(tc);
        }
    }

    return true;
}

float CentroidExtractor::CalculateGaussianParaboloid(const HeatmapFrame& frame, int cx, int cy, float& outY) const {
    // 1D Parabola Fitting for X and Y Separately around Peak
    auto GetV = [&](int dx, int dy) -> float {
        int nx = cx + dx;
        int ny = cy + dy;
        if (nx >= 0 && nx < 60 && ny >= 0 && ny < 40) {
            return std::max<float>(0.0f, static_cast<float>(frame.heatmapMatrix[ny][nx]));
        }
        return 0.0f;
    };

    float vCenter = GetV(0, 0);
    float vLeft   = GetV(-1, 0);
    float vRight  = GetV(1, 0);
    float vTop    = GetV(0, -1);
    float vBottom = GetV(0, 1);

    float denomX = 2.0f * (vLeft - 2.0f * vCenter + vRight);
    float dx = 0.0f;
    if (std::abs(denomX) > 0.001f) {
        dx = (vLeft - vRight) / denomX;
    }

    float denomY = 2.0f * (vTop - 2.0f * vCenter + vBottom);
    float dy = 0.0f;
    if (std::abs(denomY) > 0.001f) {
        dy = (vTop - vBottom) / denomY;
    }

    dx = std::clamp(dx, -0.5f, 0.5f);
    dy = std::clamp(dy, -0.5f, 0.5f);

    outY = cy + dy;
    return cx + dx;
}

void CentroidExtractor::DrawConfigUI() {
    ImGui::TextWrapped("Hierarchical PCA-KMeans Extractor (Supports 3+ Fingers):");
    ImGui::RadioButton("Native PCA Weight Centroid", &m_algorithm, 0);
    ImGui::RadioButton("2D Paraboloid Refinement", &m_algorithm, 1);
    
    // 这里的默认值建议在初始化时改为 80，保证滑动的灵敏度
    ImGui::SliderInt("Peak Detection Threshold", &m_peakThreshold, 50, 2000);
}

} // namespace Engine