#pragma once

#include "EngineTypes.h"
#include <cstdint>
#include <vector>

namespace Engine {

// TSACore TS_Process — touch size calculation.
// Converts zone signalSum → approximate radius in mm.
class TouchSizeCalculator {
public:
    // Called after IDT_Process / TE_Process, before report.
    void Process(std::vector<TouchContact>& contacts);

    // Tuneable: pixel pitch in mm (per grid cell)
    // Default for typical 40×60 panel ≈ 4.5mm pitch
    float m_pixelPitchMm = 4.5f;

    // TSACore DAT_1826b61e: per-signal-mm² scale factor
    // Controls how sigSum maps to radius.
    int m_unitPerSigMm2 = 128;

    // Fallback size when sigSum is 0
    uint8_t m_fallbackSizeMm = 5;
};

} // namespace Engine
