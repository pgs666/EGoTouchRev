#pragma once
#include "AsaTypes.h"

namespace Asa {

/// CoordinateSolver — Triangle/Gravity interpolation on 1D projections
/// Mirrors TX1CoordinateProcess + GetCoordinateByTriangleOf
class CoordinateSolver {
public:
    /// Solve coordinates from 1D projection signals
    /// @param proj  1D projections from GridPeakDetector::ProjectTo1D
    /// @param gridDimRow  Number of grid rows for clamping (default 9)
    /// @param gridDimCol  Number of grid cols for clamping (default 9)
    /// @return Coordinate in 0x400 units
    AsaCoorResult Solve(const AsaProjection& proj,
                        int gridDimRow = kGridDim,
                        int gridDimCol = kGridDim);

    // Configuration: algorithm selection
    bool useTriangle = true;  // false → gravity interpolation

    // Triangle interpolation edge parameters (from g_asaPrmt)
    int16_t triParamDim1[3] = {0, 0, 0};
    int16_t triParamDim2[3] = {0, 0, 0};

private:
    /// Triangle interpolation using 3 points (mid-grid)
    int32_t TriangleAlgUsing3Point(
        int16_t left, int16_t peak, int16_t right);

    /// Triangle interpolation at grid edge
    int32_t TriangleAlgEdge(
        int16_t peak, int16_t n1, int16_t n2,
        int16_t param1, int16_t param2);

    /// Solve one dimension using triangle interpolation
    int32_t SolveByTriangle(
        const int32_t* signal, int peakIdx, int len,
        const int16_t* edgeParam);

    /// Gravity (centroid) interpolation on 1D signal
    int32_t SolveByGravity(const int32_t* signal, int len);
};

} // namespace Asa
