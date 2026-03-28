#include "CoordinateSolver.h"
#include <algorithm>

namespace Asa {

// ── TriangleAlgUsing3Point ──
// Exact match of TSACore TriangleAlgUsing3Piont
// Returns sub-pixel offset with +0x200 bias (range [0, 0x400])
int32_t CoordinateSolver::TriangleAlgUsing3Point(
        int16_t left, int16_t peak, int16_t right) {
    const int32_t l = left, p = peak, r = right;
    int32_t result;

    if (r < l) {
        // Peak is left-biased
        int32_t minVal = r;
        if (p <= r) minVal = p - 1;
        int32_t den = p - minVal;
        if (den == 0) return 0x200;
        result = -(((l - minVal) * kCoorUnit) / den / 2);
    } else {
        // Peak is right-biased or centered
        int32_t minVal = l;
        if (p <= l) minVal = p - 1;
        int32_t den = p - minVal;
        if (den == 0) return 0x200;
        result = (((r - minVal) * kCoorUnit) / den / 2);
    }
    result += 0x200;  // bias to center of cell
    return result;
}

// ── TriangleAlgEdge ──
// Matches TSACore: calls EdgeCompensating -> TriangleAlgUsing3Point
// param1 & param2 are edge compensation parameters from g_asaPrmt
int32_t CoordinateSolver::TriangleAlgEdge(
        int16_t peak, int16_t n1, int16_t n2,
        int16_t param1, int16_t param2) {
    // EdgeCompensating: create a virtual neighbor for interpolation
    int16_t edgeParam1 = param1 != 0 ? param1 : 1;
    int32_t comp1 = ((static_cast<int32_t>(peak) - n1) * 10) /
                    static_cast<int32_t>(edgeParam1);
    int32_t comp2 = peak - ((n1 - n2) * static_cast<int32_t>(edgeParam1)) / 10;
    int16_t virtualNeighbor;
    if (comp1 < comp2) {
        virtualNeighbor = static_cast<int16_t>(comp2);
    } else {
        virtualNeighbor = static_cast<int16_t>(comp1);
    }
    // Clamp: virtual neighbor must be less than peak
    if (peak <= virtualNeighbor) {
        virtualNeighbor = peak - 1;
    }

    // Now use 3-point triangle with the virtual neighbor
    int32_t result = TriangleAlgUsing3Point(virtualNeighbor, peak, n1);

    // Sum threshold check from original
    int32_t sum = static_cast<int32_t>(peak) + n1 + n2;
    if (sum < (static_cast<int32_t>(param2) * 2) / 5) {
        result = 0;
    }
    return result;
}

// ── SolveByTriangle: dispatch to edge/mid interpolation ──
int32_t CoordinateSolver::SolveByTriangle(
        const int32_t* signal, int peakIdx, int len,
        const int16_t* edgeParam) {
    if (peakIdx < 0 || peakIdx >= len) return 0x7FFFFFFF;

    const auto s = [&](int i) -> int16_t {
        return static_cast<int16_t>(std::clamp(signal[i],
            static_cast<int32_t>(INT16_MIN),
            static_cast<int32_t>(INT16_MAX)));
    };

    if (peakIdx == 0) {
        // Left edge
        return TriangleAlgEdge(
            s(0), s(1), s(2), edgeParam[0], edgeParam[2]);
    }
    if (peakIdx == len - 1) {
        // Right edge: result = len*kCoorUnit - offset
        int32_t edge = TriangleAlgEdge(
            s(len-1), s(len-2), s(len-3),
            edgeParam[0], edgeParam[1]);
        return static_cast<int32_t>(len) * kCoorUnit - edge;
    }
    // Middle: standard 3-point
    int32_t offset = TriangleAlgUsing3Point(
        s(peakIdx - 1), s(peakIdx), s(peakIdx + 1));
    return peakIdx * kCoorUnit + offset;
}

// ── SolveByGravity: weighted centroid ──
int32_t CoordinateSolver::SolveByGravity(
        const int32_t* signal, int len) {
    int64_t weightedSum = 0;
    int64_t totalWeight = 0;
    for (int i = 0; i < len; ++i) {
        if (signal[i] > 0) {
            weightedSum += static_cast<int64_t>(signal[i]) *
                           i * kCoorUnit;
            totalWeight += signal[i];
        }
    }
    if (totalWeight <= 0) return 0x7FFFFFFF;
    return static_cast<int32_t>(weightedSum / totalWeight);
}

// ── Solve: main entry point ──
AsaCoorResult CoordinateSolver::Solve(
        const AsaProjection& proj,
        int gridDimRow, int gridDimCol) {
    AsaCoorResult result{};

    int32_t coorDim1, coorDim2;
    if (useTriangle) {
        coorDim1 = SolveByTriangle(
            proj.dim1, proj.peakIdxDim1, kGridDim, triParamDim1);
        coorDim2 = SolveByTriangle(
            proj.dim2, proj.peakIdxDim2, kGridDim, triParamDim2);
    } else {
        coorDim1 = SolveByGravity(proj.dim1, kGridDim);
        coorDim2 = SolveByGravity(proj.dim2, kGridDim);
    }

    // Invalid check
    if (coorDim1 == 0x7FFFFFFF || coorDim2 == 0x7FFFFFFF)
        return result;

    // Clamp to valid range [0, dim*kCoorUnit - 1]
    const int32_t maxDim1 = gridDimCol * kCoorUnit - 1;
    const int32_t maxDim2 = gridDimRow * kCoorUnit - 1;
    result.dim1 = std::clamp(coorDim1, 0, maxDim1);
    result.dim2 = std::clamp(coorDim2, 0, maxDim2);
    result.valid = true;
    return result;
}

} // namespace Asa
