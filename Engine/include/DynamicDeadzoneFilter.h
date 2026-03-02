#pragma once

#include "FramePipeline.h"
#include <string>

namespace Engine {

// 动态行死区滤波器
// 针对电容屏特有的“行共模噪声 (Row Common-mode Noise)”
// 取每行当前的最大波峰的百分比作为动态水平面，切除该水平面以下的起伏。
class DynamicDeadzoneFilter : public IFrameProcessor {
public:
    DynamicDeadzoneFilter() = default;
    ~DynamicDeadzoneFilter() override = default;

    bool Process(HeatmapFrame& frame) override;
    std::string GetName() const override { return "Dynamic Row Deadzone"; }

    void DrawConfigUI() override;
    
    void SaveConfig(std::ostream& out) const override {
        IFrameProcessor::SaveConfig(out);
        out << "ShrinkPercent=" << m_shrinkPercent << "\n";
    }
    
    void LoadConfig(const std::string& key, const std::string& value) override {
        IFrameProcessor::LoadConfig(key, value);
        if (key == "ShrinkPercent") m_shrinkPercent = std::stoi(value);
    }

private:
    int m_shrinkPercent = 20; // 默认取行最高峰的 20% 作为噪声截断线
};

} // namespace Engine
