#pragma once

#include "IFrameProcessor.h"
#include <array>
#include <cstdint>

namespace Engine {

class StylusProcessor : public IFrameProcessor {
public:
    bool Process(HeatmapFrame& frame) override;
    std::string GetName() const override { return "Stylus Processor (ASA/HPP2/HPP3-lite)"; }
    void DrawConfigUI() override;
    void SaveConfig(std::ostream& out) const override;
    void LoadConfig(const std::string& key, const std::string& value) override;

private:
    static constexpr size_t kMasterFrameBytes = 5063;
    static constexpr size_t kSlaveFrameBytes = 339;
    static constexpr size_t kSlaveWordCount = 166;
    static constexpr size_t kSlaveWordOffset = kMasterFrameBytes + 7; // 5063 + slave header 7
    static constexpr size_t kStylusBlockWords = 83; // 2 header words + 81 data words
    static constexpr int kRows = 40; // TX
    static constexpr int kCols = 60; // RX
    static constexpr int kBaseline = 0x7FFE;

    struct PeakCentroidResult {
        bool valid = false;
        int peakRow = 0;
        int peakCol = 0;
        int peakDelta = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    struct MasterStylusMeta {
        bool valid = false;
        uint8_t baseWord = 0xFF;
        uint16_t tx1Freq = 0;
        uint16_t tx2Freq = 0;
        uint16_t pressure = 0;
        uint32_t button = 0;
        uint32_t status = 0;
    };

    enum class AsaMode : uint8_t {
        None = 0,
        HPP2 = 1,
        HPP3 = 2,
    };

    void ResetStylusFrame(HeatmapFrame& frame) const;
    bool ParseSlaveWords(
        const HeatmapFrame& frame,
        std::array<uint16_t, kSlaveWordCount>& outWords,
        uint8_t& outWordOffset,
        uint16_t& outChecksum,
        bool& outChecksumOk) const;
    bool ValidateChecksum16(const uint8_t* bytes, size_t wordCount, uint16_t& outChecksum) const;
    bool LooksLikeBlockHeader(uint16_t w0, uint16_t w1) const;
    bool UnpackBlockToMatrix(const uint16_t* block, int16_t outMatrix[kRows][kCols]) const;
    PeakCentroidResult FindPeakCentroid(const int16_t matrix[kRows][kCols]) const;
    uint16_t FindMaxPeakDelta(const int16_t matrix[kRows][kCols]) const;
    uint16_t ComputeRecheckSignalThreshold(const HeatmapFrame& frame) const;
    bool EvaluateValidJudgment(const HeatmapFrame& frame) const;
    AsaMode ResolveAsaMode(const HeatmapFrame& frame) const;
    bool DispatchHppDataProcess(HeatmapFrame& frame) const;
    bool ProcessHpp2Branch(HeatmapFrame& frame) const;
    bool ProcessHpp3Branch(HeatmapFrame& frame) const;
    bool ApplyHpp3NoisePostProcess(HeatmapFrame& frame);
    void UpdateHpp3SignalLevel(const HeatmapFrame& frame, bool signalValid);
    void UpdatePressureHistory(uint16_t rawPressure, bool active);
    uint16_t GetPressureInMapOrder(uint16_t rawPressure) const;
    uint16_t MapPressureHpp3(uint16_t pressInMapOrder) const;
    bool IsPressurePostAllowed(const HeatmapFrame& frame);
    void ApplyPressureSignalSuppression(const HeatmapFrame& frame,
                                        const PeakCentroidResult& tx1,
                                        const PeakCentroidResult& tx2,
                                        uint16_t& inOutPressure);
    void UpdateTiltSignalRatio(uint16_t signalX, uint16_t signalY);
    float ComputeTiltLenLimit() const;
    bool IsTiltInputValid(const HeatmapFrame& frame, const PeakCentroidResult& tx1, const PeakCentroidResult& tx2) const;
    void ResetTiltHistory();
    int ConvertCoordDiffToTilt(float coordDiff, bool dimY) const;
    void SolveStylusPressure(HeatmapFrame& frame, const PeakCentroidResult& tx1, const PeakCentroidResult& tx2);
    void SolveStylusTilt(HeatmapFrame& frame, const PeakCentroidResult& tx1, const PeakCentroidResult& tx2);
    void SolveStylusReportCoordinates(HeatmapFrame& frame) const;
    bool EvaluateTouchNullLike(uint32_t status) const;
    bool CheckStylusTouchOverlap(const HeatmapFrame& frame) const;
    void RunAsaMainProcess(HeatmapFrame& frame);
    void RunStylusRecheck(HeatmapFrame& frame);
    void UpdateTouchSuppressionState(HeatmapFrame& frame);
    void SolveStylusPoint(HeatmapFrame& frame);
    void BuildStylusPacket(HeatmapFrame& frame) const;
    bool TryExtractMasterStylusMeta(const HeatmapFrame& frame, MasterStylusMeta& outMeta);
    uint32_t ResolveRawButtonBits(const HeatmapFrame& frame) const;
    uint32_t UpdateButtonState(uint32_t rawButtonBits, bool stylusActive);

