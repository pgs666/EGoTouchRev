#include "FeatureExtractor.h"
#include "imgui.h"

namespace Engine {

bool FeatureExtractor::Process(HeatmapFrame& frame) {
    if (!m_enabled) return true;
    frame.contacts.clear();

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // Step 1: Peak detection (TSACore Peak_Process)
        m_peakDet.Detect(frame);
        // Step 2: Zone flood-fill + centroid → contacts (TSACore TZ_Process)
        m_zoneExp.Process(frame, m_peakDet.GetPeaks(), m_peakDet.m_threshold);
        // Step 3: Edge compensation (TSACore CTD_ECProcess)
        m_edgeComp.Process(frame.contacts,
                           m_zoneExp.GetEdgeInfos(),
                           m_zoneExp.m_edgeBounds);
        // Step 4: Touch size in mm (TSACore TS_Process)
        m_touchSize.Process(frame.contacts);
        // Step 5: Edge rejection (TSACore ER_Process)
        m_edgeReject.Process(frame.contacts,
                             m_zoneExp.GetEdgeInfos(),
                             m_zoneExp.m_edgeBounds);

        m_cachedPeakCount = static_cast<int>(m_peakDet.GetPeaks().size());
        m_cachedZoneCount = m_zoneExp.GetZoneCount();
        m_cachedContactCount = static_cast<int>(frame.contacts.size());
    }

    return true;
}

void FeatureExtractor::DrawConfigUI() {
    ImGui::TextWrapped("Feature Extractor (TSACore-aligned)");

    ImGui::SeparatorText("Peak Detection");
    int thresh = m_peakDet.m_threshold;
    if (ImGui::SliderInt("Peak Threshold", &thresh, 5, 2000))
        m_peakDet.m_threshold = static_cast<int16_t>(thresh);
    int sigLimit = m_peakDet.m_sigTholdLimit;
    if (ImGui::SliderInt("Sig Thold Limit", &sigLimit, 100, 4000))
        m_peakDet.m_sigTholdLimit = static_cast<int16_t>(sigLimit);
    ImGui::Checkbox("Z8 Filter", &m_peakDet.m_z8Filter);
    ImGui::SameLine();
    ImGui::Checkbox("Z1 Filter", &m_peakDet.m_z1Filter);
    ImGui::Checkbox("PressureDrift Filter", &m_peakDet.m_pressureDriftFilter);
    ImGui::SameLine();
    ImGui::Checkbox("Edge Peak Filter", &m_peakDet.m_edgePeakFilter);
    ImGui::Checkbox("Edge Threshold##en", &m_peakDet.m_edgeThresholdEnabled);
    if (m_peakDet.m_edgeThresholdEnabled) {
        ImGui::SameLine();
        int et = m_peakDet.m_edgeThreshold;
        if (ImGui::SliderInt("##EdgeTh", &et, 5, 2000))
            m_peakDet.m_edgeThreshold = static_cast<int16_t>(et);
    }
    ImGui::Text("Peaks: %d / %d", m_cachedPeakCount, PeakDetector::kMaxPeaks);

    ImGui::SeparatorText("Touch Zone (TZ_Process)");
    int scale = m_zoneExp.m_tholdScaleNumer;
    if (ImGui::SliderInt("Zone Thold Scale", &scale, 16, 127, "%d/128"))
        m_zoneExp.m_tholdScaleNumer = scale;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("zoneThold = min(sigThold,peakSig) * scale/128\nHigher = smaller zones, less merging");
    ImGui::Checkbox("Dilate & Erode", &m_zoneExp.m_dilateErode);
    ImGui::Text("Zones: %d  |  Contacts: %d",
                m_cachedZoneCount, m_cachedContactCount);

    ImGui::SeparatorText("Edge Compensation");
    ImGui::Checkbox("EC Enabled", &m_edgeComp.m_enabled);
    ImGui::SliderFloat("EC Blend Range", &m_edgeComp.m_ecBlendRange, 0.25f, 5.0f, "%.2f grids");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far from edge the blend zone extends.\nOriginal firmware: 0.25. Increase to push edge coords further.");
}

void FeatureExtractor::SaveConfig(std::ostream& out) const {
    IFrameProcessor::SaveConfig(out);
    out << "PeakThreshold=" << m_peakDet.m_threshold << "\n";
    out << "SigTholdLimit=" << m_peakDet.m_sigTholdLimit << "\n";
    out << "Z8FilterEnabled=" << (m_peakDet.m_z8Filter?"1":"0") << "\n";
    out << "Z1FilterEnabled=" << (m_peakDet.m_z1Filter?"1":"0") << "\n";
    out << "PressureDriftFilter=" << (m_peakDet.m_pressureDriftFilter?"1":"0") << "\n";
    out << "EdgePeakFilter=" << (m_peakDet.m_edgePeakFilter?"1":"0") << "\n";
    out << "EdgeThresholdEnabled=" << (m_peakDet.m_edgeThresholdEnabled?"1":"0") << "\n";
    out << "EdgeThreshold=" << m_peakDet.m_edgeThreshold << "\n";
    out << "DilateErode=" << (m_zoneExp.m_dilateErode?"1":"0") << "\n";
    out << "ZoneTholdScale=" << static_cast<int>(m_zoneExp.m_tholdScaleNumer) << "\n";
    out << "ECEnabled=" << (m_edgeComp.m_enabled?"1":"0") << "\n";
    out << "ECBlendRange=" << m_edgeComp.m_ecBlendRange << "\n";
}

void FeatureExtractor::LoadConfig(const std::string& key,
                                  const std::string& value) {
    IFrameProcessor::LoadConfig(key, value);
    if (key == "PeakThreshold")
        m_peakDet.m_threshold = static_cast<int16_t>(std::stoi(value));
    else if (key == "SigTholdLimit")
        m_peakDet.m_sigTholdLimit = static_cast<int16_t>(std::stoi(value));
    else if (key == "Z8FilterEnabled")
        m_peakDet.m_z8Filter = (value == "1");
    else if (key == "Z1FilterEnabled")
        m_peakDet.m_z1Filter = (value == "1");
    else if (key == "PressureDriftFilter")
        m_peakDet.m_pressureDriftFilter = (value == "1");
    else if (key == "EdgePeakFilter")
        m_peakDet.m_edgePeakFilter = (value == "1");
    else if (key == "EdgeThresholdEnabled")
        m_peakDet.m_edgeThresholdEnabled = (value == "1");
    else if (key == "EdgeThreshold")
        m_peakDet.m_edgeThreshold = static_cast<int16_t>(std::stoi(value));
    else if (key == "DilateErode")
        m_zoneExp.m_dilateErode = (value == "1");
    else if (key == "ZoneTholdScale")
        m_zoneExp.m_tholdScaleNumer = std::stoi(value);
    else if (key == "ECEnabled")
        m_edgeComp.m_enabled = (value == "1");
    else if (key == "ECBlendRange")
        m_edgeComp.m_ecBlendRange = std::stof(value);
}

} // namespace Engine
