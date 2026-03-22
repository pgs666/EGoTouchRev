#include "StylusProcessor.h"

#include "imgui.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace Engine {

namespace {

inline uint16_t ReadU16Le(const uint8_t* ptr) {
    return static_cast<uint16_t>(ptr[0]) | (static_cast<uint16_t>(ptr[1]) << 8);
}

template <size_t N>
inline void WriteU16Le(std::array<uint8_t, N>& buf, int offset, uint16_t value) {
    buf[static_cast<size_t>(offset)] = static_cast<uint8_t>(value & 0xFFu);
    buf[static_cast<size_t>(offset + 1)] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

inline uint8_t ToDataType(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 3));
}

struct SlaveLayoutCandidate {
    uint8_t wordOffset = 0;
    uint16_t checksum16 = 0;
    bool checksumOk = false;
    int headerScore = 0;
    bool usable = false;
};

} // namespace

bool StylusProcessor::Process(HeatmapFrame& frame) {
    if (!m_enabled) {
        return true;
    }

    ResetStylusFrame(frame);

    std::array<uint16_t, kSlaveWordCount> slaveWords{};
    uint8_t wordOffset = 0;
    uint16_t checksum16 = 0;
    bool checksumOk = false;
    if (!ParseSlaveWords(frame, slaveWords, wordOffset, checksum16, checksumOk)) {
        frame.stylus.processResult = 1;
        UpdateTouchSuppressionState(frame);
        if (!m_requireSlaveFrame) {
            BuildStylusPacket(frame);
        }
        return true;
    }

    frame.stylus.slaveValid = true;
    frame.stylus.slaveWordOffset = wordOffset;
    frame.stylus.checksum16 = checksum16;
    frame.stylus.checksumOk = checksumOk;
    frame.stylus.slaveWords = slaveWords;

    const uint16_t* tx1Block = slaveWords.data();
    const uint16_t* tx2Block = slaveWords.data() + kStylusBlockWords;
    frame.stylus.tx1BlockValid = UnpackBlockToMatrix(tx1Block, frame.stylus.tx1Matrix);
    frame.stylus.tx2BlockValid = UnpackBlockToMatrix(tx2Block, frame.stylus.tx2Matrix);

    SolveStylusPoint(frame);
    RunAsaMainProcess(frame);
    RunStylusRecheck(frame);
    UpdateTouchSuppressionState(frame);
    BuildStylusPacket(frame);
    return true;
}

void StylusProcessor::DrawConfigUI() {
    ImGui::SliderInt("Stylus Peak Delta Threshold", &m_peakDeltaThreshold, 10, 3000);
    ImGui::SliderInt("Stylus Centroid Radius", &m_centroidRadius, 0, 3);
    ImGui::SliderFloat("Stylus Confidence Scale", &m_confidenceScale, 50.0f, 4000.0f, "%.1f");
    ImGui::SliderInt("Stylus Report X Max", &m_reportXMax, 255, 32767);
    ImGui::SliderInt("Stylus Report Y Max", &m_reportYMax, 255, 32767);
    ImGui::Checkbox("Stylus Require Slave Frame", &m_requireSlaveFrame);
    ImGui::Checkbox("Stylus Enable Slave Checksum", &m_enableSlaveChecksum);
    ImGui::Checkbox("Stylus Emit Invalid Packet", &m_emitPacketWhenInvalid);

    ImGui::Separator();
    ImGui::Text("ASA/HPP");
    ImGui::SliderInt("Protocol Type (1/2)", &m_protocolType, 1, 2);
    ImGui::SliderInt("Forced ASA Mode (0/1/2)", &m_forcedAsaMode, 0, 2);
    ImGui::SliderInt("DataType (0/1/2/3)", &m_dataType, 0, 3);
    ImGui::Checkbox("HPP3 Touch Enable Feature", &m_hpp3TouchEnableFeature);

    ImGui::Separator();
    ImGui::Text("Raw Stylus Meta (Master Suffix)");
    ImGui::Checkbox("Master Meta Enabled", &m_masterMetaEnabled);
    ImGui::Checkbox("Master Meta Auto Detect", &m_masterMetaAutoDetect);
    ImGui::SliderInt("Master Meta Base Word (-1=Auto)", &m_masterMetaBaseWord, -1, 112);
    ImGui::Text("Master Meta Detected Base: %d",
                (m_masterMetaDetectedBaseWord == 0xFF) ? -1 : static_cast<int>(m_masterMetaDetectedBaseWord));

    ImGui::Separator();
    ImGui::Text("Freq Shift");
    ImGui::SliderFloat("Stylus Freq Score Decay", &m_freqScoreDecay, 0.50f, 0.999f, "%.3f");
    ImGui::SliderFloat("Stylus Freq Switch Threshold", &m_freqSwitchScoreThreshold, 500.0f, 12000.0f, "%.0f");
    ImGui::SliderInt("Stylus Freq Request Hold", &m_freqRequestHoldFrames, 1, 8);
    ImGui::SliderInt("Stylus Unstable Streak Threshold", &m_unstableStreakThreshold, 1, 12);

    ImGui::Separator();
    ImGui::Text("HPP3 Noise Post");
    ImGui::Checkbox("HPP3 Noise Post Enabled", &m_hpp3NoisePostEnabled);
    ImGui::SliderInt("HPP3 Sig Ratio Factor", &m_hpp3SignalRatioFactor, 2, 8);
    ImGui::SliderInt("HPP3 Sig Drop Factor", &m_hpp3SignalDropFactor, 2, 8);
    ImGui::SliderFloat("HPP3 Coor Jump Th", &m_hpp3CoorJumpThreshold, 1.0f, 20.0f, "%.1f");
    ImGui::SliderInt("HPP3 Noise Debounce ms", &m_hpp3NoiseDebounceMs, 1, 50);

    ImGui::Separator();
    ImGui::Text("StylusRecheck");
    ImGui::Checkbox("Recheck Enabled", &m_recheckEnabled);
    ImGui::Checkbox("Disable Recheck In FreqShifting", &m_recheckDisableInFreqShifting);
    ImGui::Checkbox("Skip Recheck On NoPressInk", &m_skipRecheckOnNoPressInk);
    ImGui::Checkbox("WindowsPad Mode", &m_windowsPadMode);
    ImGui::Checkbox("Skip On Invalid Raw", &m_skipOnInvalidRawEnabled);
    ImGui::SliderInt("Noise Level", &m_noiseLevel, 0, 8);
    ImGui::SliderInt("Recheck SigTh Base", &m_recheckSignalThreshBase, 20, 600);
    ImGui::SliderInt("Recheck SigTh Noisy", &m_recheckSignalThreshNoisy, 20, 800);
    ImGui::SliderInt("Recheck SigTh Strong", &m_recheckSignalThreshStrong, 20, 600);
    ImGui::SliderInt("Recheck SigTh VeryStrong", &m_recheckSignalThreshVeryStrong, 20, 600);
    ImGui::SliderInt("Strong Peak Threshold", &m_recheckStrongPeakThreshold, 100, 3000);
    ImGui::SliderInt("Strong Ratio Q8", &m_recheckStrongRatioQ8, 64, 1024);
    ImGui::SliderInt("VeryStrong Ratio Q8", &m_recheckVeryStrongRatioQ8, 128, 2048);
    ImGui::Checkbox("Recheck Overlap Enabled", &m_recheckOverlapEnabled);
    ImGui::SliderFloat("Overlap Distance", &m_recheckOverlapDistance, 0.5f, 10.0f, "%.2f");
    ImGui::SliderInt("Overlap Peak Th", &m_recheckOverlapPeakThreshold, 100, 4000);
    ImGui::SliderInt("Overlap Touch Sig Th", &m_recheckOverlapTouchSignalThreshold, 100, 12000);
    ImGui::SliderInt("Overlap Touch Sig Th (WP)", &m_recheckOverlapTouchSignalThresholdWp, 100, 12000);

    ImGui::Separator();
    ImGui::Text("Pressure Solve");
    ImGui::Text("Pressure Map Mode: 2 (incell, hardcoded)");
    ImGui::Checkbox("Pressure Polynomial Map", &m_pressurePolyEnabled);
    ImGui::SliderInt("Pressure Map Seg1 Th", &m_pressureMapSeg1Threshold, 1, 4095);
    ImGui::SliderInt("Pressure Map Seg2 Th", &m_pressureMapSeg2Threshold, 1, 4095);
    ImGui::InputDouble("Pressure Poly1 C0", &m_pressurePolySeg1[0], 0.01, 0.1, "%.6f");
    ImGui::InputDouble("Pressure Poly1 C1", &m_pressurePolySeg1[1], 0.001, 0.01, "%.6f");
    ImGui::InputDouble("Pressure Poly1 C2", &m_pressurePolySeg1[2], 0.0001, 0.001, "%.8f");
    ImGui::InputDouble("Pressure Poly1 C3", &m_pressurePolySeg1[3], 0.00001, 0.0001, "%.10f");
    ImGui::InputDouble("Pressure Poly1 C4", &m_pressurePolySeg1[4], 0.000001, 0.00001, "%.12f");
    ImGui::InputDouble("Pressure Poly2 C0", &m_pressurePolySeg2[0], 0.01, 0.1, "%.6f");
    ImGui::InputDouble("Pressure Poly2 C1", &m_pressurePolySeg2[1], 0.001, 0.01, "%.6f");
    ImGui::InputDouble("Pressure Poly2 C2", &m_pressurePolySeg2[2], 0.0001, 0.001, "%.8f");
    ImGui::InputDouble("Pressure Poly2 C3", &m_pressurePolySeg2[3], 0.00001, 0.0001, "%.10f");
    ImGui::InputDouble("Pressure Poly2 C4", &m_pressurePolySeg2[4], 0.000001, 0.00001, "%.12f");
    ImGui::SliderInt("Pressure Map Gain %", &m_pressureMapGainPercent, 10, 300);
    ImGui::SliderInt("Pressure IIR Weight Q7", &m_pressureIirWeightQ7, 1, 127);
    ImGui::SliderInt("Pressure Tail Frames", &m_pressureTailFrames, 0, 8);
    ImGui::SliderInt("Pressure Tail Min", &m_pressureTailMin, 0, 4095);
    ImGui::SliderInt("Pressure Tail Decay", &m_pressureTailDecay, 1, 256);
    ImGui::Checkbox("Pressure Edge Suppress", &m_pressureEdgeSuppressEnabled);
    ImGui::SliderInt("Pressure Edge Margin", &m_pressureEdgeMarginCells, 0, 8);
    ImGui::SliderInt("Pressure Edge Sig Th", &m_pressureEdgeSignalThreshold, 1, 2000);
    ImGui::SliderInt("Pressure Edge Sig Release Th", &m_pressureEdgeSignalReleaseThreshold, 1, 3000);
    ImGui::Checkbox("Pressure Signal Suppress", &m_pressureSignalSuppressEnabled);
    ImGui::SliderInt("Pressure Suppress EnterTh", &m_pressureSignalSuppressEnterTh, 1, 1000);
    ImGui::SliderInt("Pressure Suppress ExitTh", &m_pressureSignalSuppressExitTh, 1, 2000);
    ImGui::SliderInt("Fake Pressure Start Th", &m_fakePressureDecreaseStartTh, 1, 3000);
    ImGui::SliderInt("Fake Pressure Level2 Th", &m_fakePressureDecreaseLevel2Th, 1, 3000);
    ImGui::SliderInt("Fake Pressure Level3 Th", &m_fakePressureDecreaseLevel3Th, 1, 4095);
    ImGui::Checkbox("Pressure Post Respect FreqShift", &m_pressurePostRespectFreqShift);
    ImGui::SliderInt("Pressure Post Debounce ms", &m_pressurePostFreqShiftDebounceMs, 1, 500);
    ImGui::SliderInt("Pressure Post Debounce ms (WP)", &m_pressurePostFreqShiftDebounceMsWp, 1, 500);

    ImGui::Separator();
    ImGui::Text("Button Status");
    ImGui::Checkbox("Button State Enabled", &m_buttonStateEnabled);
    ImGui::Checkbox("Button Use Master Meta", &m_buttonUseMasterMeta);
    ImGui::Checkbox("Button Use Slave Word", &m_buttonUseSlaveWord);
    ImGui::SliderInt("Button Slave Word Index", &m_buttonSlaveWordIndex, 0, 165);
    ImGui::SliderInt("Button Raw Bit Shift", &m_buttonRawBitShift, 0, 15);
    int buttonMask = static_cast<int>(m_buttonRawMask);
    if (ImGui::SliderInt("Button Raw Mask", &buttonMask, 1, 0xFFFF)) {
        m_buttonRawMask = static_cast<uint32_t>(buttonMask);
    }
    ImGui::SliderInt("Button Release Hold Frames", &m_buttonReleaseHoldFrames, 0, 8);

    ImGui::Separator();
    ImGui::Text("Tilt Solve");
    ImGui::Checkbox("Tilt Enabled", &m_tiltEnabled);
    ImGui::Checkbox("Tilt Keep Last On Invalid", &m_tiltKeepLastOnInvalid);
    ImGui::Checkbox("Tilt Use Signal Ratio Limit", &m_tiltUseSignalRatioLimit);
    ImGui::SliderInt("Tilt Ratio Avg Window", &m_tiltRatioAverageWindow, 1, 10);
    ImGui::SliderInt("Tilt Ratio Min For Output", &m_tiltRatioMinForOutput, 0, 500);
    ImGui::SliderInt("Tilt Ratio Ref", &m_tiltRatioRef, 1, 500);
    ImGui::SliderFloat("Tilt Ratio Scale Min", &m_tiltRatioScaleMin, 0.1f, 2.0f, "%.2f");
    ImGui::SliderFloat("Tilt Ratio Scale Max", &m_tiltRatioScaleMax, 0.1f, 3.0f, "%.2f");
    ImGui::SliderFloat("Tilt Coord Diff Limit", &m_tiltCoordDiffLimit, 0.5f, 20.0f, "%.2f");
    ImGui::SliderInt("Tilt Len Ratio Point Cnt", &m_tiltLenRatioPointCount, 0, 6);
    for (int i = 0; i < 6; ++i) {
        const std::string thLabel = "Tilt Len Ratio Th[" + std::to_string(i) + "]";
        int th = static_cast<int>(m_tiltLenRatioThresholds[static_cast<size_t>(i)]);
        if (ImGui::SliderInt(thLabel.c_str(), &th, 0, 500)) {
            m_tiltLenRatioThresholds[static_cast<size_t>(i)] = static_cast<uint16_t>(th);
        }
        const std::string scLabel = "Tilt Len Scale Permille[" + std::to_string(i) + "]";
        int sc = static_cast<int>(m_tiltLenScalePermille[static_cast<size_t>(i)]);
        if (ImGui::SliderInt(scLabel.c_str(), &sc, 0, 2000)) {
            m_tiltLenScalePermille[static_cast<size_t>(i)] = static_cast<uint16_t>(sc);
        }
    }
    ImGui::SliderInt("Tilt Diff Avg Window", &m_tiltDiffAverageWindow, 1, 10);
    ImGui::SliderInt("Tilt Out Avg Window", &m_tiltOutAverageWindow, 1, 10);
    ImGui::SliderFloat("Tilt Coord IIR Old Weight", &m_tiltCoordIirOldWeight, 0.0f, 0.98f, "%.3f");
    ImGui::SliderFloat("Tilt Degree/Cell X", &m_tiltDegreePerCellX, 1.0f, 20.0f, "%.2f");
    ImGui::SliderFloat("Tilt Degree/Cell Y", &m_tiltDegreePerCellY, 1.0f, 20.0f, "%.2f");
    ImGui::SliderFloat("Tilt Norm Len X", &m_tiltNormLenX, 1.0f, 20.0f, "%.2f");
    ImGui::SliderFloat("Tilt Norm Len Y", &m_tiltNormLenY, 1.0f, 20.0f, "%.2f");
    ImGui::SliderInt("Tilt Max Degree", &m_tiltMaxDegree, 5, 89);
    ImGui::SliderInt("Tilt Jitter Threshold", &m_tiltJitterThresholdDeg, 0, 5);
    ImGui::Checkbox("Tilt Reset When No Pressure", &m_tiltResetWhenNoPressure);

    ImGui::Separator();
    ImGui::SliderInt("Touch Suppress Hold Frames", &m_touchSuppressHoldFrames, 0, 8);
}