    int m_peakDeltaThreshold = 120;
    int m_centroidRadius = 1;
    float m_confidenceScale = 800.0f;
    int m_reportXMax = 4095;
    int m_reportYMax = 4095;
    bool m_requireSlaveFrame = true;
    bool m_enableSlaveChecksum = true;
    bool m_emitPacketWhenInvalid = true;
    int m_protocolType = 2; // 1=HPP2 protocol, 2=HPP3 protocol
    int m_forcedAsaMode = 0; // 0=Auto, 1=Force HPP2, 2=Force HPP3
    int m_dataType = 0; // 0=Line, 1=IQLine, 2=Grid, 3=TiedGrid

    // Master suffix stylus meta extract (mirrors TSACore stylus DA struct layout).
    bool m_masterMetaEnabled = true;
    bool m_masterMetaAutoDetect = true;
    int m_masterMetaBaseWord = -1; // -1 = auto detect
    uint8_t m_masterMetaDetectedBaseWord = 0xFF;

    uint16_t m_freqA = 0x0018;
    uint16_t m_freqB = 0x00A1;
    int m_currentFreqIndex = 0;
    float m_freqScore = 0.0f;
    float m_freqScoreDecay = 0.95f;
    float m_freqSwitchScoreThreshold = 5000.0f;
    int m_freqRequestHoldFrames = 2;
    int m_freqRequestRemaining = 0;
    int m_pendingFreqIndex = 0;
    int m_unstableStreak = 0;
    int m_unstableStreakThreshold = 4;

    // StylusRecheck-like gates.
    bool m_recheckEnabled = true;
    bool m_recheckDisableInFreqShifting = false;
    bool m_skipRecheckOnNoPressInk = true;
    bool m_windowsPadMode = false;
    bool m_skipOnInvalidRawEnabled = false;
    int m_noiseLevel = 0;
    int m_recheckSignalThreshBase = 120;
    int m_recheckSignalThreshNoisy = 160;
    int m_recheckSignalThreshStrong = 100;
    int m_recheckSignalThreshVeryStrong = 80;
    int m_recheckStrongPeakThreshold = 600;
    int m_recheckStrongRatioQ8 = 256;
    int m_recheckVeryStrongRatioQ8 = 512;
    bool m_recheckOverlapEnabled = true;
    float m_recheckOverlapDistance = 2.5f;
    int m_recheckOverlapPeakThreshold = 1500;
    int m_recheckOverlapTouchSignalThreshold = 6000;
    int m_recheckOverlapTouchSignalThresholdWp = 4000;

    // TSA_ASAProcess touch suppression mirror.
    bool m_hpp3TouchEnableFeature = false;
    int m_touchSuppressHoldFrames = 3;
    int m_touchSuppressCounter = 0;
    uint32_t m_prevStatus = 0;
    bool m_lastFrameBypass = false;

    // HPP3_NoisePostProcess-like gates.
    bool m_hpp3NoisePostEnabled = true;
    int m_hpp3SignalRatioFactor = 5;
    int m_hpp3SignalDropFactor = 5;
    float m_hpp3CoorJumpThreshold = 20.0f; // in heatmap grid unit (0x1400 in Q8)
    int m_hpp3NoiseDebounceMs = 10;
    uint64_t m_lastValidOutputMs = 0;
    uint16_t m_prevValidSignalX = 0;
    uint16_t m_prevValidSignalY = 0;
    float m_prevValidX = 0.0f;
    float m_prevValidY = 0.0f;
    bool m_prevValidPoint = false;
    bool m_hpp3Dim1SignalValid = false;
    bool m_hpp3Dim2SignalValid = false;
    uint8_t m_hpp3RatioWarnCountX = 0;
    uint8_t m_hpp3RatioWarnCountY = 0;
    uint64_t m_pressSigSumDim1 = 0;
    uint64_t m_pressSigSumDim2 = 0;
    int m_pressCnt = 0;
    uint16_t m_pressSigAvgDim1 = 0;
    uint16_t m_pressSigAvgDim2 = 0;

