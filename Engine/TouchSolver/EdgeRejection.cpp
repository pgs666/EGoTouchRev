#include "EdgeRejection.h"

namespace Engine {

void EdgeRejector::Process(
        std::vector<TouchContact>& contacts,
        const std::vector<ZoneEdgeInfo>& edgeInfos,
        const EdgeBounds& bounds) {
    if (!m_enabled) return;

    for (int i = 0; i < (int)contacts.size(); ++i) {
        if (i >= (int)edgeInfos.size()) break;
        auto& tc = contacts[i];
        auto& ei = edgeInfos[i];

        // Skip non-edge touches
        if (!(ei.edgeFlags & 0x20)) continue;

        // ER_IsTouchNeedMoveInCorrection:
        // New touch appearing at sensor boundary
        bool atEdge =
            ei.minCol <= bounds.colMin + m_edgeMargin ||
            ei.maxCol >= bounds.colMax - m_edgeMargin ||
            ei.minRow <= bounds.rowMin + m_edgeMargin ||
            ei.maxRow >= bounds.rowMax - m_edgeMargin;

        if (!atEdge) continue;

        // Move-In: suppress new touches at edge
        // (state==0 means touch-down)
        if (tc.state == 0) {
            tc.debugFlags |= 0x400; // ER move-in flag
            tc.isReported = false;  // Suppress report
        }
    }
}

} // namespace Engine