void StylusProcessor::SaveConfig(std::ostream& out) const {
    IFrameProcessor::SaveConfig(out);
    out << "PeakDeltaThreshold=" << m_peakDeltaThreshold << "\n";
    out << "CentroidRadius=" << m_centroidRadius << "\n";
    out << "ConfidenceScale=" << m_confidenceScale << "\n";
    out << "ReportXMax=" << m_reportXMax << "\n";
    out << "ReportYMax=" << m_reportYMax << "\n";
    out << "RequireSlaveFrame=" << (m_requireSlaveFrame ? 1 : 0) << "\n";
    out << "EnableSlaveChecksum=" << (m_enableSlaveChecksum ? 1 : 0) << "\n";
    out << "EmitPacketWhenInvalid=" << (m_emitPacketWhenInvalid ? 1 : 0) << "\n";
    out << "ProtocolType=" << m_protocolType << "\n";
    out << "ForcedAsaMode=" << m_forcedAsaMode << "\n";
    out << "DataType=" << m_dataType << "\n";
    out << "Hpp3TouchEnableFeature=" << (m_hpp3TouchEnableFeature ? 1 : 0) << "\n";
    out << "MasterMetaEnabled=" << (m_masterMetaEnabled ? 1 : 0) << "\n";
    out << "MasterMetaAutoDetect=" << (m_masterMetaAutoDetect ? 1 : 0) << "\n";
    out << "MasterMetaBaseWord=" << m_masterMetaBaseWord << "\n";
    out << "FreqA=" << m_freqA << "\n";
    out << "FreqB=" << m_freqB << "\n";
    out << "FreqScoreDecay=" << m_freqScoreDecay << "\n";
    out << "FreqSwitchScoreThreshold=" << m_freqSwitchScoreThreshold << "\n";
    out << "FreqRequestHoldFrames=" << m_freqRequestHoldFrames << "\n";
    out << "UnstableStreakThreshold=" << m_unstableStreakThreshold << "\n";
    out << "Hpp3NoisePostEnabled=" << (m_hpp3NoisePostEnabled ? 1 : 0) << "\n";
    out << "Hpp3SignalRatioFactor=" << m_hpp3SignalRatioFactor << "\n";
    out << "Hpp3SignalDropFactor=" << m_hpp3SignalDropFactor << "\n";
    out << "Hpp3CoorJumpThreshold=" << m_hpp3CoorJumpThreshold << "\n";
    out << "Hpp3NoiseDebounceMs=" << m_hpp3NoiseDebounceMs << "\n";
    out << "RecheckEnabled=" << (m_recheckEnabled ? 1 : 0) << "\n";
    out << "RecheckDisableInFreqShifting=" << (m_recheckDisableInFreqShifting ? 1 : 0) << "\n";
    out << "SkipRecheckOnNoPressInk=" << (m_skipRecheckOnNoPressInk ? 1 : 0) << "\n";
    out << "WindowsPadMode=" << (m_windowsPadMode ? 1 : 0) << "\n";
    out << "SkipOnInvalidRawEnabled=" << (m_skipOnInvalidRawEnabled ? 1 : 0) << "\n";
    out << "NoiseLevel=" << m_noiseLevel << "\n";
    out << "RecheckSignalThreshBase=" << m_recheckSignalThreshBase << "\n";
    out << "RecheckSignalThreshNoisy=" << m_recheckSignalThreshNoisy << "\n";
    out << "RecheckSignalThreshStrong=" << m_recheckSignalThreshStrong << "\n";
    out << "RecheckSignalThreshVeryStrong=" << m_recheckSignalThreshVeryStrong << "\n";
    out << "RecheckStrongPeakThreshold=" << m_recheckStrongPeakThreshold << "\n";
    out << "RecheckStrongRatioQ8=" << m_recheckStrongRatioQ8 << "\n";
    out << "RecheckVeryStrongRatioQ8=" << m_recheckVeryStrongRatioQ8 << "\n";
    out << "RecheckOverlapEnabled=" << (m_recheckOverlapEnabled ? 1 : 0) << "\n";
    out << "RecheckOverlapDistance=" << m_recheckOverlapDistance << "\n";
    out << "RecheckOverlapPeakThreshold=" << m_recheckOverlapPeakThreshold << "\n";
    out << "RecheckOverlapTouchSignalThreshold=" << m_recheckOverlapTouchSignalThreshold << "\n";
    out << "RecheckOverlapTouchSignalThresholdWp=" << m_recheckOverlapTouchSignalThresholdWp << "\n";
    out << "PressureMapMode=2\n";
    out << "PressurePolyEnabled=" << (m_pressurePolyEnabled ? 1 : 0) << "\n";
    out << "PressureMapSeg1Threshold=" << m_pressureMapSeg1Threshold << "\n";
    out << "PressureMapSeg2Threshold=" << m_pressureMapSeg2Threshold << "\n";
    out << "PressurePolySeg1C0=" << m_pressurePolySeg1[0] << "\n";
    out << "PressurePolySeg1C1=" << m_pressurePolySeg1[1] << "\n";
    out << "PressurePolySeg1C2=" << m_pressurePolySeg1[2] << "\n";
    out << "PressurePolySeg1C3=" << m_pressurePolySeg1[3] << "\n";
    out << "PressurePolySeg1C4=" << m_pressurePolySeg1[4] << "\n";
    out << "PressurePolySeg2C0=" << m_pressurePolySeg2[0] << "\n";
    out << "PressurePolySeg2C1=" << m_pressurePolySeg2[1] << "\n";
    out << "PressurePolySeg2C2=" << m_pressurePolySeg2[2] << "\n";
    out << "PressurePolySeg2C3=" << m_pressurePolySeg2[3] << "\n";
    out << "PressurePolySeg2C4=" << m_pressurePolySeg2[4] << "\n";
    out << "PressureMapGainPercent=" << m_pressureMapGainPercent << "\n";
    out << "PressureIirWeightQ7=" << m_pressureIirWeightQ7 << "\n";
    out << "PressureTailFrames=" << m_pressureTailFrames << "\n";
    out << "PressureTailMin=" << m_pressureTailMin << "\n";
    out << "PressureTailDecay=" << m_pressureTailDecay << "\n";
    out << "PressureEdgeSuppressEnabled=" << (m_pressureEdgeSuppressEnabled ? 1 : 0) << "\n";
    out << "PressureEdgeMarginCells=" << m_pressureEdgeMarginCells << "\n";
    out << "PressureEdgeSignalThreshold=" << m_pressureEdgeSignalThreshold << "\n";
    out << "PressureEdgeSignalReleaseThreshold=" << m_pressureEdgeSignalReleaseThreshold << "\n";
    out << "PressureSignalSuppressEnabled=" << (m_pressureSignalSuppressEnabled ? 1 : 0) << "\n";
    out << "PressureSignalSuppressEnterTh=" << m_pressureSignalSuppressEnterTh << "\n";
    out << "PressureSignalSuppressExitTh=" << m_pressureSignalSuppressExitTh << "\n";
    out << "FakePressureDecreaseStartTh=" << m_fakePressureDecreaseStartTh << "\n";
    out << "FakePressureDecreaseLevel2Th=" << m_fakePressureDecreaseLevel2Th << "\n";
    out << "FakePressureDecreaseLevel3Th=" << m_fakePressureDecreaseLevel3Th << "\n";
    out << "PressurePostRespectFreqShift=" << (m_pressurePostRespectFreqShift ? 1 : 0) << "\n";
    out << "PressurePostFreqShiftDebounceMs=" << m_pressurePostFreqShiftDebounceMs << "\n";
    out << "PressurePostFreqShiftDebounceMsWp=" << m_pressurePostFreqShiftDebounceMsWp << "\n";
    out << "ButtonStateEnabled=" << (m_buttonStateEnabled ? 1 : 0) << "\n";
    out << "ButtonUseMasterMeta=" << (m_buttonUseMasterMeta ? 1 : 0) << "\n";
    out << "ButtonUseSlaveWord=" << (m_buttonUseSlaveWord ? 1 : 0) << "\n";
    out << "ButtonSlaveWordIndex=" << m_buttonSlaveWordIndex << "\n";
    out << "ButtonRawBitShift=" << m_buttonRawBitShift << "\n";
    out << "ButtonRawMask=" << m_buttonRawMask << "\n";
    out << "ButtonReleaseHoldFrames=" << m_buttonReleaseHoldFrames << "\n";
    out << "TiltEnabled=" << (m_tiltEnabled ? 1 : 0) << "\n";
    out << "TiltKeepLastOnInvalid=" << (m_tiltKeepLastOnInvalid ? 1 : 0) << "\n";
    out << "TiltUseSignalRatioLimit=" << (m_tiltUseSignalRatioLimit ? 1 : 0) << "\n";
    out << "TiltRatioAverageWindow=" << m_tiltRatioAverageWindow << "\n";
    out << "TiltRatioMinForOutput=" << m_tiltRatioMinForOutput << "\n";
    out << "TiltRatioRef=" << m_tiltRatioRef << "\n";
    out << "TiltRatioScaleMin=" << m_tiltRatioScaleMin << "\n";
    out << "TiltRatioScaleMax=" << m_tiltRatioScaleMax << "\n";
    out << "TiltCoordDiffLimit=" << m_tiltCoordDiffLimit << "\n";
    out << "TiltLenRatioPointCount=" << m_tiltLenRatioPointCount << "\n";
    for (int i = 0; i < 6; ++i) {
        out << "TiltLenRatioThreshold" << i << "=" << m_tiltLenRatioThresholds[static_cast<size_t>(i)] << "\n";
        out << "TiltLenScalePermille" << i << "=" << m_tiltLenScalePermille[static_cast<size_t>(i)] << "\n";
    }
    out << "TiltDiffAverageWindow=" << m_tiltDiffAverageWindow << "\n";
    out << "TiltOutAverageWindow=" << m_tiltOutAverageWindow << "\n";
    out << "TiltCoordIirOldWeight=" << m_tiltCoordIirOldWeight << "\n";
    out << "TiltDegreePerCellX=" << m_tiltDegreePerCellX << "\n";
    out << "TiltDegreePerCellY=" << m_tiltDegreePerCellY << "\n";
    out << "TiltNormLenX=" << m_tiltNormLenX << "\n";
    out << "TiltNormLenY=" << m_tiltNormLenY << "\n";
    out << "TiltMaxDegree=" << m_tiltMaxDegree << "\n";
    out << "TiltJitterThresholdDeg=" << m_tiltJitterThresholdDeg << "\n";
    out << "TiltResetWhenNoPressure=" << (m_tiltResetWhenNoPressure ? 1 : 0) << "\n";
    out << "TouchSuppressHoldFrames=" << m_touchSuppressHoldFrames << "\n";
}

