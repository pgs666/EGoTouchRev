#include "CoorPostProcessor.h"
#include <algorithm>
#include <cmath>

namespace Asa {

void CoorPostProcessor::Reset() {
    m_initialized = false;
    m_frameCount = 0;
    m_historyIdx = 0;
    m_history.fill(AsaCoorResult{});
    m_prev[0] = m_prev[1] = AsaCoorResult{};
    m_iirDim1 = m_iirDim2 = 0.0f;
    m_currentCoef = 0.0f;
    m_lockedCoor = AsaCoorResult{};
    m_jitterLocked = false;
    m_speed = 0.0f;
}

// ── PushHistory ──
void CoorPostProcessor::PushHistory(const AsaCoorResult& cur) {
    m_history[m_historyIdx] = cur;
    m_historyIdx = (m_historyIdx + 1) % kHistoryLen;
}

// ── CalcSpeed: distance over 1-frame window ──
float CoorPostProcessor::CalcSpeed(const AsaCoorResult& cur) {
    // Use oldest entry in history as reference
    int oldest = (m_historyIdx) % kHistoryLen;
    const auto& ref = m_history[oldest];
    if (!ref.valid) return 0.0f;
    float dx = static_cast<float>(cur.dim1 - ref.dim1);
    float dy = static_cast<float>(cur.dim2 - ref.dim2);
    return std::sqrt(dx * dx + dy * dy);
}

// ── CalcIIRCoef: speed-based adaptive coefficient ──
float CoorPostProcessor::CalcIIRCoef(float speed) {
    if (speed <= speedLowThr) return iirLowCoef;
    if (speed >= speedHighThr) return iirHighCoef;
    // Linear interpolation between low and high
    float t = (speed - speedLowThr) / (speedHighThr - speedLowThr);
    return iirLowCoef + t * (iirHighCoef - iirLowCoef);
}

// ── ApplyIIR: IIR coordinate filter ──
AsaCoorResult CoorPostProcessor::ApplyIIR(
        const AsaCoorResult& cur, float coef) {
    AsaCoorResult out = cur;
    m_iirDim1 = m_iirDim1 * (1.0f - coef) +
                static_cast<float>(cur.dim1) * coef;
    m_iirDim2 = m_iirDim2 * (1.0f - coef) +
                static_cast<float>(cur.dim2) * coef;
    out.dim1 = static_cast<int32_t>(std::lround(m_iirDim1));
    out.dim2 = static_cast<int32_t>(std::lround(m_iirDim2));
    return out;
}

// ── Apply3PointAvg: 3-frame moving average ──
AsaCoorResult CoorPostProcessor::Apply3PointAvg(
        const AsaCoorResult& cur) {
    if (!m_prev[0].valid || !m_prev[1].valid) return cur;
    AsaCoorResult out = cur;
    out.dim1 = (m_prev[1].dim1 + m_prev[0].dim1 + cur.dim1) / 3;
    out.dim2 = (m_prev[1].dim2 + m_prev[0].dim2 + cur.dim2) / 3;
    return out;
}

// ── ApplyJitterLock: lock position until threshold ──
AsaCoorResult CoorPostProcessor::ApplyJitterLock(
        const AsaCoorResult& cur) {
    if (!m_jitterLocked) {
        m_lockedCoor = cur;
        m_jitterLocked = true;
        return cur;
    }
    int32_t dx = std::abs(cur.dim1 - m_lockedCoor.dim1);
    int32_t dy = std::abs(cur.dim2 - m_lockedCoor.dim2);
    int32_t thr = jitterLockThreshold;
    if (dx > thr || dy > thr) {
        m_lockedCoor = cur;
        return cur;
    }
    return m_lockedCoor;
}

// ── Process: main 9-stage chain ──
AsaCoorResult CoorPostProcessor::Process(const AsaCoorResult& raw) {
    if (!raw.valid) {
        Reset();
        return raw;
    }

    AsaCoorResult cur = raw;

    // Stage 1: Linear filter (optional, complex state machine)
    // if (enableLinearFilter) { ... } // TODO: Phase P1

    // Stage 2: Push to real-time ring buffer
    PushHistory(cur);

    // Stage 3: 3-point average filter
    if (enable3PointAvg && m_frameCount >= 2) {
        cur = Apply3PointAvg(cur);
    }

    // Stage 4: CoorRevise (TX2 correction) — TODO: Phase P1

    // Stage 5: Speed calculation
    m_speed = CalcSpeed(cur);

    // Stage 6: Dynamic IIR coefficient
    m_currentCoef = CalcIIRCoef(m_speed);

    // Stage 7: IIR coordinate filter
    if (!m_initialized) {
        m_iirDim1 = static_cast<float>(cur.dim1);
        m_iirDim2 = static_cast<float>(cur.dim2);
        m_initialized = true;
    } else {
        cur = ApplyIIR(cur, m_currentCoef);
    }

    // Stage 8: Jitter lock
    cur = ApplyJitterLock(cur);

    // Stage 9: FitToLcdScreen — done in StylusPipeline::BuildPacket

    // Update history
    m_prev[1] = m_prev[0];
    m_prev[0] = cur;
    m_frameCount++;
    return cur;
}

} // namespace Asa
