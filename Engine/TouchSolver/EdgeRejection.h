#pragma once

#include "EngineTypes.h"
#include "EdgeCompensation.h"
#include <vector>

namespace Engine {

// TSACore ER_Process — Edge Rejection.
// Suppresses touch-down events at sensor boundaries:
//   Move-In:  delay/reject touches that first appear at edge
//   Move-Out: allow touches that move from center to edge
class EdgeRejector {
public:
    void Process(std::vector<TouchContact>& contacts,
                 const std::vector<ZoneEdgeInfo>& edgeInfos,
                 const EdgeBounds& bounds);

    bool m_enabled = true;
    // Min frames touching edge before accepting a new touch
    int m_moveInDelay = 2;
    // Edge margin in grid cells for rejection zone
    int m_edgeMargin = 2;
};

} // namespace Engine