void StylusProcessor::LoadConfig(const std::string& key, const std::string& value) {
    IFrameProcessor::LoadConfig(key, value);
    if (key == "PeakDeltaThreshold") m_peakDeltaThreshold = std::max(0, std::stoi(value));
    else if (key == "CentroidRadius") m_centroidRadius = std::clamp(std::stoi(value), 0, 6);
    else if (key == "ConfidenceScale") m_confidenceScale = std::max(1.0f, std::stof(value));
    else if (key == "ReportXMax") m_reportXMax = std::clamp(std::stoi(value), 16, 65535);
    else if (key == "ReportYMax") m_reportYMax = std::clamp(std::stoi(value), 16, 65535);
    else if (key == "RequireSlaveFrame") m_requireSlaveFrame = (value == "1" || value == "true");
    else if (key == "EnableSlaveChecksum") m_enableSlaveChecksum = (value == "1" || value == "true");
    else if (key == "EmitPacketWhenInvalid") m_emitPacketWhenInvalid = (value == "1" || value == "true");
    else if (key == "ProtocolType") m_protocolType = std::clamp(std::stoi(value), 1, 2);
    else if (key == "ForcedAsaMode") m_forcedAsaMode = std::clamp(std::stoi(value), 0, 2);
    else if (key == "DataType") m_dataType = std::clamp(std::stoi(value), 0, 3);
    else if (key == "Hpp3TouchEnableFeature") m_hpp3TouchEnableFeature = (value == "1" || value == "true");
    else if (key == "MasterMetaEnabled") m_masterMetaEnabled = (value == "1" || value == "true");
    else if (key == "MasterMetaAutoDetect") m_masterMetaAutoDetect = (value == "1" || value == "true");
    else if (key == "MasterMetaBaseWord") m_masterMetaBaseWord = std::clamp(std::stoi(value), -1, 112);
    else if (key == "FreqA") m_freqA = static_cast<uint16_t>(std::clamp(std::stoi(value), 0, 0xFFFF));
    else if (key == "FreqB") m_freqB = static_cast<uint16_t>(std::clamp(std::stoi(value), 0, 0xFFFF));
    else if (key == "FreqScoreDecay") m_freqScoreDecay = std::clamp(std::stof(value), 0.50f, 0.999f);
    else if (key == "FreqSwitchScoreThreshold") m_freqSwitchScoreThreshold = std::max(100.0f, std::stof(value));
    else if (key == "FreqRequestHoldFrames") m_freqRequestHoldFrames = std::clamp(std::stoi(value), 1, 20);
    else if (key == "UnstableStreakThreshold") m_unstableStreakThreshold = std::clamp(std::stoi(value), 1, 50);
    else if (key == "Hpp3NoisePostEnabled") m_hpp3NoisePostEnabled = (value == "1" || value == "true");
    else if (key == "Hpp3SignalRatioFactor") m_hpp3SignalRatioFactor = std::clamp(std::stoi(value), 2, 16);
    else if (key == "Hpp3SignalDropFactor") m_hpp3SignalDropFactor = std::clamp(std::stoi(value), 2, 16);
    else if (key == "Hpp3CoorJumpThreshold") m_hpp3CoorJumpThreshold = std::max(0.1f, std::stof(value));
    else if (key == "Hpp3NoiseDebounceMs") m_hpp3NoiseDebounceMs = std::clamp(std::stoi(value), 1, 200);
    else if (key == "RecheckEnabled") m_recheckEnabled = (value == "1" || value == "true");
    else if (key == "RecheckDisableInFreqShifting") m_recheckDisableInFreqShifting = (value == "1" || value == "true");
    else if (key == "SkipRecheckOnNoPressInk") m_skipRecheckOnNoPressInk = (value == "1" || value == "true");
    else if (key == "WindowsPadMode") m_windowsPadMode = (value == "1" || value == "true");
    else if (key == "SkipOnInvalidRawEnabled") m_skipOnInvalidRawEnabled = (value == "1" || value == "true");
    else if (key == "NoiseLevel") m_noiseLevel = std::clamp(std::stoi(value), 0, 16);
    else if (key == "RecheckSignalThreshBase") m_recheckSignalThreshBase = std::max(0, std::stoi(value));
    else if (key == "RecheckSignalThreshNoisy") m_recheckSignalThreshNoisy = std::max(0, std::stoi(value));
    else if (key == "RecheckSignalThreshStrong") m_recheckSignalThreshStrong = std::max(0, std::stoi(value));
    else if (key == "RecheckSignalThreshVeryStrong") m_recheckSignalThreshVeryStrong = std::max(0, std::stoi(value));
    else if (key == "RecheckStrongPeakThreshold") m_recheckStrongPeakThreshold = std::max(1, std::stoi(value));
    else if (key == "RecheckStrongRatioQ8") m_recheckStrongRatioQ8 = std::max(1, std::stoi(value));
    else if (key == "RecheckVeryStrongRatioQ8") m_recheckVeryStrongRatioQ8 = std::max(1, std::stoi(value));
    else if (key == "RecheckOverlapEnabled") m_recheckOverlapEnabled = (value == "1" || value == "true");
    else if (key == "RecheckOverlapDistance") m_recheckOverlapDistance = std::max(0.1f, std::stof(value));
    else if (key == "RecheckOverlapPeakThreshold") m_recheckOverlapPeakThreshold = std::max(1, std::stoi(value));
    else if (key == "RecheckOverlapTouchSignalThreshold") m_recheckOverlapTouchSignalThreshold = std::max(1, std::stoi(value));
    else if (key == "RecheckOverlapTouchSignalThresholdWp") m_recheckOverlapTouchSignalThresholdWp = std::max(1, std::stoi(value));
    else if (key == "PressureMapMode") m_pressureMapMode = 2; // hardcoded incell
    else if (key == "PressurePolyEnabled") m_pressurePolyEnabled = (value == "1" || value == "true");
    else if (key == "PressureMapSeg1Threshold") m_pressureMapSeg1Threshold = std::clamp(std::stoi(value), 1, 4095);
    else if (key == "PressureMapSeg2Threshold") m_pressureMapSeg2Threshold = std::clamp(std::stoi(value), 1, 4095);
    else if (key == "PressurePolySeg1C0") m_pressurePolySeg1[0] = std::stod(value);
    else if (key == "PressurePolySeg1C1") m_pressurePolySeg1[1] = std::stod(value);
    else if (key == "PressurePolySeg1C2") m_pressurePolySeg1[2] = std::stod(value);
    else if (key == "PressurePolySeg1C3") m_pressurePolySeg1[3] = std::stod(value);
    else if (key == "PressurePolySeg1C4") m_pressurePolySeg1[4] = std::stod(value);
    else if (key == "PressurePolySeg2C0") m_pressurePolySeg2[0] = std::stod(value);
    else if (key == "PressurePolySeg2C1") m_pressurePolySeg2[1] = std::stod(value);
    else if (key == "PressurePolySeg2C2") m_pressurePolySeg2[2] = std::stod(value);
    else if (key == "PressurePolySeg2C3") m_pressurePolySeg2[3] = std::stod(value);
    else if (key == "PressurePolySeg2C4") m_pressurePolySeg2[4] = std::stod(value);
    else if (key == "PressureMapGainPercent") m_pressureMapGainPercent = std::clamp(std::stoi(value), 1, 1000);
    else if (key == "PressureIirWeightQ7" || key == "PressureIirWeightQ8") m_pressureIirWeightQ7 = std::clamp(std::stoi(value), 1, 127);
    else if (key == "PressureTailFrames") m_pressureTailFrames = std::clamp(std::stoi(value), 0, 30);
    else if (key == "PressureTailMin") m_pressureTailMin = std::clamp(std::stoi(value), 0, 256);
    else if (key == "PressureTailDecay") m_pressureTailDecay = std::clamp(std::stoi(value), 1, 1024);
    else if (key == "PressureEdgeSuppressEnabled") m_pressureEdgeSuppressEnabled = (value == "1" || value == "true");
    else if (key == "PressureEdgeMarginCells") m_pressureEdgeMarginCells = std::clamp(std::stoi(value), 0, 12);
    else if (key == "PressureEdgeSignalThreshold") m_pressureEdgeSignalThreshold = std::max(1, std::stoi(value));
    else if (key == "PressureEdgeSignalReleaseThreshold") m_pressureEdgeSignalReleaseThreshold = std::max(1, std::stoi(value));
    else if (key == "PressureSignalSuppressEnabled") m_pressureSignalSuppressEnabled = (value == "1" || value == "true");
    else if (key == "PressureSignalSuppressEnterTh") m_pressureSignalSuppressEnterTh = std::max(1, std::stoi(value));
    else if (key == "PressureSignalSuppressExitTh") m_pressureSignalSuppressExitTh = std::max(1, std::stoi(value));
    else if (key == "FakePressureDecreaseStartTh") m_fakePressureDecreaseStartTh = std::max(1, std::stoi(value));
    else if (key == "FakePressureDecreaseLevel2Th") m_fakePressureDecreaseLevel2Th = std::max(1, std::stoi(value));
    else if (key == "FakePressureDecreaseLevel3Th") m_fakePressureDecreaseLevel3Th = std::max(1, std::stoi(value));
    else if (key == "PressurePostRespectFreqShift") m_pressurePostRespectFreqShift = (value == "1" || value == "true");
    else if (key == "PressurePostFreqShiftDebounceMs") m_pressurePostFreqShiftDebounceMs = std::clamp(std::stoi(value), 1, 5000);
    else if (key == "PressurePostFreqShiftDebounceMsWp") m_pressurePostFreqShiftDebounceMsWp = std::clamp(std::stoi(value), 1, 5000);
    else if (key == "ButtonStateEnabled") m_buttonStateEnabled = (value == "1" || value == "true");
    else if (key == "ButtonUseMasterMeta") m_buttonUseMasterMeta = (value == "1" || value == "true");
    else if (key == "ButtonUseSlaveWord") m_buttonUseSlaveWord = (value == "1" || value == "true");
    else if (key == "ButtonSlaveWordIndex") m_buttonSlaveWordIndex = std::clamp(std::stoi(value), 0, 165);
    else if (key == "ButtonRawBitShift") m_buttonRawBitShift = std::clamp(std::stoi(value), 0, 31);
    else if (key == "ButtonRawMask") m_buttonRawMask = static_cast<uint32_t>(std::max(1, std::stoi(value)));
    else if (key == "ButtonReleaseHoldFrames") m_buttonReleaseHoldFrames = std::clamp(std::stoi(value), 0, 16);
    else if (key == "TiltEnabled") m_tiltEnabled = (value == "1" || value == "true");
    else if (key == "TiltKeepLastOnInvalid") m_tiltKeepLastOnInvalid = (value == "1" || value == "true");
    else if (key == "TiltUseSignalRatioLimit") m_tiltUseSignalRatioLimit = (value == "1" || value == "true");
    else if (key == "TiltRatioAverageWindow") m_tiltRatioAverageWindow = std::clamp(std::stoi(value), 1, 10);
    else if (key == "TiltRatioMinForOutput") m_tiltRatioMinForOutput = std::clamp(std::stoi(value), 0, 1000);
    else if (key == "TiltRatioRef") m_tiltRatioRef = std::clamp(std::stoi(value), 1, 1000);
    else if (key == "TiltRatioScaleMin") m_tiltRatioScaleMin = std::clamp(std::stof(value), 0.01f, 10.0f);
    else if (key == "TiltRatioScaleMax") m_tiltRatioScaleMax = std::clamp(std::stof(value), 0.01f, 10.0f);
    else if (key == "TiltCoordDiffLimit") m_tiltCoordDiffLimit = std::max(0.1f, std::stof(value));
    else if (key == "TiltLenRatioPointCount") m_tiltLenRatioPointCount = std::clamp(std::stoi(value), 0, 6);
    else if (key.rfind("TiltLenRatioThreshold", 0) == 0 && key.size() > 21) {
        const int idx = std::stoi(key.substr(21));
        if (idx >= 0 && idx < 6) m_tiltLenRatioThresholds[static_cast<size_t>(idx)] = static_cast<uint16_t>(std::clamp(std::stoi(value), 0, 2000));
    }
    else if (key.rfind("TiltLenScalePermille", 0) == 0 && key.size() > 20) {
        const int idx = std::stoi(key.substr(20));
        if (idx >= 0 && idx < 6) m_tiltLenScalePermille[static_cast<size_t>(idx)] = static_cast<uint16_t>(std::clamp(std::stoi(value), 0, 3000));
    }
    else if (key == "TiltDiffAverageWindow") m_tiltDiffAverageWindow = std::clamp(std::stoi(value), 1, 10);
    else if (key == "TiltOutAverageWindow") m_tiltOutAverageWindow = std::clamp(std::stoi(value), 1, 10);
    else if (key == "TiltCoordIirOldWeight") m_tiltCoordIirOldWeight = std::clamp(std::stof(value), 0.0f, 0.995f);
    else if (key == "TiltDegreePerCellX") m_tiltDegreePerCellX = std::max(0.1f, std::stof(value));
    else if (key == "TiltDegreePerCellY") m_tiltDegreePerCellY = std::max(0.1f, std::stof(value));
    else if (key == "TiltNormLenX") m_tiltNormLenX = std::max(0.1f, std::stof(value));
    else if (key == "TiltNormLenY") m_tiltNormLenY = std::max(0.1f, std::stof(value));
    else if (key == "TiltMaxDegree") m_tiltMaxDegree = std::clamp(std::stoi(value), 1, 89);
    else if (key == "TiltJitterThresholdDeg") m_tiltJitterThresholdDeg = std::clamp(std::stoi(value), 0, 10);
    else if (key == "TiltResetWhenNoPressure") m_tiltResetWhenNoPressure = (value == "1" || value == "true");
    else if (key == "TouchSuppressHoldFrames") m_touchSuppressHoldFrames = std::clamp(std::stoi(value), 0, 16);
}

