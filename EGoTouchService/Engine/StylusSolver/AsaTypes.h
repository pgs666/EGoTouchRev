#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace Asa {

// ── Slave Frame Constants ──
static constexpr int kSlaveHeaderBytes = 7;
static constexpr int kBlockWords       = 83;   // 2 anchor + 81 grid
static constexpr int kGridDim          = 9;
static constexpr int kGridSize         = kGridDim * kGridDim; // 81
static constexpr int kCoorUnit         = 0x400; // 1024 sub-units per sensor pitch
static constexpr uint16_t kAnchorInvalid = 0x00FF;

// ── Frequency Block (one per TX channel) ──
struct FreqBlock {
    uint16_t anchorRow = kAnchorInvalid;
    uint16_t anchorCol = kAnchorInvalid;
    int16_t  grid[kGridDim][kGridDim]{};
    bool     valid = false;

    void Clear() {
        anchorRow = kAnchorInvalid;
        anchorCol = kAnchorInvalid;
        std::memset(grid, 0, sizeof(grid));
        valid = false;
    }
};

// ── Dual-frequency Grid Data ──
struct AsaGridData {
    FreqBlock tx1;  // primary frequency
    FreqBlock tx2;  // secondary frequency (used for tilt)

    void Clear() { tx1.Clear(); tx2.Clear(); }
};

// ── 1D Projection (row/column sums from 9×9 grid) ──
struct AsaProjection {
    int32_t dim1[kGridDim]{};  // row projection
    int32_t dim2[kGridDim]{};  // col projection
    int     peakIdxDim1 = -1;  // peak index in dim1
    int     peakIdxDim2 = -1;  // peak index in dim2

    void Clear() {
        std::memset(dim1, 0, sizeof(dim1));
        std::memset(dim2, 0, sizeof(dim2));
        peakIdxDim1 = peakIdxDim2 = -1;
    }
};

// ── Grid Peak Unit (flood-fill output) ──
struct GridPeakUnit {
    int     peakRow = -1;
    int     peakCol = -1;
    int32_t peakValue = 0;
    int32_t neighborSum3x3 = 0;
    int     connectedPixels = 0;
    bool    valid = false;
};

// ── Coordinate Result (in 0x400 units) ──
struct AsaCoorResult {
    int32_t dim1 = 0;   // X: range [0, 9*0x400-1]
    int32_t dim2 = 0;   // Y: range [0, 9*0x400-1]
    bool    valid = false;
};

// ── Extract grid data from slave words ──
inline AsaGridData ExtractGridFromSlaveWords(
        const uint16_t* words, int wordCount) {
    AsaGridData out;
    out.Clear();
    if (!words || wordCount < kBlockWords * 2) return out;

    // TX1 block: words[0..82]
    out.tx1.anchorRow = words[0];
    out.tx1.anchorCol = words[1];
    for (int r = 0; r < kGridDim; ++r)
        for (int c = 0; c < kGridDim; ++c)
            out.tx1.grid[r][c] = static_cast<int16_t>(
                words[2 + r * kGridDim + c]);
    out.tx1.valid = (out.tx1.anchorRow != kAnchorInvalid) ||
                    (out.tx1.anchorCol != kAnchorInvalid);

    // TX2 block: words[83..165]
    const uint16_t* tx2 = words + kBlockWords;
    out.tx2.anchorRow = tx2[0];
    out.tx2.anchorCol = tx2[1];
    for (int r = 0; r < kGridDim; ++r)
        for (int c = 0; c < kGridDim; ++c)
            out.tx2.grid[r][c] = static_cast<int16_t>(
                tx2[2 + r * kGridDim + c]);
    // TX2 validity requires TX1 to be valid (TX2 outputs garbage when no pen)
    out.tx2.valid = out.tx1.valid;

    return out;
}

} // namespace Asa
