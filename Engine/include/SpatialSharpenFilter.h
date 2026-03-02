#pragma once
#include "IFrameProcessor.h"
#include <string>

namespace Engine {

class SpatialSharpenFilter : public IFrameProcessor {
public:
    SpatialSharpenFilter() = default;
    ~SpatialSharpenFilter() override = default;

    bool Process(HeatmapFrame& frame) override;
    
    std::string GetName() const override { return "Spatial Sharpen Filter"; }
    
    bool IsEnabled() const override { return m_enabled; }
    void SetEnabled(bool enabled) override { m_enabled = enabled; }
    
    void DrawConfigUI() override;
    
    void SaveConfig(std::ostream& out) const override {
        IFrameProcessor::SaveConfig(out);
        out << "Strength=" << m_strength << "\n";
    }
    
    void LoadConfig(const std::string& key, const std::string& value) override {
        IFrameProcessor::LoadConfig(key, value);
        if (key == "Strength") m_strength = std::stof(value);
    }

private:
    bool m_enabled = false; // Default off, let the user toggle when needed
    float m_strength = 1.0f; // Sharpening factor
};

} // namespace Engine