void StylusProcessor::ResetStylusFrame(HeatmapFrame& frame) const {
    frame.stylus = StylusFrameData{};
    frame.stylus.dataType = ToDataType(m_dataType);
}

bool StylusProcessor::ValidateChecksum16(const uint8_t* bytes, size_t wordCount, uint16_t& outChecksum) const {
    uint32_t sum = 0;
    for (size_t i = 0; i < wordCount; ++i) sum += ReadU16Le(bytes + i * 2);
    outChecksum = static_cast<uint16_t>(sum & 0xFFFFu);
    return (outChecksum == 0) && (sum != 0);
}

bool StylusProcessor::LooksLikeBlockHeader(uint16_t w0, uint16_t w1) const {
    if (w0 == 0x00FF && w1 == 0x00FF) return true;
    const int tx = static_cast<int8_t>(w0 & 0xFFu);
    const int rx = static_cast<int8_t>(w1 & 0xFFu);
    return (tx >= -8 && tx <= (kRows + 8)) && (rx >= -8 && rx <= (kCols + 8));
}

bool StylusProcessor::ParseSlaveWords(const HeatmapFrame& frame, std::array<uint16_t, kSlaveWordCount>& outWords,
                                      uint8_t& outWordOffset, uint16_t& outChecksum, bool& outChecksumOk) const {
    if (frame.rawData.size() < (kMasterFrameBytes + kSlaveFrameBytes)) return false;
    const uint8_t* slave = frame.rawData.data() + kMasterFrameBytes;
    constexpr size_t kSlaveSize = kSlaveFrameBytes;
    const std::array<std::pair<uint8_t, size_t>, 3> layouts = {{{7, 166}, {5, 167}, {5, 166}}};

    SlaveLayoutCandidate best{};
    int bestScore = std::numeric_limits<int>::min();
    for (const auto& [offset, checksumWords] : layouts) {
        if (offset + kSlaveWordCount * 2 > kSlaveSize) continue;
        SlaveLayoutCandidate c{};
        c.wordOffset = offset;
        c.usable = true;
        if (offset + checksumWords * 2 <= kSlaveSize) c.checksumOk = ValidateChecksum16(slave + offset, checksumWords, c.checksum16);
        const uint16_t b0 = ReadU16Le(slave + offset + 0);
        const uint16_t b1 = ReadU16Le(slave + offset + 2);
        const uint16_t b2 = ReadU16Le(slave + offset + kStylusBlockWords * 2);
        const uint16_t b3 = ReadU16Le(slave + offset + kStylusBlockWords * 2 + 2);
        if (LooksLikeBlockHeader(b0, b1)) c.headerScore += 2;
        if (LooksLikeBlockHeader(b2, b3)) c.headerScore += 2;
        if (c.checksumOk) c.headerScore += 3;
        if (offset == 7) c.headerScore += 1;
        if (m_enableSlaveChecksum && !c.checksumOk) continue;
        if (c.headerScore > bestScore) {
            bestScore = c.headerScore;
            best = c;
        }
    }

    if (!best.usable) return false;
    outWordOffset = best.wordOffset;
    outChecksum = best.checksum16;
    outChecksumOk = best.checksumOk;
    for (size_t i = 0; i < kSlaveWordCount; ++i) outWords[i] = ReadU16Le(slave + best.wordOffset + i * 2);
    return true;
}

bool StylusProcessor::UnpackBlockToMatrix(const uint16_t* block, int16_t outMatrix[kRows][kCols]) const {
    std::memset(outMatrix, 0, sizeof(int16_t) * static_cast<size_t>(kRows) * static_cast<size_t>(kCols));
    if (block == nullptr) return false;
    if (block[0] == 0x00FF && block[1] == 0x00FF) return false;
    const int anchorTx = static_cast<int8_t>(block[0] & 0xFFu);
    const int anchorRx = static_cast<int8_t>(block[1] & 0xFFu);
    const int txStart = anchorTx - 4;
    const int rxStart = anchorRx - 2;

    bool mappedAny = false;
    for (int row = 0; row < 9; ++row) {
        for (int col = 0; col < 9; ++col) {
            const int tx = txStart + row;
            const int rx = rxStart + col;
            if (tx < 0 || tx >= kRows || rx < 0 || rx >= kCols) continue;
            const uint16_t sample = block[2 + row * 9 + col];
            const int linear = tx * kCols + rx;
            const int flipped = (kRows * kCols - 1) - linear;
            outMatrix[flipped / kCols][flipped % kCols] = static_cast<int16_t>(sample);
            mappedAny = true;
        }
    }
    return mappedAny;
}

StylusProcessor::PeakCentroidResult StylusProcessor::FindPeakCentroid(const int16_t matrix[kRows][kCols]) const {
    PeakCentroidResult out{};
    int bestDelta = std::numeric_limits<int>::min();
    int bestRow = 0;
    int bestCol = 0;
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            const int delta = static_cast<int>(matrix[r][c]) - kBaseline;
            if (delta > bestDelta) { bestDelta = delta; bestRow = r; bestCol = c; }
        }
    }
    if (bestDelta < m_peakDeltaThreshold) return out;
    double sumW = 0.0, sumX = 0.0, sumY = 0.0;
    const int radius = std::max(0, m_centroidRadius);
    for (int r = std::max(0, bestRow - radius); r <= std::min(kRows - 1, bestRow + radius); ++r) {
        for (int c = std::max(0, bestCol - radius); c <= std::min(kCols - 1, bestCol + radius); ++c) {
            const int delta = static_cast<int>(matrix[r][c]) - kBaseline;
            if (delta <= 0) continue;
            const double w = static_cast<double>(delta);
            sumW += w;
            sumX += static_cast<double>(c) * w;
            sumY += static_cast<double>(r) * w;
        }
    }
    out.valid = true;
    out.peakRow = bestRow;
    out.peakCol = bestCol;
    out.peakDelta = bestDelta;
    out.x = (sumW > 0.0) ? static_cast<float>(sumX / sumW) : static_cast<float>(bestCol);
    out.y = (sumW > 0.0) ? static_cast<float>(sumY / sumW) : static_cast<float>(bestRow);
    return out;
}

uint16_t StylusProcessor::FindMaxPeakDelta(const int16_t matrix[kRows][kCols]) const {
    int bestDelta = std::numeric_limits<int>::min();
    for (int r = 0; r < kRows; ++r)
        for (int c = 0; c < kCols; ++c)
            bestDelta = std::max(bestDelta, static_cast<int>(matrix[r][c]) - kBaseline);
    return static_cast<uint16_t>(std::clamp(bestDelta, 0, 0xFFFF));
}

StylusProcessor::AsaMode StylusProcessor::ResolveAsaMode(const HeatmapFrame& frame) const {
    if (m_forcedAsaMode == 1) return AsaMode::HPP2;
    if (m_forcedAsaMode == 2) return AsaMode::HPP3;
    if (frame.stylus.tx1BlockValid && frame.stylus.tx2BlockValid) return AsaMode::HPP3;
    if (frame.stylus.tx1BlockValid || frame.stylus.tx2BlockValid) return AsaMode::HPP2;
    return AsaMode::None;
}

bool StylusProcessor::EvaluateValidJudgment(const HeatmapFrame& frame) const {
    const uint32_t status = frame.stylus.status;
    const bool p1 = (status & 0x1u) != 0;
    const bool p2 = (status & 0x2u) != 0;
    if ((p1 && p2) || (!p1 && !p2)) return false;
    if ((status & (0x4u | 0x8u | 0x10u)) == 0) return false;
    if (((status & 0x4u) == 0) && !frame.stylus.tx1BlockValid) return false;
    return true;
}

bool StylusProcessor::ProcessHpp2Branch(HeatmapFrame& frame) const {
    (void)frame;
    return frame.stylus.tx1BlockValid || frame.stylus.tx2BlockValid;
}

bool StylusProcessor::ProcessHpp3Branch(HeatmapFrame& frame) const {
    const uint8_t type = ToDataType(m_dataType);
    if ((type == 0 || type == 1 || type == 3) && !frame.stylus.tx1BlockValid) return false;
    if (type == 2 && !frame.stylus.tx1BlockValid && !frame.stylus.tx2BlockValid) return false;
    frame.stylus.noPressInkActive = (frame.stylus.pressure == 0) && ((frame.stylus.status & 0x10u) != 0);
    return true;
}

bool StylusProcessor::DispatchHppDataProcess(HeatmapFrame& frame) const {
    const AsaMode mode = static_cast<AsaMode>(frame.stylus.asaMode);
    if (mode == AsaMode::HPP2) return ProcessHpp2Branch(frame);
    if (mode == AsaMode::HPP3) return ProcessHpp3Branch(frame);
    return false;
}

