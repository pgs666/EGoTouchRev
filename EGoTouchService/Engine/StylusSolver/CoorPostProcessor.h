#pragma once
#include "AsaTypes.h"
#include <array>
#include <cmath>

namespace Asa {

/// CoorPostProcessor — 9-stage coordinate post-processing chain
/// Mirrors ASA_CoorPostProcess from TSACore
class CoorPostProcessor {
public:
    /// Process coordinates through the full post-processing chain
    /// @param raw  Raw interpolated coordinate (0x400 units)
    /// @return Post-processed coordinate
    AsaCoorResult Process(const AsaCoorResult& raw);

    /// Reset all state (on pen-up or fresh start)
    void Reset();

    // ── Configuration ──
    // Speed-based IIR
    float iirLowCoef   = 0.3f;   // IIR coef for low speed
    float iirHighCoef  = 0.9f;   // IIR coef for high speed
    float speedLowThr  = 10.0f;  // speed threshold: low
    float speedHighThr = 204.0f; // speed threshold: high

    // Jitter lock
    int32_t jitterLockThreshold  = 20;  // center jitter lock
    int32_t jitterEdgeThreshold  = 40;  // edge jitter lock

    // 3-point average filter
    bool enable3PointAvg = true;
    // Linear filter state machine
    bool enableLinearFilter = false;  // complex, disabled by default

private:
    // ── Internal state ──
    bool     m_initialized = false;
    int      m_frameCount  = 0;

    // History ring buffer (24 frames for speed calc)
    static constexpr int kHistoryLen = 24;
    std::array<AsaCoorResult, kHistoryLen> m_history{};
    int m_historyIdx = 0;

    // 3-point average
    AsaCoorResult m_prev[2]{};  // previous 2 frames

    // IIR filter state
    float m_iirDim1 = 0.0f;
    float m_iirDim2 = 0.0f;
    float m_currentCoef = 0.0f;

    // Jitter lock
    AsaCoorResult m_lockedCoor{};
    bool  m_jitterLocked = false;

    // Speed
    float m_speed = 0.0f;

    // Helpers
    float CalcSpeed(const AsaCoorResult& cur);
    float CalcIIRCoef(float speed);
    AsaCoorResult ApplyIIR(const AsaCoorResult& cur, float coef);
    AsaCoorResult Apply3PointAvg(const AsaCoorResult& cur);
    AsaCoorResult ApplyJitterLock(const AsaCoorResult& cur);
    void PushHistory(const AsaCoorResult& cur);
};

} // namespace Asa
