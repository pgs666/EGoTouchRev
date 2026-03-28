#pragma once
#include "AsaTypes.h"
#include <vector>

namespace Asa {

/// GridPeakDetector — Flood-fill peak detection on 9×9 grid
/// Mirrors HPP3_FindPeakOfNormalGrid + GetGridTx1Peaks
class GridPeakDetector {
public:
    /// Run flood-fill peak detection on a 9×9 grid
    /// @return Primary peak unit (strongest valid peak)
    GridPeakUnit FindPeak(const int16_t grid[kGridDim][kGridDim]);

    /// Project grid onto 1D signals around the detected peak
    /// @param grid     The 9×9 grid
    /// @param peak     Peak location from FindPeak()
    /// @return Row/column 1D projections with peak indices
    AsaProjection ProjectTo1D(
        const int16_t grid[kGridDim][kGridDim],
        const GridPeakUnit& peak);

    // Configuration
    int   noiseThreshold = 50;     // signal > this to be considered (lowered for bringup)
    int   maxConnected   = 81;     // disabled for bringup (full grid = 9*9)
    int   projRadius     = 2;      // rows/cols around peak for projection

private:
    bool IsPeak(const int16_t grid[kGridDim][kGridDim],
                int r, int c) const;
    int  FloodFill(const int16_t grid[kGridDim][kGridDim],
                   bool visited[kGridDim][kGridDim],
                   int r, int c,
                   std::vector<std::pair<int,int>>& region) const;
    int32_t Calc3x3Sum(const int16_t grid[kGridDim][kGridDim],
                       int r, int c) const;
    int  FindLinePeak(const int32_t* signal, int len) const;
};

} // namespace Asa