bool StylusProcessor::ApplyHpp3NoisePostProcess(HeatmapFrame& frame) {
    frame.stylus.hpp3NoiseInvalid = false;
    frame.stylus.hpp3NoiseDebounce = false;
    frame.stylus.hpp3Dim1SignalValid = frame.stylus.tx1BlockValid;
    frame.stylus.hpp3Dim2SignalValid = frame.stylus.tx2BlockValid;
    frame.stylus.hpp3RatioWarnCountX = 0;
    frame.stylus.hpp3RatioWarnCountY = 0;
    frame.stylus.hpp3SignalAvgX = m_pressSigAvgDim1;
    frame.stylus.hpp3SignalAvgY = m_pressSigAvgDim2;
    frame.stylus.hpp3SignalSampleCount = static_cast<uint8_t>(std::clamp(m_pressCnt, 0, 255));

    if (!m_hpp3NoisePostEnabled) {
        return false;
    }
    if (static_cast<AsaMode>(frame.stylus.asaMode) != AsaMode::HPP3) {
        return false;
    }

    const bool inRange = (frame.stylus.status & 0x10u) != 0;
    const bool prevStylusDataActive = (m_prevStatus & 0x6u) != 0;
    const int ratioFactor = std::max(2, m_hpp3SignalRatioFactor);
    const int dropFactor = std::max(2, m_hpp3SignalDropFactor);
    const int sigX = std::max(1, static_cast<int>(frame.stylus.signalX));
    const int sigY = std::max(1, static_cast<int>(frame.stylus.signalY));
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    m_hpp3Dim1SignalValid = frame.stylus.tx1BlockValid;
    m_hpp3Dim2SignalValid = frame.stylus.tx2BlockValid;
    m_hpp3RatioWarnCountX = 0;
    m_hpp3RatioWarnCountY = 0;
    bool invalid = false;
    if (inRange && frame.stylus.point.valid) {
        if ((sigX > sigY * ratioFactor) || (sigY > sigX * ratioFactor)) {
            invalid = true;
            m_hpp3Dim1SignalValid = false;
            m_hpp3Dim2SignalValid = false;
        }

        if (prevStylusDataActive && m_prevValidPoint) {
            if ((sigX * dropFactor < static_cast<int>(m_prevValidSignalX)) ||
                (sigY * dropFactor < static_cast<int>(m_prevValidSignalY))) {
                invalid = true;
                m_hpp3Dim1SignalValid = false;
                m_hpp3Dim2SignalValid = false;
            }

            const float dx = frame.stylus.point.x - m_prevValidX;
            const float dy = frame.stylus.point.y - m_prevValidY;
            const float jumpSq = dx * dx + dy * dy;
            if (jumpSq > (m_hpp3CoorJumpThreshold * m_hpp3CoorJumpThreshold)) {
                invalid = true;
                m_hpp3Dim1SignalValid = false;
                m_hpp3Dim2SignalValid = false;
            }
        }

        // Mirrors HPP3_NoisePostProcess: mild ratio imbalance (3:2) bumps both counters.
        if ((sigX > ((sigY * 3) / 2)) || (sigY > ((sigX * 3) / 2))) {
            m_hpp3RatioWarnCountX = static_cast<uint8_t>(std::min(255, static_cast<int>(m_hpp3RatioWarnCountX) + 1));
            m_hpp3RatioWarnCountY = static_cast<uint8_t>(std::min(255, static_cast<int>(m_hpp3RatioWarnCountY) + 1));
        }

        if (prevStylusDataActive && m_hpp3Dim1SignalValid && m_hpp3Dim2SignalValid) {
            m_lastValidOutputMs = nowMs;
        }
    }

    frame.stylus.hpp3Dim1SignalValid = m_hpp3Dim1SignalValid;
    frame.stylus.hpp3Dim2SignalValid = m_hpp3Dim2SignalValid;
    frame.stylus.hpp3RatioWarnCountX = m_hpp3RatioWarnCountX;
    frame.stylus.hpp3RatioWarnCountY = m_hpp3RatioWarnCountY;
    UpdateHpp3SignalLevel(frame, m_hpp3Dim1SignalValid && m_hpp3Dim2SignalValid);
    frame.stylus.hpp3SignalAvgX = m_pressSigAvgDim1;
    frame.stylus.hpp3SignalAvgY = m_pressSigAvgDim2;
    frame.stylus.hpp3SignalSampleCount = static_cast<uint8_t>(std::clamp(m_pressCnt, 0, 255));

    if (invalid) {
        frame.stylus.hpp3NoiseInvalid = true;
        frame.stylus.status &= ~0x10u;
        if (m_lastValidOutputMs > 0 && nowMs > m_lastValidOutputMs &&
            (nowMs - m_lastValidOutputMs) < static_cast<uint64_t>(std::max(1, m_hpp3NoiseDebounceMs))) {
            frame.stylus.hpp3NoiseDebounce = true;
        }
        return true;
    }

    if (inRange && frame.stylus.point.valid && m_hpp3Dim1SignalValid && m_hpp3Dim2SignalValid) {
        m_prevValidSignalX = frame.stylus.signalX;
        m_prevValidSignalY = frame.stylus.signalY;
        m_prevValidX = frame.stylus.point.x;
        m_prevValidY = frame.stylus.point.y;
        m_prevValidPoint = true;
    } else if (!inRange) {
        m_prevValidPoint = false;
    }

    return false;
}

void StylusProcessor::UpdateHpp3SignalLevel(const HeatmapFrame& frame, bool signalValid) {
    const bool inRange = (frame.stylus.status & 0x10u) != 0;
    const bool validForStats = inRange && signalValid && m_hpp3Dim1SignalValid && m_hpp3Dim2SignalValid &&
                               (m_hpp3RatioWarnCountX == 0) && (m_hpp3RatioWarnCountY == 0);

    if (!validForStats) {
        if (!inRange) {
            m_pressSigSumDim1 = 0;
            m_pressSigSumDim2 = 0;
            m_pressCnt = 0;
        }
    } else if (m_pressCnt < 10) {
        m_pressSigSumDim1 += frame.stylus.signalX;
        m_pressSigSumDim2 += frame.stylus.signalY;
        m_pressCnt += 1;
    } else if (m_pressCnt > 0) {
        m_pressSigSumDim1 = static_cast<uint64_t>(frame.stylus.signalX) +
                            (m_pressSigSumDim1 - (m_pressSigSumDim1 / static_cast<uint64_t>(m_pressCnt)));
        m_pressSigSumDim2 = static_cast<uint64_t>(frame.stylus.signalY) +
                            (m_pressSigSumDim2 - (m_pressSigSumDim2 / static_cast<uint64_t>(m_pressCnt)));
    }

    if (m_pressCnt > 0) {
        m_pressSigAvgDim1 = static_cast<uint16_t>(std::clamp<uint64_t>(m_pressSigSumDim1 / static_cast<uint64_t>(m_pressCnt), 0, 0xFFFF));
        m_pressSigAvgDim2 = static_cast<uint16_t>(std::clamp<uint64_t>(m_pressSigSumDim2 / static_cast<uint64_t>(m_pressCnt), 0, 0xFFFF));
    } else {
        m_pressSigAvgDim1 = 0;
        m_pressSigAvgDim2 = 0;
    }
}

uint16_t StylusProcessor::ComputeRecheckSignalThreshold(const HeatmapFrame& frame) const {
    int threshold = (m_noiseLevel > 2) ? m_recheckSignalThreshNoisy : m_recheckSignalThreshBase;
    const int signalX = std::max(1, static_cast<int>(frame.stylus.signalX));
    const int signalY = std::max(1, static_cast<int>(frame.stylus.signalY));
    const int peak = static_cast<int>(frame.stylus.maxRawPeak);
    const int ratioXQ8 = (peak << 8) / signalX;
    const int ratioYQ8 = (peak << 8) / signalY;
    if (peak > m_recheckStrongPeakThreshold && ratioXQ8 > m_recheckStrongRatioQ8 && ratioYQ8 > m_recheckStrongRatioQ8) {
        threshold = m_recheckSignalThreshStrong;
    }
    if (ratioXQ8 > m_recheckVeryStrongRatioQ8 && ratioYQ8 > m_recheckVeryStrongRatioQ8) {
        threshold = m_recheckSignalThreshVeryStrong;
    }
    return static_cast<uint16_t>(std::clamp(threshold, 0, 0xFFFF));
}

bool StylusProcessor::CheckStylusTouchOverlap(const HeatmapFrame& frame) const {
    if (!frame.stylus.point.valid || frame.contacts.empty()) return false;
    const float radiusSq = m_recheckOverlapDistance * m_recheckOverlapDistance;
    const int touchTh = m_windowsPadMode ? m_recheckOverlapTouchSignalThresholdWp : m_recheckOverlapTouchSignalThreshold;
    for (const auto& t : frame.contacts) {
        const float dx = t.x - frame.stylus.point.x;
        const float dy = t.y - frame.stylus.point.y;
        if (dx * dx + dy * dy > radiusSq) continue;
        if (static_cast<int>(frame.stylus.maxRawPeak) > m_recheckOverlapPeakThreshold && t.signalSum > touchTh) return true;
    }
    return false;
}

bool StylusProcessor::EvaluateTouchNullLike(uint32_t status) const {
    if (((status & 0x1u) == 0) || ((status & 0x4u) != 0)) {
        if (!m_hpp3TouchEnableFeature && ((status & 0x2u) != 0) && ((status & 0x4u) == 0) && ((status & 0x8u) == 0)) return true;
        return false;
    }
    return true;
}

void StylusProcessor::SolveStylusReportCoordinates(HeatmapFrame& frame) const {
    if (!frame.stylus.point.valid) {
        frame.stylus.point.reportX = 0xFFFFu;
        frame.stylus.point.reportY = 0xFFFFu;
        return;
    }

    const float xNorm = std::clamp(frame.stylus.point.x / static_cast<float>(std::max(1, kCols - 1)), 0.0f, 1.0f);
    const float yNorm = std::clamp(frame.stylus.point.y / static_cast<float>(std::max(1, kRows - 1)), 0.0f, 1.0f);
    frame.stylus.point.reportX = static_cast<uint16_t>(std::clamp(
        static_cast<int>(std::lround(xNorm * static_cast<float>(std::max(1, m_reportXMax)))), 0, 0xFFFF));
    frame.stylus.point.reportY = static_cast<uint16_t>(std::clamp(
        static_cast<int>(std::lround(yNorm * static_cast<float>(std::max(1, m_reportYMax)))), 0, 0xFFFF));
}

bool StylusProcessor::TryExtractMasterStylusMeta(const HeatmapFrame& frame, MasterStylusMeta& outMeta) {
    outMeta = MasterStylusMeta{};
    m_masterMetaDetectedBaseWord = 0xFF;
    if (!m_masterMetaEnabled) {
        return false;
    }
    constexpr size_t kMasterSuffixOffset = 4807;
    constexpr int kMasterSuffixWords = 128;
    constexpr int kStructWords = 16; // use +8/+9/+10/+12/+13/+14/+15 relative layout
    if (frame.rawData.size() < kMasterFrameBytes || (kMasterSuffixOffset + kMasterSuffixWords * 2) > frame.rawData.size()) {
        return false;
    }

    std::array<uint16_t, kMasterSuffixWords> words{};
    const uint8_t* suffix = frame.rawData.data() + kMasterSuffixOffset;
    for (int i = 0; i < kMasterSuffixWords; ++i) {
        words[static_cast<size_t>(i)] = ReadU16Le(suffix + static_cast<size_t>(i) * 2);
    }

    const auto read32 = [&words](int wordIdx) -> uint32_t {
        return static_cast<uint32_t>(words[static_cast<size_t>(wordIdx)]) |
               (static_cast<uint32_t>(words[static_cast<size_t>(wordIdx + 1)]) << 16);
    };

    const auto scoreCandidate = [&](int baseWord) -> int {
        const uint16_t tx1Freq = words[static_cast<size_t>(baseWord + 8)];
        const uint16_t tx2Freq = words[static_cast<size_t>(baseWord + 9)];
        const uint16_t pressure = words[static_cast<size_t>(baseWord + 10)];
        const uint32_t button = read32(baseWord + 12);
        const uint32_t status = read32(baseWord + 14);

        int score = 0;
        if (pressure <= 0x0FFFu) score += 1;
        if ((status & 0x3u) != 0u) score += 2;
        if ((status & 0x10u) != 0u) score += 1;
        if ((status & ~0x7Fu) == 0u) score += 1;
        if (tx1Freq == m_freqA || tx1Freq == m_freqB) score += 1;
        if (tx2Freq == m_freqA || tx2Freq == m_freqB) score += 1;
        if ((button & ~0xFFu) == 0u) score += 1;
        return score;
    };

    int baseWord = -1;
    if (m_masterMetaBaseWord >= 0 && m_masterMetaBaseWord <= (kMasterSuffixWords - kStructWords)) {
        baseWord = m_masterMetaBaseWord;
    } else if (m_masterMetaAutoDetect) {
        int bestScore = std::numeric_limits<int>::min();
        int bestBase = -1;
        for (int i = 0; i <= (kMasterSuffixWords - kStructWords); ++i) {
            const int score = scoreCandidate(i);
            if (score > bestScore) {
                bestScore = score;
                bestBase = i;
            }
        }
        if (bestBase >= 0 && bestScore >= 4) {
            baseWord = bestBase;
        }
    }

    if (baseWord < 0 || baseWord > (kMasterSuffixWords - kStructWords)) {
        return false;
    }

    outMeta.valid = true;
    outMeta.baseWord = static_cast<uint8_t>(baseWord);
    outMeta.tx1Freq = words[static_cast<size_t>(baseWord + 8)];
    outMeta.tx2Freq = words[static_cast<size_t>(baseWord + 9)];
    outMeta.pressure = words[static_cast<size_t>(baseWord + 10)];
    outMeta.button = read32(baseWord + 12);
    outMeta.status = read32(baseWord + 14);
    m_masterMetaDetectedBaseWord = outMeta.baseWord;
    return true;
}

uint32_t StylusProcessor::ResolveRawButtonBits(const HeatmapFrame& frame) const {
    if (m_buttonUseSlaveWord && frame.stylus.slaveValid) {
        const int idx = std::clamp(m_buttonSlaveWordIndex, 0, static_cast<int>(kSlaveWordCount) - 1);
        return frame.stylus.slaveWords[static_cast<size_t>(idx)];
    }
    return 0;
}

uint32_t StylusProcessor::UpdateButtonState(uint32_t rawButtonBits, bool stylusActive) {
    if (!stylusActive) {
        m_buttonReleaseCounter = 0;
        return 0;
    }

    const int shift = std::clamp(m_buttonRawBitShift, 0, 31);
    const uint32_t masked = (rawButtonBits >> shift) & std::max<uint32_t>(1u, m_buttonRawMask);
    const uint32_t pressed = (masked != 0u) ? 1u : 0u;
    if (!m_buttonStateEnabled) {
        return pressed;
    }

    if (pressed != 0u) {
        m_buttonReleaseCounter = std::max(0, m_buttonReleaseHoldFrames);
        return 1u;
    }
    if (m_buttonReleaseCounter > 0) {
        m_buttonReleaseCounter -= 1;
        return 1u;
    }
    return 0u;
}

