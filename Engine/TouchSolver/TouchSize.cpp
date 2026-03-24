#include "TouchSize.h"

namespace Engine {

// TSACore TS_GetSizeInMM replica:
// Iterates radius 1..14, finds smallest r where:
//   scale * r * (r + r²) >= sigSum << 10
static uint8_t GetSizeInMM(int sigSum, int scale) {
    if (sigSum >= 0x200000) return 0xFF; // overflow
    uint8_t r = 1;
    int shifted = sigSum << 10;
    while (r < 15) {
        int threshold = scale * r * (r + r * r);
        if (threshold > shifted) break;
        r++;
    }
    return r;
}

void TouchSizeCalculator::Process(
        std::vector<TouchContact>& contacts) {
    for (auto& tc : contacts) {
        uint8_t sizeMm = GetSizeInMM(
            tc.signalSum, m_unitPerSigMm2);
        if (sizeMm == 0) sizeMm = m_fallbackSizeMm;
        tc.sizeMm = static_cast<float>(sizeMm);
    }
}

} // namespace Engine
