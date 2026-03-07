#include "BaselineSubtraction.h"
#include <cstring>
#include "imgui.h"

namespace Engine {

bool BaselineSubtraction::Process(HeatmapFrame& frame) {
    if (!m_enabled) return true;

    int16_t* ptr = reinterpret_cast<int16_t*>(frame.heatmapMatrix);

    int16_t base = static_cast<int16_t>(m_baseline);

    // 使用标准 C++ 循环以解决潜在的 SSE/NEON 地址未对齐异常
    for(int i = 0; i < 2400; ++i) {
        ptr[i] = ptr[i] - base;
    }

    return true;
}

void BaselineSubtraction::DrawConfigUI() {
    ImGui::TextWrapped("Subtracts a baseline from the raw AD values to center the signal.");
    
    // 输入框，方便精确输入
    ImGui::InputInt("Baseline Value", &m_baseline, 1, 100);
    
    // 滑动条，范围设定在 0 到 65535 之间（16位无符号范围），但这里用带符号int存储处理
    ImGui::SliderInt("Baseline Slider", &m_baseline, 0, 65535, "%d");
    
    // 重置按钮
    if (ImGui::Button("Reset to Default (0x7FFE)")) {
        m_baseline = 0x7FFE;
    }
}

void BaselineSubtraction::LoadConfig(const std::string& key, const std::string& value) {
    if (key == "BaselineValue") {
        try {
            m_baseline = std::stoi(value);
        } catch (...) {
            m_baseline = 0x7FFE;
        }
    }
}

void BaselineSubtraction::SaveConfig(std::ostream& out) const {
    out << "BaselineValue=" << m_baseline << "\n";
}

} // namespace Engine