void StylusProcessor::UpdatePressureHistory(uint16_t rawPressure, bool active) {
    if (!active || rawPressure == 0) {
        m_btPressCnt = 0;
        m_btPressBuf = {{0, 0, 0, 0}};
        return;
    }

    m_btPressCnt = static_cast<uint8_t>(std::min(255, static_cast<int>(m_btPressCnt) + 1));
    for (int i = static_cast<int>(m_btPressBuf.size()) - 1; i > 0; --i) {
        m_btPressBuf[static_cast<size_t>(i)] = m_btPressBuf[static_cast<size_t>(i - 1)];
    }
    m_btPressBuf[0] = rawPressure;
}

uint16_t StylusProcessor::GetPressureInMapOrder(uint16_t rawPressure) const {
    if (rawPressure == 0) {
        return 0;
    }
    // Fixed for current target hardware.
    constexpr int kPressureMapModeForced = 2; // incell
    const uint8_t mapCnt = (m_btPressCnt > 0) ? static_cast<uint8_t>(m_btPressCnt - 1) : 0;
    if (kPressureMapModeForced == 1) {
        if (mapCnt < 6 && m_btPressBuf[0] != 0) {
            const uint8_t idx = m_btPressMapOncell[static_cast<size_t>(mapCnt)];
            if (idx < m_btPressBuf.size()) {
                return m_btPressBuf[idx];
            }
        }
        return rawPressure;
    }
    if (kPressureMapModeForced == 2) {
        if (mapCnt < 4 && m_btPressBuf[0] != 0) {
            const uint8_t idx = m_btPressMapIncell[static_cast<size_t>(mapCnt)];
            if (idx < m_btPressBuf.size()) {
                return m_btPressBuf[idx];
            }
        }
        return rawPressure;
    }
    return rawPressure;
}

uint16_t StylusProcessor::MapPressureHpp3(uint16_t pressInMapOrder) const {
    if (pressInMapOrder == 0x0FFF) {
        return 0x0FFF;
    }

    const int x = static_cast<int>(pressInMapOrder);
    const int th1 = std::clamp(m_pressureMapSeg1Threshold, 1, 4095);
    const int th2 = std::clamp(m_pressureMapSeg2Threshold, th1, 4095);

    if (x <= th1) {
        // Mirrors TSACore fallback: 0/1 only under first segment.
        return static_cast<uint16_t>((x > 1) ? 1 : x);
    }

    if (!m_pressurePolyEnabled) {
        return static_cast<uint16_t>(std::clamp(x, 0, 0x0FFF));
    }

    const auto evalPoly = [x](const std::array<double, 5>& c) -> int {
        const double xd = static_cast<double>(x);
        const double y = c[0] + c[1] * xd + c[2] * xd * xd + c[3] * xd * xd * xd + c[4] * xd * xd * xd * xd;
        // TSACore decompilation shows cast-to-int truncation instead of rounding.
        return static_cast<int>(y);
    };

    int mapped = 0;
    if (x <= th2) {
        mapped = evalPoly(m_pressurePolySeg1);
    } else {
        mapped = evalPoly(m_pressurePolySeg2);
    }
    return static_cast<uint16_t>(std::clamp(mapped, 0, 0x0FFF));
}

bool StylusProcessor::IsPressurePostAllowed(const HeatmapFrame& frame) {
    if (!m_pressurePostRespectFreqShift) {
        return true;
    }

    const bool inFreqShift = (frame.stylus.status & 0x40u) != 0;
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    if (inFreqShift) {
        if (!m_pressurePostFreqShiftActive) {
            m_pressurePostFreqShiftStartMs = nowMs;
            m_pressurePostFreqShiftActive = true;
        }
        return false;
    }

    if (m_pressurePostFreqShiftActive) {
        const int debounceMs = m_windowsPadMode
                                   ? std::max(1, m_pressurePostFreqShiftDebounceMsWp)
                                   : std::max(1, m_pressurePostFreqShiftDebounceMs);
        if (nowMs <= m_pressurePostFreqShiftStartMs ||
            (nowMs - m_pressurePostFreqShiftStartMs) <= static_cast<uint64_t>(debounceMs)) {
            return false;
        }
        m_pressurePostFreqShiftActive = false;
    }
    return true;
}

void StylusProcessor::ApplyPressureSignalSuppression(const HeatmapFrame& frame,
                                                     const PeakCentroidResult& tx1,
                                                     const PeakCentroidResult& tx2,
                                                     uint16_t& inOutPressure) {
    if (!m_pressureSignalSuppressEnabled) {
        return;
    }

    if (inOutPressure == 0) {
        m_pressureSignalSuppressActive = false;
    }

    const bool tx1Valid = tx1.valid;
    const bool tx2Valid = tx2.valid;
    const int signalX = static_cast<int>(frame.stylus.signalX);

    if (!m_pressureSignalSuppressActive &&
        signalX < std::max(1, m_pressureSignalSuppressEnterTh) &&
        !tx1Valid && !tx2Valid) {
        m_pressureSignalSuppressActive = true;
        inOutPressure = 0;
    } else if (m_pressureSignalSuppressActive) {
        if (signalX > std::max(1, m_pressureSignalSuppressExitTh)) {
            m_pressureSignalSuppressActive = false;
        } else {
            inOutPressure = 0;
        }
    }
}

void StylusProcessor::UpdateTiltSignalRatio(uint16_t signalX, uint16_t signalY) {
    int ratio = 500;
    const int sx = std::max(1, static_cast<int>(signalX));
    const int sy = std::max(0, static_cast<int>(signalY));
    if (sy < sx * 5) {
        ratio = (sy * 100) / sx;
    }

    m_tiltRatioBufCount = std::min(10, m_tiltRatioBufCount + 1);
    for (int i = 9; i > 0; --i) {
        m_tiltSignalRatioBuf[static_cast<size_t>(i)] = m_tiltSignalRatioBuf[static_cast<size_t>(i - 1)];
    }
    m_tiltSignalRatioBuf[0] = static_cast<uint16_t>(std::clamp(ratio, 0, 500));
}

float StylusProcessor::ComputeTiltLenLimit() const {
    const int count = std::max(1, std::min(m_tiltRatioBufCount, std::clamp(m_tiltRatioAverageWindow, 1, 10)));
    int ratioSum = 0;
    for (int i = 0; i < count; ++i) {
        ratioSum += static_cast<int>(m_tiltSignalRatioBuf[static_cast<size_t>(i)]);
    }
    const float ratioAvg = static_cast<float>(ratioSum) / static_cast<float>(count);

    if (m_tiltUseSignalRatioLimit && ratioAvg <= static_cast<float>(m_tiltRatioMinForOutput)) {
        return 0.0f;
    }

    const float baseLimit = std::max(0.1f, m_tiltCoordDiffLimit);
    const int ptCount = std::clamp(m_tiltLenRatioPointCount, 0, 6);
    if (ptCount > 1) {
        const auto& th = m_tiltLenRatioThresholds;
        const auto& sc = m_tiltLenScalePermille;
        if (ratioAvg <= static_cast<float>(th[0])) {
            return 0.0f;
        }
        if (ratioAvg <= static_cast<float>(th[static_cast<size_t>(ptCount - 1)])) {
            for (int i = 0; i < (ptCount - 1); ++i) {
                const float x0 = static_cast<float>(th[static_cast<size_t>(i)]);
                const float x1 = static_cast<float>(th[static_cast<size_t>(i + 1)]);
                if (ratioAvg > x0 && ratioAvg <= x1 && x1 > x0) {
                    const float y0 = static_cast<float>(sc[static_cast<size_t>(i)]);
                    const float y1 = static_cast<float>(sc[static_cast<size_t>(i + 1)]);
                    const float k = (ratioAvg - x0) / (x1 - x0);
                    const float scalePermille = y0 + (y1 - y0) * k;
                    return std::max(0.0f, baseLimit * scalePermille / 1000.0f);
                }
            }
        }
    }

    const float ratioRef = static_cast<float>(std::max(1, m_tiltRatioRef));
    const float ratioScale = std::clamp(ratioAvg / ratioRef, m_tiltRatioScaleMin, m_tiltRatioScaleMax);
    return std::max(0.1f, baseLimit * ratioScale);
}

bool StylusProcessor::IsTiltInputValid(const HeatmapFrame& frame,
                                       const PeakCentroidResult& tx1,
                                       const PeakCentroidResult& tx2) const {
    const uint8_t type = ToDataType(m_dataType);
    if (type == 2) {
        return frame.stylus.tx1BlockValid && frame.stylus.tx2BlockValid;
    }
    return tx1.valid && tx2.valid;
}

void StylusProcessor::ResetTiltHistory() {
    m_tiltRatioBufCount = 0;
    m_tiltDiffBufCount = 0;
    m_tiltOutBufCount = 0;
    m_tiltSignalRatioBuf.fill(0);
    m_tiltDiffBufX.fill(0.0f);
    m_tiltDiffBufY.fill(0.0f);
    m_tiltOutBufX.fill(0);
    m_tiltOutBufY.fill(0);
    m_prevTiltDiffX = 0.0f;
    m_prevTiltDiffY = 0.0f;
    m_prevPreTiltX = 0;
    m_prevPreTiltY = 0;
    m_prevTiltX = 0;
    m_prevTiltY = 0;
    m_tiltHasHistory = false;
}

int StylusProcessor::ConvertCoordDiffToTilt(float coordDiff, bool dimY) const {
    const float normLen = std::max(0.1f, dimY ? m_tiltNormLenY : m_tiltNormLenX);
    const float legacyScale = std::max(0.1f, (dimY ? m_tiltDegreePerCellY : m_tiltDegreePerCellX) / 8.0f);
    const float scaledDiff = coordDiff * legacyScale;
    float degree = 0.0f;
    if (std::abs(scaledDiff) < normLen) {
        degree = std::asin(scaledDiff / normLen) * 57.2957795f;
    } else {
        degree = (scaledDiff < 0.0f) ? -90.0f : 90.0f;
    }
    const int maxTilt = std::clamp(m_tiltMaxDegree, 1, 89);
    return std::clamp(static_cast<int>(std::lround(degree)), -maxTilt, maxTilt);
}