    // Pressure solve/postprocess (HPP3_PressureProcess + PostPressure lite).
    int m_pressureMapMode = 2; // hardcoded incell: 2
    std::array<uint8_t, 6> m_btPressMapOncell{{0, 1, 1, 2, 3, 3}};
    std::array<uint8_t, 4> m_btPressMapIncell{{0, 1, 2, 3}};
    std::array<uint16_t, 4> m_btPressBuf{{0, 0, 0, 0}};
    uint8_t m_btPressCnt = 0;
    bool m_pressurePolyEnabled = true;
    int m_pressureMapSeg1Threshold = 11;
    int m_pressureMapSeg2Threshold = 127;
    // y = c0 + c1*x + c2*x^2 + c3*x^3 + c4*x^4
    std::array<double, 5> m_pressurePolySeg1{{0.0, 0.0, 0.0078740157480315, 0.0, 0.0}};
    std::array<double, 5> m_pressurePolySeg2{{-409.317785463, 4.39982201266, -0.00161165641489, 2.623779267e-07, -1.60182e-11}};
    int m_pressureMapGainPercent = 100;
    int m_pressureIirWeightQ7 = 64; // 64/128 = 0.5 new sample.
    int m_pressureTailFrames = 0;
    int m_pressureTailMin = 10;
    int m_pressureTailDecay = 48;
    bool m_pressureEdgeSuppressEnabled = true;
    int m_pressureEdgeMarginCells = 2;
    int m_pressureEdgeSignalThreshold = 100;
    int m_pressureEdgeSignalReleaseThreshold = 140;
    bool m_pressureEdgeSuppressState = false;
    bool m_pressureSignalSuppressEnabled = true;
    int m_pressureSignalSuppressEnterTh = 60;
    int m_pressureSignalSuppressExitTh = 120;
    bool m_pressureSignalSuppressActive = false;
    int m_pressureTailCounter = 0;
    uint16_t m_prevPressure = 0;
    int m_fakePressureDecreaseStartTh = 100;
    int m_fakePressureDecreaseLevel2Th = 301;
    int m_fakePressureDecreaseLevel3Th = 501;
    bool m_fakePressureDecreaseAdded = false;
    int m_fakePressureDecreaseAddNum = 0;
    bool m_pressurePostRespectFreqShift = true;
    int m_pressurePostFreqShiftDebounceMs = 50;
    int m_pressurePostFreqShiftDebounceMsWp = 150;
    bool m_pressurePostFreqShiftActive = false;
    uint64_t m_pressurePostFreqShiftStartMs = 0;

    // Button process (mirrors TSACore ButtonProcess release-hold behavior).
    bool m_buttonStateEnabled = true;
    bool m_buttonUseMasterMeta = true;
    bool m_buttonUseSlaveWord = false;
    int m_buttonSlaveWordIndex = 0;
    int m_buttonRawBitShift = 0;
    uint32_t m_buttonRawMask = 0x1u;
    int m_buttonReleaseHoldFrames = 2;
    int m_buttonReleaseCounter = 0;

    // Tilt solve (TiltProcess-lite).
    bool m_tiltEnabled = true;
    bool m_tiltKeepLastOnInvalid = true;
    bool m_tiltUseSignalRatioLimit = true;
    int m_tiltRatioAverageWindow = 3;
    int m_tiltRatioBufCount = 0;
    std::array<uint16_t, 10> m_tiltSignalRatioBuf{};
    int m_tiltRatioMinForOutput = 20;
    int m_tiltRatioRef = 100;
    float m_tiltRatioScaleMin = 0.5f;
    float m_tiltRatioScaleMax = 1.5f;
    float m_tiltCoordDiffLimit = 6.0f; // base len-limit (cell unit)
    int m_tiltLenRatioPointCount = 4; // mirrors stylus len-limit interpolation point count.
    std::array<uint16_t, 6> m_tiltLenRatioThresholds{{40, 80, 120, 180, 260, 340}};
    std::array<uint16_t, 6> m_tiltLenScalePermille{{0, 450, 700, 850, 950, 1000}};
    int m_tiltDiffAverageWindow = 5;
    int m_tiltDiffBufCount = 0;
    std::array<float, 10> m_tiltDiffBufX{};
    std::array<float, 10> m_tiltDiffBufY{};
    int m_tiltOutAverageWindow = 5;
    int m_tiltOutBufCount = 0;
    std::array<int16_t, 10> m_tiltOutBufX{};
    std::array<int16_t, 10> m_tiltOutBufY{};
    float m_tiltCoordIirOldWeight = 0.875f; // fallback smoothing
    float m_tiltDegreePerCellX = 8.0f;
    float m_tiltDegreePerCellY = 8.0f;
    float m_tiltNormLenX = 7.16f;
    float m_tiltNormLenY = 7.16f;
    int m_tiltMaxDegree = 60;
    int m_tiltJitterThresholdDeg = 1;
    bool m_tiltHasHistory = false;
    bool m_tiltResetWhenNoPressure = true;
    float m_prevTiltDiffX = 0.0f;
    float m_prevTiltDiffY = 0.0f;
    int16_t m_prevPreTiltX = 0;
    int16_t m_prevPreTiltY = 0;
    int16_t m_prevTiltX = 0;
    int16_t m_prevTiltY = 0;
    uint32_t m_prevTiltStatus = 0;
    uint16_t m_prevTiltPressure = 0;
};

} // namespace Engine
