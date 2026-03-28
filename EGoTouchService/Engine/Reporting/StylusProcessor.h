#pragma once
// ═══════════════════════════════════════════════════════════════
// DEPRECATED — StylusProcessor
// All logic has been migrated to Engine::StylusPipeline
// (Engine/StylusSolver/StylusPipeline.h).
// This stub is retained only for backward compilation compatibility
// (e.g. ServiceProxy GUI config pipeline listing).
// ═══════════════════════════════════════════════════════════════

#include "IFrameProcessor.h"

namespace Engine {

class StylusProcessor : public IFrameProcessor {
public:
    bool Process(HeatmapFrame&) override { return true; }
    std::string GetName() const override {
        return "StylusProcessor [DEPRECATED]";
    }
    void DrawConfigUI() override {}
    void SaveConfig(std::ostream&) const override {}
    void LoadConfig(const std::string&,
                    const std::string&) override {}
};

} // namespace Engine