void StylusProcessor::SolveStylusPressure(HeatmapFrame& frame, const PeakCentroidResult& tx1, const PeakCentroidResult& tx2) {
    uint16_t rawPressure = 0;
    if (frame.stylus.masterMetaValid) {
        rawPressure = static_cast<uint16_t>(std::clamp(static_cast<int>(frame.stylus.masterMetaPressure), 0, 0x0FFF));
    } else if (tx1.valid && tx2.valid) {
        rawPressure = static_cast<uint16_t>(std::clamp((tx1.peakDelta + tx2.peakDelta) / 2, 0, 0x0FFF));
    } else if (tx1.valid || tx2.valid) {
        const int v = tx1.valid ? tx1.peakDelta : tx2.peakDelta;
        rawPressure = static_cast<uint16_t>(std::clamp(v, 0, 0x0FFF));
    }

    UpdatePressureHistory(rawPressure, rawPressure > 0);
    const uint16_t pressInMapOrder = GetPressureInMapOrder(rawPressure);
    const uint16_t mappedByCurve = MapPressureHpp3(pressInMapOrder);

    int mapped = static_cast<int>(mappedByCurve) * std::clamp(m_pressureMapGainPercent, 1, 1000) / 100;
    mapped = std::clamp(mapped, 0, 0x0FFF);

    if (mapped > 0 && m_prevPressure > 0) {
        const int w = std::clamp(m_pressureIirWeightQ7, 1, 127);
        mapped = ((static_cast<int>(m_prevPressure) * (128 - w)) + mapped * w + 64) >> 7;
        mapped = std::clamp(mapped, 0, 0x0FFF);
    }

    if (mapped == 0 && m_prevPressure > 0 && m_pressureTailFrames > 0 && m_pressureTailCounter < m_pressureTailFrames) {
        const int decayed = std::max(std::clamp(m_pressureTailMin, 0, 0x0FFF),
                                     std::max(0, static_cast<int>(m_prevPressure) - std::max(1, m_pressureTailDecay)));
        mapped = std::clamp(decayed, 0, 0x0FFF);
        m_pressureTailCounter += 1;
    } else if (mapped > 0) {
        m_pressureTailCounter = 0;
    } else {
        m_pressureTailCounter = 0;
    }

    uint16_t mappedU16 = static_cast<uint16_t>(mapped);
    ApplyPressureSignalSuppression(frame, tx1, tx2, mappedU16);
    mapped = static_cast<int>(mappedU16);

    const bool allowPost = IsPressurePostAllowed(frame);
    if (!allowPost) {
        m_pressureEdgeSuppressState = false;
        m_fakePressureDecreaseAdded = false;
        m_fakePressureDecreaseAddNum = 0;
    }

    if (allowPost && mapped < 11 && m_prevPressure > 500) {
        if (!m_fakePressureDecreaseAdded && m_fakePressureDecreaseAddNum == 0) {
            if (m_prevPressure > m_fakePressureDecreaseLevel3Th) m_fakePressureDecreaseAddNum = 3;
            else if (m_prevPressure > m_fakePressureDecreaseLevel2Th) m_fakePressureDecreaseAddNum = 2;
            else if (m_prevPressure > m_fakePressureDecreaseStartTh) m_fakePressureDecreaseAddNum = 1;
            m_fakePressureDecreaseAdded = true;
        }
        if (m_fakePressureDecreaseAddNum > 0) {
            mapped = (m_fakePressureDecreaseAddNum * static_cast<int>(m_prevPressure)) / (m_fakePressureDecreaseAddNum + 1);
            m_fakePressureDecreaseAddNum -= 1;
        }
    }

    if (mapped == 0) {
        m_pressureEdgeSuppressState = false;
        m_fakePressureDecreaseAdded = false;
        m_fakePressureDecreaseAddNum = 0;
    } else if (allowPost && m_pressureEdgeSuppressEnabled) {
        const int edgeMargin = std::clamp(m_pressureEdgeMarginCells, 0, 12);
        const bool tx1Edge = tx1.valid &&
                             (tx1.peakRow < edgeMargin || tx1.peakRow >= (kRows - edgeMargin) ||
                              tx1.peakCol < edgeMargin || tx1.peakCol >= (kCols - edgeMargin));
        const bool tx2Edge = tx2.valid &&
                             (tx2.peakRow < edgeMargin || tx2.peakRow >= (kRows - edgeMargin) ||
                              tx2.peakCol < edgeMargin || tx2.peakCol >= (kCols - edgeMargin));
        const int edgeEnterTh = std::max(1, m_pressureEdgeSignalThreshold);
        const int edgeExitTh = std::max(edgeEnterTh, m_pressureEdgeSignalReleaseThreshold);
        const int sigX = static_cast<int>(frame.stylus.signalX);
        const int sigY = static_cast<int>(frame.stylus.signalY);

        if (!m_pressureEdgeSuppressState) {
            if (!tx1Edge || !tx2Edge) {
                if ((tx1Edge && sigX < edgeEnterTh) || (tx2Edge && sigY < edgeEnterTh)) {
                    m_pressureEdgeSuppressState = true;
                }
            } else {
                if (sigX < ((edgeEnterTh * 2) / 3) || sigY < ((edgeEnterTh * 2) / 3)) {
                    m_pressureEdgeSuppressState = true;
                }
            }
        }

        if (m_pressureEdgeSuppressState) {
            const bool release =
                (!tx1Edge || sigX > edgeExitTh) &&
                (!tx2Edge || sigY > edgeExitTh);
            if (release) {
                m_pressureEdgeSuppressState = false;
            }
        }

        if (m_pressureEdgeSuppressState) {
            mapped = 0;
        }
    }

    mapped = std::clamp(mapped, 0, 0x0FFF);
    frame.stylus.point.rawPressure = rawPressure;
    frame.stylus.point.mappedPressure = static_cast<uint16_t>(mapped);
    frame.stylus.point.pressure = static_cast<uint16_t>(mapped);
    frame.stylus.pressure = frame.stylus.point.pressure;
    m_prevPressure = frame.stylus.point.pressure;
}

void StylusProcessor::SolveStylusTilt(HeatmapFrame& frame, const PeakCentroidResult& tx1, const PeakCentroidResult& tx2) {
    frame.stylus.point.tiltValid = false;
    frame.stylus.point.preTiltX = 0;
    frame.stylus.point.preTiltY = 0;
    frame.stylus.point.tiltX = 0;
    frame.stylus.point.tiltY = 0;
    frame.stylus.point.tiltMagnitude = 0.0f;
    frame.stylus.point.tiltAzimuthDeg = 0.0f;

    if (!m_tiltEnabled) {
        m_prevTiltStatus = frame.stylus.status;
        m_prevTiltPressure = frame.stylus.point.pressure;
        return;
    }

    const bool prevStatusActive = ((m_prevTiltStatus & 0x2u) != 0) || ((m_prevTiltStatus & 0x4u) != 0);
    if (!prevStatusActive || (m_tiltResetWhenNoPressure && m_prevTiltPressure == 0)) {
        ResetTiltHistory();
    }

    auto keepLastTilt = [&]() {
        if (m_tiltKeepLastOnInvalid && m_tiltHasHistory) {
            frame.stylus.point.preTiltX = m_prevPreTiltX;
            frame.stylus.point.preTiltY = m_prevPreTiltY;
            frame.stylus.point.tiltX = m_prevTiltX;
            frame.stylus.point.tiltY = m_prevTiltY;
            frame.stylus.point.tiltMagnitude = std::sqrt(
                static_cast<float>(m_prevTiltX * m_prevTiltX + m_prevTiltY * m_prevTiltY));
            frame.stylus.point.tiltAzimuthDeg = std::atan2(
                static_cast<float>(m_prevTiltY), static_cast<float>(m_prevTiltX)) * 57.2957795f;
            if (frame.stylus.point.tiltAzimuthDeg < 0.0f) frame.stylus.point.tiltAzimuthDeg += 360.0f;
            frame.stylus.point.tiltValid = true;
        }
    };

    const bool txDualValid = tx1.valid && tx2.valid && IsTiltInputValid(frame, tx1, tx2);
    if (!txDualValid) {
        keepLastTilt();
        if (!frame.stylus.point.valid) {
            ResetTiltHistory();
        }
        m_prevTiltStatus = frame.stylus.status;
        m_prevTiltPressure = frame.stylus.point.pressure;
        return;
    }

    UpdateTiltSignalRatio(frame.stylus.signalX, frame.stylus.signalY);
    float diffX = tx2.x - tx1.x;
    float diffY = tx2.y - tx1.y;
    const float diffLimit = ComputeTiltLenLimit();
    if (diffLimit <= 0.0f) {
        keepLastTilt();
        m_prevTiltStatus = frame.stylus.status;
        m_prevTiltPressure = frame.stylus.point.pressure;
        return;
    }

    if (((std::abs(diffX) > diffLimit) || (std::abs(diffY) > diffLimit)) && (m_tiltDiffBufCount != 0)) {
        diffX = m_tiltDiffBufX[0];
        diffY = m_tiltDiffBufY[0];
    }

    if ((std::abs(diffX) > diffLimit) || (std::abs(diffY) > diffLimit)) {
        if (m_tiltDiffBufCount == 0) {
            diffX = std::clamp(diffX, -diffLimit, diffLimit);
            diffY = std::clamp(diffY, -diffLimit, diffLimit);
        } else {
            diffX = m_tiltDiffBufX[0];
            diffY = m_tiltDiffBufY[0];
        }
    }

    m_tiltDiffBufCount = std::min(10, m_tiltDiffBufCount + 1);
    for (int i = 9; i > 0; --i) {
        m_tiltDiffBufX[static_cast<size_t>(i)] = m_tiltDiffBufX[static_cast<size_t>(i - 1)];
        m_tiltDiffBufY[static_cast<size_t>(i)] = m_tiltDiffBufY[static_cast<size_t>(i - 1)];
    }
    m_tiltDiffBufX[0] = diffX;
    m_tiltDiffBufY[0] = diffY;
    {
        const int count = std::max(1, std::min(m_tiltDiffBufCount, std::clamp(m_tiltDiffAverageWindow, 1, 10)));
        float sumX = 0.0f;
        float sumY = 0.0f;
        for (int i = 0; i < count; ++i) {
            sumX += m_tiltDiffBufX[static_cast<size_t>(i)];
            sumY += m_tiltDiffBufY[static_cast<size_t>(i)];
        }
        diffX = sumX / static_cast<float>(count);
        diffY = sumY / static_cast<float>(count);
    }

    if (m_tiltHasHistory && m_tiltDiffAverageWindow <= 1) {
        const float oldW = std::clamp(m_tiltCoordIirOldWeight, 0.0f, 0.995f);
        diffX = m_prevTiltDiffX * oldW + diffX * (1.0f - oldW);
        diffY = m_prevTiltDiffY * oldW + diffY * (1.0f - oldW);
    }
    m_prevTiltDiffX = diffX;
    m_prevTiltDiffY = diffY;

    int preX = ConvertCoordDiffToTilt(diffX, false);
    int preY = ConvertCoordDiffToTilt(diffY, true);

    float mag = std::sqrt(diffX * diffX + diffY * diffY);
    if (mag < 1e-5f) mag = 1.0f;
    if (mag > diffLimit) {
        const float scale = diffLimit / mag;
        diffX *= scale;
        diffY *= scale;
        preX = ConvertCoordDiffToTilt(diffX, false);
        preY = ConvertCoordDiffToTilt(diffY, true);
    }

    m_tiltOutBufCount = std::min(10, m_tiltOutBufCount + 1);
    for (int i = 9; i > 0; --i) {
        m_tiltOutBufX[static_cast<size_t>(i)] = m_tiltOutBufX[static_cast<size_t>(i - 1)];
        m_tiltOutBufY[static_cast<size_t>(i)] = m_tiltOutBufY[static_cast<size_t>(i - 1)];
    }
    m_tiltOutBufX[0] = static_cast<int16_t>(preX);
    m_tiltOutBufY[0] = static_cast<int16_t>(preY);

    int outX = preX;
    int outY = preY;
    if (!(frame.stylus.point.pressure == 0 || m_prevTiltPressure == 0)) {
        const int count = std::max(1, std::min(m_tiltOutBufCount, std::clamp(m_tiltOutAverageWindow, 1, 10)));
        int sumX = 0;
        int sumY = 0;
        for (int i = 0; i < count; ++i) {
            sumX += static_cast<int>(m_tiltOutBufX[static_cast<size_t>(i)]);
            sumY += static_cast<int>(m_tiltOutBufY[static_cast<size_t>(i)]);
        }
        outX = sumX / count;
        outY = sumY / count;
    }

    if (m_tiltHasHistory) {
        const int jitTh = std::max(0, m_tiltJitterThresholdDeg);
        if (std::abs(outX - static_cast<int>(m_prevTiltX)) <= jitTh) outX = static_cast<int>(m_prevTiltX);
        else if (outX > static_cast<int>(m_prevTiltX)) outX -= 1;
        else if (outX < static_cast<int>(m_prevTiltX)) outX += 1;
        if (std::abs(outY - static_cast<int>(m_prevTiltY)) <= jitTh) outY = static_cast<int>(m_prevTiltY);
        else if (outY > static_cast<int>(m_prevTiltY)) outY -= 1;
        else if (outY < static_cast<int>(m_prevTiltY)) outY += 1;
    }

    frame.stylus.point.preTiltX = static_cast<int16_t>(preX);
    frame.stylus.point.preTiltY = static_cast<int16_t>(preY);
    frame.stylus.point.tiltX = static_cast<int16_t>(outX);
    frame.stylus.point.tiltY = static_cast<int16_t>(outY);
    frame.stylus.point.tiltMagnitude = std::sqrt(static_cast<float>(outX * outX + outY * outY));
    frame.stylus.point.tiltAzimuthDeg = std::atan2(static_cast<float>(outY), static_cast<float>(outX)) * 57.2957795f;
    if (frame.stylus.point.tiltAzimuthDeg < 0.0f) frame.stylus.point.tiltAzimuthDeg += 360.0f;
    frame.stylus.point.tiltValid = true;

    m_prevPreTiltX = frame.stylus.point.preTiltX;
    m_prevPreTiltY = frame.stylus.point.preTiltY;
    m_prevTiltX = frame.stylus.point.tiltX;
    m_prevTiltY = frame.stylus.point.tiltY;
    m_tiltHasHistory = true;
    m_prevTiltStatus = frame.stylus.status;
    m_prevTiltPressure = frame.stylus.point.pressure;
}

