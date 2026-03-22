#pragma once

#include <cstdint>
#include <algorithm>

namespace Engine {

// =============================================================================
// Edge Compensation LUT -- 1:1 replica of TSACore's CTD_ECProcess
// =============================================================================
// The primary LUT g_ctd256Ln[256] is a monotonically increasing uint16 table
// extracted verbatim from TSACore.dll (address 0x6bb51d80, 512 bytes).
// It maps a pixel subpixel index (0..255) to a nonlinear "logarithmic" scale
// used for piecewise interpolation of edge push offsets.
//
// Algorithm flow:
//   1. CTD_ECGetOffset(subpixelIdx, edgeWidth) -> computes a Q8 fractional
//      offset (0..255) by doing piecewise linear interpolation inside g_ctd256Ln
//      using the segment bounds from g_tsaPrmtRam.
//   2. CTD_ECGetFinalOffset(rawDist, 256 - offset) -> applies a blending
//      formula that smoothly transitions between the raw distance and the
//      compensated offset.
//   3. The final coordinate = boundaryCoord ± finalOffset.
// =============================================================================

// ---- Official g_ctd256Ln[256] from firmware memory dump ----
static const uint16_t g_ctd256Ln[256] = {
    0x0000, 0x0000, 0x00B1, 0x0119, 0x0162, 0x019C, 0x01CA, 0x01F2,
    0x0214, 0x0232, 0x024D, 0x0265, 0x027C, 0x0290, 0x02A3, 0x02B5,
    0x02C5, 0x02D5, 0x02E3, 0x02F1, 0x02FE, 0x030B, 0x0317, 0x0322,
    0x032D, 0x0338, 0x0342, 0x034B, 0x0355, 0x035E, 0x0366, 0x036F,
    0x0377, 0x037F, 0x0386, 0x038E, 0x0395, 0x039C, 0x03A3, 0x03A9,
    0x03B0, 0x03B6, 0x03BC, 0x03C2, 0x03C8, 0x03CE, 0x03D4, 0x03D9,
    0x03DF, 0x03E4, 0x03E9, 0x03EE, 0x03F3, 0x03F8, 0x03FD, 0x0401,
    0x0406, 0x040B, 0x040F, 0x0413, 0x0418, 0x041C, 0x0420, 0x0424,
    0x0428, 0x042C, 0x0430, 0x0434, 0x0438, 0x043B, 0x043F, 0x0443,
    0x0446, 0x044A, 0x044D, 0x0451, 0x0454, 0x0458, 0x045B, 0x045E,
    0x0461, 0x0464, 0x0468, 0x046B, 0x046E, 0x0471, 0x0474, 0x0477,
    0x047A, 0x047D, 0x047F, 0x0482, 0x0485, 0x0488, 0x048B, 0x048D,
    0x0490, 0x0493, 0x0495, 0x0498, 0x049A, 0x049D, 0x049F, 0x04A2,
    0x04A4, 0x04A7, 0x04A9, 0x04AC, 0x04AE, 0x04B0, 0x04B3, 0x04B5,
    0x04B7, 0x04BA, 0x04BC, 0x04BE, 0x04C0, 0x04C3, 0x04C5, 0x04C7,
    0x04C9, 0x04CB, 0x04CD, 0x04CF, 0x04D1, 0x04D4, 0x04D6, 0x04D8,
    0x04DA, 0x04DC, 0x04DE, 0x04E0, 0x04E1, 0x04E3, 0x04E5, 0x04E7,
    0x04E9, 0x04EB, 0x04ED, 0x04EF, 0x04F1, 0x04F2, 0x04F4, 0x04F6,
    0x04F8, 0x04FA, 0x04FB, 0x04FD, 0x04FF, 0x0501, 0x0502, 0x0504,
    0x0506, 0x0507, 0x0509, 0x050B, 0x050C, 0x050E, 0x0510, 0x0511,
    0x0513, 0x0514, 0x0516, 0x0518, 0x0519, 0x051B, 0x051C, 0x051E,
    0x051F, 0x0521, 0x0522, 0x0524, 0x0525, 0x0527, 0x0528, 0x052A,
    0x052B, 0x052D, 0x052E, 0x052F, 0x0531, 0x0532, 0x0534, 0x0535,
    0x0537, 0x0538, 0x0539, 0x053B, 0x053C, 0x053D, 0x053F, 0x0540,
    0x0541, 0x0543, 0x0544, 0x0545, 0x0547, 0x0548, 0x0549, 0x054B,
    0x054C, 0x054D, 0x054E, 0x0550, 0x0551, 0x0552, 0x0553, 0x0555,
    0x0556, 0x0557, 0x0558, 0x055A, 0x055B, 0x055C, 0x055D, 0x055E,
    0x0560, 0x0561, 0x0562, 0x0563, 0x0564, 0x0565, 0x0567, 0x0568,
    0x0569, 0x056A, 0x056B, 0x056C, 0x056D, 0x056F, 0x0570, 0x0571,
    0x0572, 0x0573, 0x0574, 0x0575, 0x0576, 0x0577, 0x0578, 0x0579,
    0x057B, 0x057C, 0x057D, 0x057E, 0x057F, 0x0580, 0x0581, 0x0582,
    0x0583, 0x0584, 0x0585, 0x0586, 0x0587, 0x0588, 0x0589, 0x058A
};

// ---- Piecewise segment descriptor (replaces g_tsaPrmtRam) ----
// Structure: { numSegments, [edgeWidthThreshold, lutIndexLow, lutIndexHigh] * N }
// The official firmware stores 4 such descriptors (Dim1Near, Dim1Far, Dim2Near, Dim2Far)
// in TSAPrmt.dll's runtime heap. Since these are device-specific, we provide
// sensible defaults that match a typical 40x60 panel geometry.
struct ECSegment {
    uint8_t edgeWidthThreshold; // Subpixel edge width threshold (0..255)
    uint8_t lutIdxLow;          // LUT index for lower bound interpolation
    uint8_t lutIdxHigh;         // LUT index for upper bound interpolation
};

struct ECProfile {
    uint8_t numSegments;
    ECSegment segments[4]; // Max 4 segments per edge
};

// Default profiles: these approximate the typical Himax panel configuration.
// Users can tune via UI or by hooking runtime parameter extraction.
static const ECProfile g_defaultECProfiles[4] = {
    // Dim1 Near (left edge / row 0)
    { 3, { {64, 2, 32}, {128, 32, 96}, {255, 96, 192}, {0,0,0} } },
    // Dim1 Far (right edge / row max)
    { 3, { {64, 2, 32}, {128, 32, 96}, {255, 96, 192}, {0,0,0} } },
    // Dim2 Near (top edge / col 0)
    { 3, { {64, 2, 32}, {128, 32, 96}, {255, 96, 192}, {0,0,0} } },
    // Dim2 Far (bottom edge / col max)
    { 3, { {64, 2, 32}, {128, 32, 96}, {255, 96, 192}, {0,0,0} } },
};

// ---- 1:1 replica of CTD_ECGetOffset ----
// Given a subpixel centroid fraction (0..255) and the edge width of the touch zone,
// returns a Q8 fraction (0..255) representing how much to compensate.
inline int CTD_ECGetOffset(uint8_t subpixelIdx, uint8_t edgeWidth, const ECProfile& profile) {
    // Find the matching segment
    int segIdx = 0;
    while (segIdx < (int)(profile.numSegments - 1) &&
           profile.segments[segIdx].edgeWidthThreshold < edgeWidth) {
        segIdx++;
    }

    const auto& seg = profile.segments[segIdx];
    uint16_t lutHigh = g_ctd256Ln[seg.lutIdxHigh];
    uint16_t lutLow  = g_ctd256Ln[seg.lutIdxLow];

    int result;
    if (lutHigh - lutLow == 0) {
        result = 0;
    } else {
        // Piecewise linear interpolation within the LUT
        result = ((int)(g_ctd256Ln[subpixelIdx] - lutLow) * 0x100) / (int)(lutHigh - lutLow);
    }

    return std::min(result, 0xFF);
}

// ---- 1:1 replica of CTD_ECGetFinalOffset ----
// Blends the raw distance with the compensated offset using a smooth transition zone.
// param_1: raw pixel distance from edge boundary (Q8.8 format difference)
// param_2: 256 - offset from CTD_ECGetOffset (Q8 complement)
// Returns: the final offset to apply to the coordinate.
inline int CTD_ECGetFinalOffset(int rawDist, int complementOffset) {
    int excess = rawDist - 0x100;
    int result = complementOffset;

    if (excess > 0) {
        result = rawDist; // Beyond transition zone: use raw distance
        if (excess < 0x40) {
            // Inside transition zone (0x100..0x140): blend
            result = rawDist * excess * 4 + (excess * -4 + 0x100) * complementOffset;
            if (result < 0) result += 0xFF;
            result >>= 8;
        }
    }

    return result;
}

} // namespace Engine
