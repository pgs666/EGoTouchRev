#include "SignalSegmenter.h"
#include <imgui.h>
#include <queue>
#include <algorithm>

namespace Engine {

SignalSegmenter::SignalSegmenter() {
}

bool SignalSegmenter::Process(Engine::HeatmapFrame& frame) {
    if (!m_enabled) return true;

    m_debugPeaks.clear();
    
    // We only want to find peaks, not yet modify the contacts list since
    // this is Phase 1 (Verification)

    std::vector<Blob> blobs;
    FindBlobs(frame, blobs);

    for (const auto& blob : blobs) {
        FindPeaksInBlob(frame, blob, m_debugPeaks);
    }
    
    return true;
}

void SignalSegmenter::FindBlobs(const Engine::HeatmapFrame& frame, std::vector<Blob>& outBlobs) {
    bool visited[40][60] = { false };
    
    // 8-way connectivity directions
    const int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    const int dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};

    for (int r = 0; r < 40; ++r) {
        for (int c = 0; c < 60; ++c) {
            if (visited[r][c] || frame.heatmapMatrix[r][c] < m_baseThreshold) {
                visited[r][c] = true;
                continue;
            }

            // Start an BFS to extract a Blob
            Blob currentBlob;
            std::queue<std::pair<int, int>> q;
            
            q.push({r, c});
            visited[r][c] = true;
            
            while (!q.empty()) {
                auto [currR, currC] = q.front();
                q.pop();
                
                int val = frame.heatmapMatrix[currR][currC];
                currentBlob.pixels.push_back({currR, currC, val});
                if (val > currentBlob.maxVal) {
                    currentBlob.maxVal = val;
                }
                
                // Check 8 neighbors
                for (int i = 0; i < 8; ++i) {
                    int nr = currR + dr[i];
                    int nc = currC + dc[i];
                    
                    if (nr >= 0 && nr < 40 && nc >= 0 && nc < 60) {
                        if (!visited[nr][nc] && frame.heatmapMatrix[nr][nc] >= m_baseThreshold) {
                            visited[nr][nc] = true;
                            q.push({nr, nc});
                        }
                    }
                }
            }
            
            // Only keep blobs that have at least some mass
            if (!currentBlob.pixels.empty()) {
                outBlobs.push_back(currentBlob);
            }
        }
    }
}

void SignalSegmenter::FindPeaksInBlob(const Engine::HeatmapFrame& frame, const Blob& blob, std::vector<std::pair<int, int>>& outPeaks) {
    if (blob.pixels.empty()) return;
    
    // 1. Gather all pixels into a 2D access map for this blob to do 8-way checks and gradient climbing
    // Since we only care about pixels IN the blob, we use a map keyed by (r, c)
    std::map<std::pair<int, int>, int> blobPixels;
    for (const auto& p : blob.pixels) {
        blobPixels[{p.r, p.c}] = p.val;
    }

    // A helper lambda to safely get pixel value (returns -1 if out of blob bounds)
    auto getBlobVal = [&](int r, int c) -> int {
        auto it = blobPixels.find({r, c});
        if (it != blobPixels.end()) return it->second;
        return -1;
    };

    // 2. Find all Candidate Peaks (Strict 8-way local maxima)
    struct Candidate {
        int r, c, val;
    };
    std::vector<Candidate> candidates;

    // 8-way offsets
    const int dr[8] = {-1, -1, -1,  0, 0,  1, 1, 1};
    const int dc[8] = {-1,  0,  1, -1, 1, -1, 0, 1};

    for (const auto& p : blob.pixels) {
        bool isLocalMax = true;
        for (int i = 0; i < 8; ++i) {
            int nr = p.r + dr[i];
            int nc = p.c + dc[i];
            int neighborVal = getBlobVal(nr, nc);
            
            // If neighbor exists in blob and is strictly greater, not a local max
            // We use >= to handle plateaus: if a neighbor is equal, we only keep one (the one processed first or tie-breaker)
            // To be safe with plateaus, we require strictly greater than all neighbors.
            if (neighborVal > p.val) {
                isLocalMax = false;
                break;
            }
        }
        
        if (isLocalMax && p.val >= m_baseThreshold) {
            candidates.push_back({p.r, p.c, p.val});
        }
    }

    // Sort candidates descending by value
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.val > b.val;
    });

    if (candidates.empty()) return;

    // 3. Saddle Point Verification (Gradient Ascent Climbing)
    // The highest candidate is ALWAYS a true peak for this blob.
    outPeaks.push_back({candidates[0].r, candidates[0].c});

    // Check subsequent candidates
    for (size_t i = 1; i < candidates.size(); ++i) {
        int currR = candidates[i].r;
        int currC = candidates[i].c;
        int currVal = candidates[i].val;
        
        // Optional: filter out candidates that are too weak compared to the global max of this blob
        // (to avoid extreme noise, based on the deleted MediumSignalRatio concept, if desired.
        // For now, we rely purely on Saddle Point topology).

        bool reachedHigherPeakWithoutValley = false;
        
        // Start climbing
        int traceR = currR;
        int traceC = currC;
        int traceVal = currVal;
        
        // We must reach a higher peak through a *strictly increasing* path.
        // If we get trapped (no neighbor is strictly higher than our current spot), we are a valid peak!
        while (true) {
            int nextR = traceR;
            int nextC = traceC;
            int bestNeighborVal = -1;

            // Find the highest neighbor that is strictly greater than our current position
            for (int k = 0; k < 8; ++k) {
                int nr = traceR + dr[k];
                int nc = traceC + dc[k];
                
                int neighborVal = getBlobVal(nr, nc);
                if (neighborVal > bestNeighborVal && neighborVal > traceVal) {
                    bestNeighborVal = neighborVal;
                    nextR = nr;
                    nextC = nc;
                }
            }

            // If we have nowhere to go (no neighbor is strictly higher), climbing stops.
            // This means we are at a local maximum (or trapped by a saddle), hence a valid peak.
            if (bestNeighborVal == -1) {
                break;
            }

            // Move to the next highest neighbor
            traceR = nextR;
            traceC = nextC;
            traceVal = bestNeighborVal;

            // Did we climb higher than our original candidate starting point?
            // If so, we are just a shoulder/slope leading up to a bigger mountain.
            if (traceVal > currVal) {
                reachedHigherPeakWithoutValley = true;
                break;
            }
        }

        // Decision
        if (!reachedHigherPeakWithoutValley) {
            outPeaks.push_back({currR, currC});
        }
    }
}

void SignalSegmenter::DrawConfigUI() {
    ImGui::Text("Signal Segmenter Settings");
    ImGui::Separator();
    
    ImGui::Checkbox("Enable Segmenter (Phase 1)", &m_enabled);
    
    if (m_enabled) {
        ImGui::SliderInt("Base Threshold", &m_baseThreshold, 1, 255, "%d");
        ImGui::SliderFloat("High Signal Ratio", &m_highSignalRatio, 0.1f, 1.0f, "%.2f");
        
        ImGui::Separator();
        ImGui::Text("Iterative Subtraction Settings");
        ImGui::SliderFloat("Damp Dist 1", &m_dampDist1, 0.0f, 1.2f, "%.2f");
        ImGui::SliderFloat("Damp Dist sqrt(2)", &m_dampDistSqrt2, 0.0f, 1.2f, "%.2f");
        ImGui::SliderFloat("Damp Dist 2", &m_dampDist2, 0.0f, 1.2f, "%.2f");
        
        ImGui::Separator();
        ImGui::Text("Detected Local Peaks: %zu", m_debugPeaks.size());
    }
}

} // namespace Engine