void StylusProcessor::SolveStylusPoint(HeatmapFrame& frame) {
    const PeakCentroidResult tx1 = FindPeakCentroid(frame.stylus.tx1Matrix);
    const PeakCentroidResult tx2 = FindPeakCentroid(frame.stylus.tx2Matrix);
    MasterStylusMeta masterMeta{};
    if (TryExtractMasterStylusMeta(frame, masterMeta)) {
        frame.stylus.masterMetaValid = true;
        frame.stylus.masterMetaBaseWord = masterMeta.baseWord;
        frame.stylus.masterMetaTx1Freq = masterMeta.tx1Freq;
        frame.stylus.masterMetaTx2Freq = masterMeta.tx2Freq;
        frame.stylus.masterMetaPressure = masterMeta.pressure;
        frame.stylus.masterMetaButton = masterMeta.button;
        frame.stylus.masterMetaStatus = masterMeta.status;
    } else {
        frame.stylus.masterMetaValid = false;
        frame.stylus.masterMetaBaseWord = 0xFF;
        frame.stylus.masterMetaTx1Freq = 0;
        frame.stylus.masterMetaTx2Freq = 0;
        frame.stylus.masterMetaPressure = 0;
        frame.stylus.masterMetaButton = 0;
        frame.stylus.masterMetaStatus = 0;
    }

    if (m_currentFreqIndex == 0) { frame.stylus.tx1Freq = m_freqA; frame.stylus.tx2Freq = m_freqB; }
    else { frame.stylus.tx1Freq = m_freqB; frame.stylus.tx2Freq = m_freqA; }
    frame.stylus.nextTx1Freq = frame.stylus.tx1Freq;
    frame.stylus.nextTx2Freq = frame.stylus.tx2Freq;
    frame.stylus.button = 0;
    frame.stylus.rawButton = 0;
    frame.stylus.buttonSource = 0;
    frame.stylus.dataType = ToDataType(m_dataType);
    frame.stylus.point.valid = false;
    frame.stylus.point.tx1X = tx1.x;
    frame.stylus.point.tx1Y = tx1.y;
    frame.stylus.point.tx2X = tx2.x;
    frame.stylus.point.tx2Y = tx2.y;
    frame.stylus.point.peakTx1 = static_cast<uint16_t>(std::clamp(tx1.peakDelta, 0, 0xFFFF));
    frame.stylus.point.peakTx2 = static_cast<uint16_t>(std::clamp(tx2.peakDelta, 0, 0xFFFF));
    if (tx1.valid && tx2.valid) {
        const float w1 = static_cast<float>(std::max(1, tx1.peakDelta));
        const float w2 = static_cast<float>(std::max(1, tx2.peakDelta));
        frame.stylus.point.valid = true;
        frame.stylus.point.x = (tx1.x * w1 + tx2.x * w2) / (w1 + w2);
        frame.stylus.point.y = (tx1.y * w1 + tx2.y * w2) / (w1 + w2);
        frame.stylus.point.confidence = std::clamp(static_cast<float>(std::max(tx1.peakDelta, tx2.peakDelta)) / m_confidenceScale, 0.0f, 1.0f);
    } else if (tx1.valid || tx2.valid) {
        const PeakCentroidResult& p = tx1.valid ? tx1 : tx2;
        frame.stylus.point.valid = true;
        frame.stylus.point.x = p.x;
        frame.stylus.point.y = p.y;
        frame.stylus.point.confidence = std::clamp(static_cast<float>(p.peakDelta) / m_confidenceScale, 0.0f, 1.0f);
    }

    SolveStylusReportCoordinates(frame);
    frame.stylus.signalX = frame.stylus.point.peakTx1;
    frame.stylus.signalY = frame.stylus.point.peakTx2;
    frame.stylus.maxRawPeak = std::max(FindMaxPeakDelta(frame.stylus.tx1Matrix), FindMaxPeakDelta(frame.stylus.tx2Matrix));

    const uint32_t protocolBit = (m_protocolType == 1) ? 0x1u : 0x2u;
    uint32_t status = 0;
    if (frame.stylus.point.valid) { status = protocolBit | 0x10u; m_unstableStreak = 0; }
    else if (frame.stylus.tx1BlockValid || frame.stylus.tx2BlockValid) { status = protocolBit | 0x4u; m_unstableStreak += 1; }
    else m_unstableStreak = 0;
    if (m_unstableStreak >= m_unstableStreakThreshold && status != 0) status |= 0x20u;
    const float diff = static_cast<float>(tx2.peakDelta - tx1.peakDelta);
    m_freqScore = std::clamp(m_freqScore * m_freqScoreDecay + diff, -30000.0f, 30000.0f);
    if (m_freqRequestRemaining == 0 && status != 0) {
        if (m_currentFreqIndex == 0 && m_freqScore > m_freqSwitchScoreThreshold) { m_pendingFreqIndex = 1; m_currentFreqIndex = 1; m_freqRequestRemaining = m_freqRequestHoldFrames; }
        else if (m_currentFreqIndex == 1 && m_freqScore < -m_freqSwitchScoreThreshold) { m_pendingFreqIndex = 0; m_currentFreqIndex = 0; m_freqRequestRemaining = m_freqRequestHoldFrames; }
    }
    if (m_freqRequestRemaining > 0) {
        status |= 0x40u;
        if (m_pendingFreqIndex == 0) { frame.stylus.nextTx1Freq = m_freqA; frame.stylus.nextTx2Freq = m_freqB; }
        else { frame.stylus.nextTx1Freq = m_freqB; frame.stylus.nextTx2Freq = m_freqA; }
        m_freqRequestRemaining -= 1;
    }
    frame.stylus.status = status;
    frame.stylus.asaMode = static_cast<uint8_t>(ResolveAsaMode(frame));

    uint32_t rawButtonBits = 0;
    if (m_buttonUseMasterMeta && frame.stylus.masterMetaValid) {
        rawButtonBits = frame.stylus.masterMetaButton;
        frame.stylus.buttonSource = 1;
    } else {
        rawButtonBits = ResolveRawButtonBits(frame);
        if (rawButtonBits != 0 && m_buttonUseSlaveWord) {
            frame.stylus.buttonSource = 2;
        }
    }
    frame.stylus.rawButton = rawButtonBits;
    frame.stylus.button = UpdateButtonState(rawButtonBits, (status & 0x10u) != 0u);

    SolveStylusPressure(frame, tx1, tx2);
    SolveStylusTilt(frame, tx1, tx2);

    frame.stylus.noPressInkActive = (frame.stylus.pressure == 0) && ((frame.stylus.status & 0x10u) != 0);
}

void StylusProcessor::RunAsaMainProcess(HeatmapFrame& frame) {
    frame.stylus.validJudgmentPassed = EvaluateValidJudgment(frame);
    frame.stylus.processResult = 5;
    frame.stylus.freqShiftReleasing = false;
    frame.stylus.modeExitRelease = false;
    frame.stylus.hpp3NoiseInvalid = false;
    frame.stylus.hpp3NoiseDebounce = false;
    if (!frame.stylus.validJudgmentPassed) {
        if ((frame.stylus.status & 0x40u) == 0) frame.stylus.processResult = 1;
        else { frame.stylus.processResult = 3; frame.stylus.freqShiftReleasing = true; }
        m_lastFrameBypass = true;
        return;
    }

    const bool noiseInvalidated = ApplyHpp3NoisePostProcess(frame);
    if (noiseInvalidated && frame.stylus.hpp3NoiseDebounce) {
        frame.stylus.processResult = 5;
        m_lastFrameBypass = true;
        return;
    }

    const bool prevInRange = (m_prevStatus & 0x10u) != 0;
    const bool curInRange = (frame.stylus.status & 0x10u) != 0;
    if (prevInRange && !curInRange) {
        frame.stylus.modeExitRelease = true;
        frame.stylus.processResult = 3;
        m_lastFrameBypass = true;
        m_prevStatus = frame.stylus.status;
        return;
    }

    if (!curInRange) {
        frame.stylus.processResult = 5;
        m_lastFrameBypass = true;
        m_prevStatus = frame.stylus.status;
        return;
    }

    m_prevStatus = frame.stylus.status;
    if ((frame.stylus.status & 0x4u) != 0) {
        frame.stylus.processResult = 3;
        frame.stylus.freqShiftReleasing = true;
        m_lastFrameBypass = true;
        return;
    }
    if (!DispatchHppDataProcess(frame)) {
        frame.stylus.processResult = 3;
        frame.stylus.freqShiftReleasing = true;
        m_lastFrameBypass = true;
        return;
    }
    frame.stylus.processResult = 0;
    m_lastFrameBypass = false;
}

void StylusProcessor::RunStylusRecheck(HeatmapFrame& frame) {
    frame.stylus.recheckEnabled = m_recheckEnabled;
    frame.stylus.recheckPassed = true;
    frame.stylus.recheckOverlap = false;
    frame.stylus.recheckThreshold = 0;
    if ((frame.stylus.status & 0x10u) == 0) { frame.stylus.recheckPassed = false; return; }
    if (!m_recheckEnabled) return;
    if (!frame.stylus.point.valid) { frame.stylus.recheckPassed = false; return; }
    const uint16_t threshold = ComputeRecheckSignalThreshold(frame);
    frame.stylus.recheckThreshold = threshold;
    if (static_cast<int>(frame.stylus.signalX) < threshold || static_cast<int>(frame.stylus.signalY) < threshold) { frame.stylus.recheckPassed = false; return; }
    if (m_windowsPadMode && static_cast<int>(frame.stylus.maxRawPeak) < m_recheckStrongPeakThreshold) { frame.stylus.recheckPassed = false; return; }
    if (m_skipRecheckOnNoPressInk && frame.stylus.noPressInkActive) return;
    if (m_recheckOverlapEnabled && CheckStylusTouchOverlap(frame)) { frame.stylus.recheckOverlap = true; frame.stylus.recheckPassed = false; return; }
    if (m_skipOnInvalidRawEnabled && static_cast<int>(frame.stylus.maxRawPeak) < m_recheckStrongPeakThreshold) frame.stylus.recheckPassed = true;
}

void StylusProcessor::UpdateTouchSuppressionState(HeatmapFrame& frame) {
    frame.stylus.touchNullLike = false;
    frame.stylus.touchSuppressActive = false;
    frame.stylus.touchSuppressFrames = 0;
    const bool needRecheck = m_recheckEnabled && ((frame.stylus.status & 0x2u) != 0) && !m_recheckDisableInFreqShifting;
    if (needRecheck && !frame.stylus.recheckPassed) {
        frame.stylus.processResult = 3;
        m_touchSuppressCounter = 0;
    } else if (frame.stylus.slaveValid && frame.stylus.status != 0) {
        frame.stylus.touchNullLike = EvaluateTouchNullLike(frame.stylus.status);
        if (frame.stylus.touchNullLike) m_touchSuppressCounter = std::max(0, m_touchSuppressHoldFrames);
    }
    if (m_touchSuppressCounter > 0) {
        frame.stylus.touchSuppressActive = true;
        m_touchSuppressCounter -= 1;
        frame.stylus.touchSuppressFrames = static_cast<uint8_t>(std::clamp(m_touchSuppressCounter, 0, 255));
    }
}

void StylusProcessor::BuildStylusPacket(HeatmapFrame& frame) const {
    frame.stylus.packet = StylusPacket{};
    frame.stylus.packet.reportId = 0x08;
    frame.stylus.packet.length = 13;
    if (!frame.stylus.slaveValid) { frame.stylus.packet.valid = false; return; }
    if (!frame.stylus.point.valid && !m_emitPacketWhenInvalid) { frame.stylus.packet.valid = false; return; }
    frame.stylus.packet.valid = true;
    auto& bytes = frame.stylus.packet.bytes;
    bytes.fill(0);
    bytes[0] = frame.stylus.packet.reportId;
    uint8_t statusByte = static_cast<uint8_t>(frame.stylus.status & 0x60u);
    if (frame.stylus.point.valid) {
        statusByte = static_cast<uint8_t>((statusByte & 0xFEu) | (frame.stylus.status & 0x01u));
    } else {
        statusByte = static_cast<uint8_t>(statusByte & 0xFEu);
    }
    bytes[1] = statusByte;
    bytes[2] = 0x80u;

    uint16_t vhfX = 0;
    uint16_t vhfY = 0;
    uint16_t pressure = frame.stylus.pressure;
    int16_t tiltX = 0;
    int16_t tiltY = 0;
    if (frame.stylus.point.valid) {
        const float xNorm = std::clamp(frame.stylus.point.x / static_cast<float>(std::max(1, kCols - 1)), 0.0f, 1.0f);
        const float yNorm = std::clamp(frame.stylus.point.y / static_cast<float>(std::max(1, kRows - 1)), 0.0f, 1.0f);
        const int rawX = std::clamp(static_cast<int>(std::lround(xNorm * 1600.0f)), 0, 1600);
        const int rawY = std::clamp(static_cast<int>(std::lround(yNorm * 1600.0f)), 0, 1600);
        vhfX = static_cast<uint16_t>(std::clamp(16000 - rawX * 10, 0, 0xFFFF));
        vhfY = static_cast<uint16_t>(std::clamp(rawY * 10, 0, 0xFFFF));
        pressure = frame.stylus.point.pressure;
        tiltX = static_cast<int16_t>(std::clamp(static_cast<int>(frame.stylus.point.tiltX), -127, 127));
        tiltY = static_cast<int16_t>(std::clamp(static_cast<int>(frame.stylus.point.tiltY), -127, 127));
    }
    WriteU16Le(bytes, 3, vhfX);
    WriteU16Le(bytes, 5, vhfY);
    WriteU16Le(bytes, 7, pressure);
    WriteU16Le(bytes, 9, static_cast<uint16_t>(tiltX));
    WriteU16Le(bytes, 11, static_cast<uint16_t>(tiltY));
}

} // namespace Engine
