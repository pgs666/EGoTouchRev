#include "TouchTracker.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine {
namespace {
constexpr uint32_t kReportFlagReported = 0x00000008u;
constexpr uint32_t kReportFlagStart = 0x00100000u;
constexpr uint32_t kReportFlagSuppressed = 0x00000004u;
constexpr uint32_t kReportFlagHistoryApplied = 0x00000080u;
constexpr uint32_t kReportFlagHistoryShift = 0x00200000u;
constexpr uint32_t kReportFlagStepBack = 0x00800000u;
constexpr uint32_t kReportFlagFilterHold = 0x00400000u;

int ComputeReportEvent(bool currentReported, bool previousReported) {
    if (!currentReported && !previousReported) {
        return TouchReportIdle;
    }
    if (currentReported && !previousReported) {
        return TouchReportDown;
    }
    if (currentReported && previousReported) {
        return TouchReportMove;
    }
    return TouchReportUp;
}
}

float TouchTracker::DistanceSq(float x1, float y1, float x2, float y2) {
    const float dx = x1 - x2;
    const float dy = y1 - y2;
    return dx * dx + dy * dy;
}

bool TouchTracker::IsEdgeTouch(float x, float y, int cols, int rows, float edgeMargin) {
    return (x <= edgeMargin) ||
           (y <= edgeMargin) ||
           (x >= static_cast<float>(cols) - edgeMargin) ||
           (y >= static_cast<float>(rows) - edgeMargin);
}

std::vector<int> TouchTracker::SolveAssignment(const std::vector<std::vector<float>>& cost) {
    const int rowsOriginal = static_cast<int>(cost.size());
    if (rowsOriginal == 0) {
        return {};
    }

    const int colsOriginal = static_cast<int>(cost[0].size());
    if (colsOriginal == 0) {
        return std::vector<int>(rowsOriginal, -1);
    }

    bool transposed = false;
    std::vector<std::vector<float>> matrix = cost;
    int n = rowsOriginal;
    int m = colsOriginal;

    if (n > m) {
        transposed = true;
        std::vector<std::vector<float>> transposedMatrix(m, std::vector<float>(n, 0.0f));
        for (int r = 0; r < n; ++r) {
            for (int c = 0; c < m; ++c) {
                transposedMatrix[c][r] = matrix[r][c];
            }
        }
        matrix = std::move(transposedMatrix);
        std::swap(n, m);
    }

    const float kInf = std::numeric_limits<float>::max() / 8.0f;
    std::vector<float> u(n + 1, 0.0f);
    std::vector<float> v(m + 1, 0.0f);
    std::vector<int> p(m + 1, 0);
    std::vector<int> way(m + 1, 0);

    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<float> minv(m + 1, kInf);
        std::vector<char> used(m + 1, false);

        do {
            used[j0] = true;
            const int i0 = p[j0];
            float delta = kInf;
            int j1 = 0;
            for (int j = 1; j <= m; ++j) {
                if (used[j]) {
                    continue;
                }
                const float cur = matrix[i0 - 1][j - 1] - u[i0] - v[j];
                if (cur < minv[j]) {
                    minv[j] = cur;
                    way[j] = j0;
                }
                if (minv[j] < delta) {
                    delta = minv[j];
                    j1 = j;
                }
            }
            for (int j = 0; j <= m; ++j) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);

        do {
            const int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }

    std::vector<int> rowToCol(n, -1);
    for (int j = 1; j <= m; ++j) {
        if (p[j] != 0) {
            rowToCol[p[j] - 1] = j - 1;
        }
    }

    if (!transposed) {
        return rowToCol;
    }

    // Transposed case: rowToCol maps previous->current; convert to current->previous.
    std::vector<int> currentToPrevious(rowsOriginal, -1);
    for (int previousIndex = 0; previousIndex < colsOriginal; ++previousIndex) {
        const int currentIndex = rowToCol[previousIndex];
        if (currentIndex >= 0 && currentIndex < rowsOriginal) {
            currentToPrevious[currentIndex] = previousIndex;
        }
    }
    return currentToPrevious;
}

void TouchTracker::QueuePendingPoint(TrackState& track, float x, float y) {
    constexpr int kCap = TrackState::kPendingHistoryCapacity;
    if (track.pendingCount < kCap) {
        const int idx = (track.pendingStart + track.pendingCount) % kCap;
        track.pendingX[idx] = x;
        track.pendingY[idx] = y;
        track.pendingCount += 1;
        return;
    }

    // Ring overwrite oldest.
    track.pendingX[track.pendingStart] = x;
    track.pendingY[track.pendingStart] = y;
    track.pendingStart = (track.pendingStart + 1) % kCap;
}

bool TouchTracker::PopPendingPoint(TrackState& track, float& outX, float& outY) {
    if (track.pendingCount <= 0) {
        return false;
    }

    outX = track.pendingX[track.pendingStart];
    outY = track.pendingY[track.pendingStart];
    track.pendingStart = (track.pendingStart + 1) % TrackState::kPendingHistoryCapacity;
    track.pendingCount -= 1;
    return true;
}

float TouchTracker::EstimateSizeMm(int area, int signalSum) const {
    if (signalSum > 0) {
        const float fromSignal = std::cbrt(static_cast<float>(signalSum)) * m_sizeSignalScale;
        return std::max(m_fallbackSizeMm, fromSignal);
    }
    if (area > 0) {
        return std::max(m_fallbackSizeMm, std::sqrt(static_cast<float>(area)) * m_sizeAreaScale);
    }
    return m_fallbackSizeMm;
}

int TouchTracker::ComputeTouchDownDebounceFrames(const TouchContact& touch) const {
    int frames = m_touchDownDebounceFrames;
    if (!m_dynamicDebounceEnabled) {
        return frames;
    }

    int extra = 0;
    if (touch.signalSum > 0 && touch.signalSum < m_touchDownWeakSignalThreshold) {
        extra += 1;
    }
    if (touch.sizeMm > 0.0f && touch.sizeMm < m_touchDownSmallSizeThresholdMm) {
        extra += 1;
    }
    if (touch.isEdge) {
        extra += 1;
    }

    extra = std::clamp(extra, 0, m_touchDownDebounceMaxExtra);
    return frames + extra;
}

int TouchTracker::ComputeLiftOffDebounceFrames(const TrackState& track) const {
    if (!m_dynamicDebounceEnabled) {
        return 0;
    }
    if ((track.reportFlags & kReportFlagReported) == 0) {
        return 0;
    }
    if ((track.reportFlags & kReportFlagHistoryApplied) != 0) {
        return 0;
    }

    int frames = m_liftOffDebounceBaseFrames;
    frames += 1;
    if (track.signalSum >= m_liftOffStrongSignalThreshold) {
        frames += 1;
    }
    if (track.sizeMm >= m_liftOffLargeSizeThresholdMm) {
        frames += 1;
    }

    return std::clamp(frames, 0, m_liftOffDebounceMaxFrames);
}

int TouchTracker::AllocateId(const std::vector<TrackState>& reservedNextTracks) const {
    for (int i = 0; i < m_maxTouchCount; ++i) {
        const int candidate = ((m_nextIdSeed - 1 + i) % m_maxTouchCount) + 1;
        bool used = false;

        for (const auto& t : reservedNextTracks) {
            if (t.id == candidate) {
                used = true;
                break;
            }
        }
        if (used) {
            continue;
        }
        for (const auto& t : m_tracks) {
            if (t.id == candidate) {
                used = true;
                break;
            }
        }
        if (!used) {
            return candidate;
        }
    }
    return 0;
}

bool TouchTracker::ApplyStylusTouchSuppression(HeatmapFrame& frame) {
    if (m_stylusSuppressGlobalEnabled && frame.stylus.touchSuppressActive) {
        frame.contacts.clear();
        m_tracks.clear();
        m_recentReleased.clear();
        return true;
    }

    if (!m_stylusSuppressLocalEnabled || !frame.stylus.point.valid) {
        return false;
    }

    const int penPeak = std::max({static_cast<int>(frame.stylus.signalX),
                                  static_cast<int>(frame.stylus.signalY),
                                  static_cast<int>(frame.stylus.maxRawPeak)});
    if (penPeak < m_stylusSuppressPenPeakThreshold) {
        return false;
    }

    const float radiusSq = m_stylusSuppressLocalDistance * m_stylusSuppressLocalDistance;
    const float stylusX = frame.stylus.point.x;
    const float stylusY = frame.stylus.point.y;
    frame.contacts.erase(std::remove_if(frame.contacts.begin(), frame.contacts.end(),
                        [&](const TouchContact& c) {
                            const float distSq = DistanceSq(c.x, c.y, stylusX, stylusY);
                            if (distSq > radiusSq) {
                                return false;
                            }

                            const bool keepStrongTouch = (c.signalSum >= m_stylusSuppressTouchSignalKeep) &&
                                                         (c.area >= m_stylusSuppressTouchAreaKeep);
                            return !keepStrongTouch;
                        }),
                        frame.contacts.end());
    return false;
}

bool TouchTracker::ResolveStylusAftContext(const HeatmapFrame& frame, float& outStylusX, float& outStylusY) {
    if (!m_stylusAftEnabled) {
        return false;
    }

    if (frame.stylus.point.valid) {
        m_lastStylusX = frame.stylus.point.x;
        m_lastStylusY = frame.stylus.point.y;
        m_stylusFramesSinceActive = 0;
    } else if (m_stylusFramesSinceActive < 1000000) {
        m_stylusFramesSinceActive += 1;
    }

    if (m_stylusFramesSinceActive > m_stylusAftRecentFrames) {
        return false;
    }

    outStylusX = m_lastStylusX;
    outStylusY = m_lastStylusY;
    return true;
}

bool TouchTracker::ShouldStylusAftSuppress(
    const TouchContact& touch,
    int touchAge,
    float stylusX,
    float stylusY,
    int& outHoldFrames) const {
    outHoldFrames = 0;
    if (!m_stylusAftEnabled) {
        return false;
    }

    const float distSq = DistanceSq(touch.x, touch.y, stylusX, stylusY);
    if (distSq > (m_stylusAftRadius * m_stylusAftRadius)) {
        return false;
    }

    const bool palmLike = (touch.area >= m_stylusAftPalmAreaThreshold) ||
                          (touch.sizeMm >= m_stylusAftPalmSizeThresholdMm);
    const bool weakTouch = (touch.signalSum < m_stylusAftWeakSignalThreshold) &&
                           (touch.sizeMm < m_stylusAftWeakSizeThresholdMm);
    const bool inDebounceAge = (touchAge <= m_stylusAftDebounceFrames);

    if (palmLike) {
        outHoldFrames = std::max(0, m_stylusAftPalmSuppressFrames);
        return true;
    }
    if (weakTouch || inDebounceAge) {
        outHoldFrames = std::max(0, m_stylusAftSuppressFrames);
        return true;
    }
    return false;
}

bool TouchTracker::Process(HeatmapFrame& frame) {
    if (!m_enabled) {
        return true;
    }

    if (frame.contacts.size() > static_cast<size_t>(m_maxTouchCount)) {
        frame.contacts.resize(static_cast<size_t>(m_maxTouchCount));
    }

    if (ApplyStylusTouchSuppression(frame)) {
        return true;
    }

    constexpr int kRows = 40;
    constexpr int kCols = 60;
    constexpr float kEdgeMargin = 2.0f;
    float stylusAftX = 0.0f;
    float stylusAftY = 0.0f;
    const bool stylusAftActive = ResolveStylusAftContext(frame, stylusAftX, stylusAftY);

    // TE recovery window maintenance (short-frame re-acquire support).
    if (!m_recentReleased.empty()) {
        for (auto& r : m_recentReleased) {
            r.framesLeft -= 1;
        }
        m_recentReleased.erase(
            std::remove_if(m_recentReleased.begin(), m_recentReleased.end(),
                           [](const RecentReleasedTrack& r) { return r.framesLeft < 0; }),
            m_recentReleased.end());
    }

    const size_t currentCount = frame.contacts.size();
    const size_t previousCount = m_tracks.size();

    std::vector<int> currentToPrevious(currentCount, -1);
    std::vector<char> alwaysMatchedCurrent(currentCount, false);
    const float maxDistSq = m_maxTrackDistance * m_maxTrackDistance;
    const float alwaysMatchDistSq = m_alwaysMatchDistance * m_alwaysMatchDistance;
    const float edgeBoostSq = m_edgeTrackBoost * m_edgeTrackBoost;

    if (currentCount > 0 && previousCount > 0) {
        std::vector<std::vector<float>> cost(currentCount, std::vector<float>(previousCount, 0.0f));
        for (size_t cur = 0; cur < currentCount; ++cur) {
            for (size_t pre = 0; pre < previousCount; ++pre) {
                const auto& track = m_tracks[pre];
                const float predX = track.x + track.vx * m_predictionScale;
                const float predY = track.y + track.vy * m_predictionScale;
                cost[cur][pre] = DistanceSq(frame.contacts[cur].x, frame.contacts[cur].y, predX, predY);
            }
        }

        if (m_useHungarian) {
            currentToPrevious = SolveAssignment(cost);
        } else {
            std::vector<char> prevUsed(previousCount, false);
            for (size_t cur = 0; cur < currentCount; ++cur) {
                float best = std::numeric_limits<float>::max();
                int bestIdx = -1;
                for (size_t pre = 0; pre < previousCount; ++pre) {
                    if (prevUsed[pre]) {
                        continue;
                    }
                    if (cost[cur][pre] < best) {
                        best = cost[cur][pre];
                        bestIdx = static_cast<int>(pre);
                    }
                }
                if (bestIdx >= 0) {
                    prevUsed[bestIdx] = true;
                    currentToPrevious[cur] = bestIdx;
                }
            }
        }

        for (size_t cur = 0; cur < currentCount; ++cur) {
            const int pre = currentToPrevious[cur];
            if (pre < 0) {
                continue;
            }
            const auto& track = m_tracks[static_cast<size_t>(pre)];
            const float predX = track.x + track.vx * m_predictionScale;
            const float predY = track.y + track.vy * m_predictionScale;
            const float distSq = DistanceSq(frame.contacts[cur].x, frame.contacts[cur].y, predX, predY);

            float matchThresholdSq = maxDistSq;
            const bool edgeTouch = IsEdgeTouch(track.x, track.y, kCols, kRows, kEdgeMargin) ||
                                   IsEdgeTouch(frame.contacts[cur].x, frame.contacts[cur].y, kCols, kRows, kEdgeMargin);
            if (edgeTouch) {
                matchThresholdSq *= edgeBoostSq;
            }
            const float approxSizeMm = std::max(track.sizeMm,
                                                EstimateSizeMm(frame.contacts[cur].area,
                                                               frame.contacts[cur].signalSum));
            const bool lowAccTouch = (approxSizeMm <= m_accBoostSizeMm);
            if (edgeTouch || lowAccTouch) {
                matchThresholdSq *= (m_accThresholdBoost * m_accThresholdBoost);
            }

            if (distSq > matchThresholdSq) {
                currentToPrevious[cur] = -1;
            }
        }

        // IDT_AlwaysMatchProcess-like fallback: if both sides are still unmatched
        // and very close, force a stable pairing.
        std::vector<char> curUsed(currentCount, false);
        std::vector<char> preUsed(previousCount, false);
        for (size_t cur = 0; cur < currentCount; ++cur) {
            const int pre = currentToPrevious[cur];
            if (pre >= 0 && static_cast<size_t>(pre) < previousCount) {
                curUsed[cur] = true;
                preUsed[static_cast<size_t>(pre)] = true;
            }
        }

        for (size_t cur = 0; cur < currentCount; ++cur) {
            if (curUsed[cur]) {
                continue;
            }

            float bestDistSq = std::numeric_limits<float>::max();
            int bestPre = -1;
            for (size_t pre = 0; pre < previousCount; ++pre) {
                if (preUsed[pre]) {
                    continue;
                }
                const auto& track = m_tracks[pre];
                const float predX = track.x + track.vx * m_predictionScale;
                const float predY = track.y + track.vy * m_predictionScale;
                const float distSq = DistanceSq(frame.contacts[cur].x, frame.contacts[cur].y, predX, predY);
                if (distSq < bestDistSq) {
                    bestDistSq = distSq;
                    bestPre = static_cast<int>(pre);
                }
            }

            const bool curIsEdge = IsEdgeTouch(frame.contacts[cur].x, frame.contacts[cur].y,
                                               kCols, kRows, kEdgeMargin);
            if (bestPre >= 0 && bestDistSq <= alwaysMatchDistSq && !curIsEdge) {
                currentToPrevious[cur] = bestPre;
                alwaysMatchedCurrent[cur] = true;
                curUsed[cur] = true;
                preUsed[static_cast<size_t>(bestPre)] = true;
            }
        }
    }

    std::vector<char> previousMatched(previousCount, false);
    std::vector<TrackState> nextTracks;
    nextTracks.reserve(static_cast<size_t>(m_maxTouchCount));

    std::vector<TouchContact> outputContacts;
    outputContacts.reserve(currentCount + previousCount);

    const float reportMoveThresholdSq = m_reportMoveThreshold * m_reportMoveThreshold;

    for (size_t cur = 0; cur < currentCount; ++cur) {
        TouchContact out = frame.contacts[cur];
        const int pre = currentToPrevious[cur];
        if (pre >= 0) {
            const size_t preIdx = static_cast<size_t>(pre);
            previousMatched[preIdx] = true;
            TrackState t = m_tracks[preIdx];
            const float rawX = out.x;
            const float rawY = out.y;

            out.id = t.id;
            out.prevIndex = pre;
            out.debugFlags = 0x01;
            out.isEdge = IsEdgeTouch(rawX, rawY, kCols, kRows, kEdgeMargin);
            out.lifeFlags = TouchLifeMapped;
            if (out.isEdge) {
                out.lifeFlags |= TouchLifeEdge;
            }
            if (alwaysMatchedCurrent[cur]) {
                out.lifeFlags |= TouchLifeAlwaysMatch;
            }

            const float prevX = t.x;
            const float prevY = t.y;
            const float currentSizeMm = EstimateSizeMm(out.area, out.signalSum);
            t.sizeMm = std::max({currentSizeMm, t.sizeMm, m_fallbackSizeMm});
            out.sizeMm = t.sizeMm;
            out.state = ((t.age <= 1) || (t.downDebounceFrames > 0)) ? TouchStateDown : TouchStateMove;

            if (t.downDebounceFrames > 0) {
                out.state = TouchStateDown;
                out.lifeFlags |= TouchLifeDebounced;
                t.downDebounceFrames -= 1;
            }

            t.vx = rawX - prevX;
            t.vy = rawY - prevY;
            t.x = rawX;
            t.y = rawY;
            t.area = out.area;
            t.signalSum = out.signalSum;
            t.missed = 0;
            t.age += 1;
            t.upEventEmitted = false;
            t.reportFlags &= ~kReportFlagStepBack;
            if (!stylusAftActive && t.stylusSuppressFrames > 0) {
                t.stylusSuppressFrames -= 1;
            }

            const bool prevReported = (t.reportFlags & kReportFlagReported) != 0;
            bool shouldReport = true;
            if (m_enableReportFilter && out.state == TouchStateMove) {
                const float dx = out.x - t.reportX;
                const float dy = out.y - t.reportY;
                shouldReport = (dx * dx + dy * dy) >= reportMoveThresholdSq;
            }
            const bool inDownDebounce = (out.state == TouchStateDown) &&
                                        ((out.lifeFlags & TouchLifeDebounced) != 0);
            if (inDownDebounce && !m_reportDuringDownDebounce) {
                shouldReport = false;
            }
            if (m_reportStartGateEnabled && !prevReported) {
                if (out.signalSum < m_reportStartMinSignal) {
                    shouldReport = false;
                }
                if (out.sizeMm < m_reportStartMinSizeMm) {
                    shouldReport = false;
                }
                if (m_reportSuppressEdgeFirst && out.isEdge &&
                    out.signalSum < m_reportEdgeStartMinSignal) {
                    shouldReport = false;
                }
            }
            if (m_reportEdgeWeakSuppressEnabled && !prevReported && out.isEdge &&
                out.signalSum < m_reportEdgeWeakSignalThreshold &&
                out.sizeMm < m_reportEdgeWeakSizeThresholdMm) {
                shouldReport = false;
            }
            if (m_reportWeakTouchSuppressEnabled && prevReported && out.state == TouchStateMove) {
                const float dxRaw = rawX - t.reportX;
                const float dyRaw = rawY - t.reportY;
                const float weakMoveSq = dxRaw * dxRaw + dyRaw * dyRaw;
                const float weakMoveThresholdSq = m_reportWeakTouchMinMove * m_reportWeakTouchMinMove;
                if (out.signalSum < m_reportWeakTouchMinSignal &&
                    out.sizeMm < m_reportWeakTouchMinSizeMm &&
                    weakMoveSq < weakMoveThresholdSq) {
                    shouldReport = false;
                }
            }
            if (!prevReported) {
                if (shouldReport) {
                    t.startGatePassCount += 1;
                } else {
                    t.startGatePassCount = 0;
                }
                if (t.startGatePassCount < std::max(1, m_reportStartStableFrames)) {
                    shouldReport = false;
                } else {
                    t.startGatePassCount = 0;
                }
            } else {
                t.startGatePassCount = 0;
            }

            bool stylusAftSuppress = false;
            if (stylusAftActive) {
                if (t.stylusSuppressFrames > 0) {
                    stylusAftSuppress = true;
                    t.stylusSuppressFrames -= 1;
                } else {
                    int holdFrames = 0;
                    if (ShouldStylusAftSuppress(out, t.age, stylusAftX, stylusAftY, holdFrames)) {
                        stylusAftSuppress = true;
                        t.stylusSuppressFrames = std::max(0, holdFrames - 1);
                    }
                }
            }
            if (stylusAftSuppress) {
                shouldReport = false;
                out.debugFlags |= 0x100;
            }

            const bool rawShouldReport = shouldReport;
            bool holdReportedCoordinate = false;
            if (!prevReported) {
                t.suppressStreak = 0;
            } else if (!rawShouldReport && m_reportSuppressDebounceFrames > 0) {
                t.suppressStreak += 1;
                if (t.suppressStreak <= m_reportSuppressDebounceFrames) {
                    shouldReport = true;
                    holdReportedCoordinate = m_reportHoldPrevCoordinate;
                } else {
                    shouldReport = false;
                }
            } else {
                if (rawShouldReport) {
                    t.suppressStreak = 0;
                }
                shouldReport = rawShouldReport;
            }

            if (!rawShouldReport && m_enableReportHistoryReplay && out.state != TouchStateUp) {
                QueuePendingPoint(t, rawX, rawY);
            }
            bool historyApplied = false;
            if (shouldReport && !holdReportedCoordinate && m_enableReportHistoryReplay) {
                float replayX = 0.0f;
                float replayY = 0.0f;
                if (PopPendingPoint(t, replayX, replayY)) {
                    out.x = replayX;
                    out.y = replayY;
                    historyApplied = true;
                    out.debugFlags |= 0x10;
                }
            }
            if (!historyApplied && !holdReportedCoordinate) {
                out.x = rawX;
                out.y = rawY;
            }
            if (holdReportedCoordinate) {
                out.x = t.reportX;
                out.y = t.reportY;
            }
            bool edgeJitterHold = false;
            if (shouldReport && prevReported && !holdReportedCoordinate &&
                out.state == TouchStateMove && m_reportEdgeJitterHoldEnabled &&
                out.isEdge && out.signalSum < m_reportEdgeJitterSignalThreshold &&
                t.age <= m_reportEdgeJitterMaxAge) {
                out.x = t.reportX;
                out.y = t.reportY;
                edgeJitterHold = true;
                out.debugFlags |= 0x80;
            }
            if (shouldReport && prevReported && !holdReportedCoordinate && !edgeJitterHold &&
                out.state == TouchStateMove && m_reportCoordinateFilterEnabled) {
                const float prevReportX = t.reportX;
                const float prevReportY = t.reportY;
                const float dx = out.x - prevReportX;
                const float dy = out.y - prevReportY;
                const float distSq = dx * dx + dy * dy;
                const float thresholdSq = m_reportCoordinateFilterDistThreshold *
                                          m_reportCoordinateFilterDistThreshold;
                const float alpha = (distSq <= thresholdSq)
                                        ? m_reportCoordinateFilterAlphaSmall
                                        : m_reportCoordinateFilterAlphaLarge;
                const float alphaClamped = std::clamp(alpha, 0.0f, 1.0f);
                out.x = prevReportX + dx * alphaClamped;
                out.y = prevReportY + dy * alphaClamped;
                out.debugFlags |= 0x40;
            }

            int resolvedReportEvent = ComputeReportEvent(shouldReport, prevReported);
            // TS/TE consistency: when a track is already reported, keep Down as a one-shot transition.
            if (out.state == TouchStateDown && shouldReport && !prevReported) {
                resolvedReportEvent = TouchReportDown;
            }
            out.reportEvent = resolvedReportEvent;
            out.isReported = shouldReport || (resolvedReportEvent == TouchReportUp);
            if (shouldReport) {
                t.reportFlags |= kReportFlagReported;
                t.reportFlags |= kReportFlagStart;
                t.reportFlags &= ~kReportFlagSuppressed;
                if (holdReportedCoordinate) {
                    t.reportFlags |= kReportFlagFilterHold;
                } else {
                    t.reportFlags &= ~kReportFlagFilterHold;
                }
                if (edgeJitterHold) {
                    t.reportFlags |= kReportFlagFilterHold;
                }
                if (historyApplied) {
                    t.reportFlags |= kReportFlagHistoryApplied;
                    t.reportFlags |= kReportFlagHistoryShift;
                } else {
                    t.reportFlags &= ~kReportFlagHistoryApplied;
                    t.reportFlags &= ~kReportFlagHistoryShift;
                }
            } else {
                t.reportFlags &= ~kReportFlagReported;
                t.reportFlags &= ~kReportFlagStart;
                t.reportFlags &= ~kReportFlagHistoryApplied;
                t.reportFlags &= ~kReportFlagHistoryShift;
                t.reportFlags &= ~kReportFlagFilterHold;
                t.reportFlags |= kReportFlagSuppressed;
            }
            out.reportFlags = t.reportFlags;
            t.reportActive = shouldReport;
            if (shouldReport) {
                t.reportX = out.x;
                t.reportY = out.y;
            }
            t.liftOffDebounceFrames = ComputeLiftOffDebounceFrames(t);

            if (outputContacts.size() < static_cast<size_t>(m_maxTouchCount)) {
                outputContacts.push_back(out);
            }
            if (nextTracks.size() < static_cast<size_t>(m_maxTouchCount)) {
                nextTracks.push_back(t);
            }
            continue;
        }

        if (m_teRecoverEnabled && !m_recentReleased.empty()) {
            const float recoverDistSq = m_teRecoverDistance * m_teRecoverDistance;
            const float minReleaseSpeedSq = m_teRecoverMinReleaseSpeed * m_teRecoverMinReleaseSpeed;
            const float minRecoverCos = std::clamp(m_teRecoverMinCosine, -1.0f, 1.0f);
            float bestDistSq = recoverDistSq;
            int bestRecoverIdx = -1;

            for (size_t i = 0; i < m_recentReleased.size(); ++i) {
                const auto& released = m_recentReleased[i];
                if (!released.wasReported) {
                    continue;
                }

                const bool idAlreadyUsed = std::any_of(
                    nextTracks.begin(), nextTracks.end(),
                    [&](const TrackState& tr) { return tr.id == released.id; });
                if (idAlreadyUsed) {
                    continue;
                }

                const float distSq = DistanceSq(out.x, out.y, released.x, released.y);
                if (distSq > recoverDistSq) {
                    continue;
                }

                if (m_teRecoverDirectionConstraint) {
                    const float releaseVelSq = released.vx * released.vx + released.vy * released.vy;
                    if (releaseVelSq >= minReleaseSpeedSq && distSq > 1e-6f) {
                        const float dx = out.x - released.x;
                        const float dy = out.y - released.y;
                        const float dot = released.vx * dx + released.vy * dy;
                        const float invDen = 1.0f / std::sqrt(releaseVelSq * distSq);
                        const float cosine = dot * invDen;
                        if (cosine < minRecoverCos) {
                            continue;
                        }
                    }
                }

                if (distSq <= bestDistSq) {
                    bestDistSq = distSq;
                    bestRecoverIdx = static_cast<int>(i);
                }
            }

            if (bestRecoverIdx >= 0 &&
                outputContacts.size() < static_cast<size_t>(m_maxTouchCount) &&
                nextTracks.size() < static_cast<size_t>(m_maxTouchCount)) {
                const RecentReleasedTrack released = m_recentReleased[static_cast<size_t>(bestRecoverIdx)];

                TrackState t;
                t.id = released.id;
                t.x = out.x;
                t.y = out.y;
                t.vx = out.x - released.x;
                t.vy = out.y - released.y;
                t.area = out.area;
                t.signalSum = out.signalSum;
                t.sizeMm = std::max(released.sizeMm, EstimateSizeMm(out.area, out.signalSum));
                t.age = 2;
                t.missed = 0;
                t.downDebounceFrames = 0;
                t.liftOffDebounceFrames = 0;
                t.startGatePassCount = 0;
                t.suppressStreak = 0;
                t.upEventEmitted = false;
                t.reportActive = true;
                t.reportX = out.x;
                t.reportY = out.y;
                t.reportFlags = (kReportFlagReported | kReportFlagStart);

                out.id = t.id;
                out.state = TouchStateMove;
                out.sizeMm = t.sizeMm;
                out.isEdge = IsEdgeTouch(out.x, out.y, kCols, kRows, kEdgeMargin);
                out.isReported = true;
                out.prevIndex = -1;
                out.debugFlags = 0x12;
                out.lifeFlags = TouchLifeMapped;
                if (out.isEdge) {
                    out.lifeFlags |= TouchLifeEdge;
                }
                out.reportFlags = t.reportFlags;
                out.reportEvent = TouchReportMove;

                t.liftOffDebounceFrames = ComputeLiftOffDebounceFrames(t);

                outputContacts.push_back(out);
                nextTracks.push_back(t);

                m_recentReleased.erase(m_recentReleased.begin() + bestRecoverIdx);
                continue;
            }
        }

        TrackState t;
        t.id = AllocateId(nextTracks);
        if (t.id == 0) {
            continue;
        }
        t.x = out.x;
        t.y = out.y;
        t.vx = 0.0f;
        t.vy = 0.0f;
        t.area = out.area;
        t.signalSum = out.signalSum;
        t.sizeMm = EstimateSizeMm(out.area, out.signalSum);
        t.age = 1;
        t.missed = 0;
        out.isEdge = IsEdgeTouch(out.x, out.y, kCols, kRows, kEdgeMargin);

        if (m_touchDownRejectEnabled) {
            const bool weakSignal = (t.signalSum < m_touchDownRejectMinSignal);
            const bool tinySize = (t.sizeMm < m_touchDownRejectMinSizeMm);
            const bool weakEdge = out.isEdge && (t.signalSum < m_touchDownEdgeRejectMinSignal);
            if ((weakSignal && tinySize) || weakEdge) {
                continue;
            }
        }

        t.downDebounceFrames = ComputeTouchDownDebounceFrames(out);
        t.liftOffDebounceFrames = 0;
        t.upEventEmitted = false;
        t.reportActive = false;
        t.reportX = out.x;
        t.reportY = out.y;
        t.reportFlags = kReportFlagSuppressed;
        t.suppressStreak = 0;
        t.liftOffDebounceFrames = ComputeLiftOffDebounceFrames(t);

        m_nextIdSeed = (t.id % m_maxTouchCount) + 1;

        out.id = t.id;
        out.state = TouchStateDown;
        out.sizeMm = t.sizeMm;
        out.isEdge = IsEdgeTouch(out.x, out.y, kCols, kRows, kEdgeMargin);
        out.isReported = true;
        out.prevIndex = -1;
        out.debugFlags = 0x02;
        out.lifeFlags = TouchLifeNew;
        if (out.isEdge) {
            out.lifeFlags |= TouchLifeEdge;
        }
        if (t.downDebounceFrames > 0) {
            out.lifeFlags |= TouchLifeDebounced;
            t.downDebounceFrames -= 1;
        }

        const bool inDownDebounce = (out.lifeFlags & TouchLifeDebounced) != 0;
        bool shouldReport = !inDownDebounce || m_reportDuringDownDebounce;
        if (m_reportStartGateEnabled) {
            if (out.signalSum < m_reportStartMinSignal) {
                shouldReport = false;
            }
            if (out.sizeMm < m_reportStartMinSizeMm) {
                shouldReport = false;
            }
            if (m_reportSuppressEdgeFirst && out.isEdge &&
                out.signalSum < m_reportEdgeStartMinSignal) {
                shouldReport = false;
            }
        }
        if (m_reportEdgeWeakSuppressEnabled && out.isEdge &&
            out.signalSum < m_reportEdgeWeakSignalThreshold &&
            out.sizeMm < m_reportEdgeWeakSizeThresholdMm) {
            shouldReport = false;
        }
        if (shouldReport) {
            t.startGatePassCount += 1;
        } else {
            t.startGatePassCount = 0;
        }
        if (t.startGatePassCount < std::max(1, m_reportStartStableFrames)) {
            shouldReport = false;
        } else {
            t.startGatePassCount = 0;
        }

        if (stylusAftActive) {
            int holdFrames = 0;
            if (ShouldStylusAftSuppress(out, t.age, stylusAftX, stylusAftY, holdFrames)) {
                shouldReport = false;
                t.stylusSuppressFrames = std::max(0, holdFrames - 1);
                out.debugFlags |= 0x100;
            }
        }

        if (!shouldReport && m_enableReportHistoryReplay) {
            QueuePendingPoint(t, out.x, out.y);
        }
        if (shouldReport && m_enableReportHistoryReplay) {
            float replayX = 0.0f;
            float replayY = 0.0f;
            if (PopPendingPoint(t, replayX, replayY)) {
                out.x = replayX;
                out.y = replayY;
                out.debugFlags |= 0x10;
                t.reportFlags |= kReportFlagHistoryApplied;
            } else {
                t.reportFlags &= ~kReportFlagHistoryApplied;
            }
        } else {
            t.reportFlags &= ~kReportFlagHistoryApplied;
        }

        out.isReported = shouldReport;
        if (shouldReport) {
            t.reportActive = true;
            t.reportFlags &= ~kReportFlagSuppressed;
            t.reportFlags &= ~kReportFlagFilterHold;
            t.reportFlags |= (kReportFlagReported | kReportFlagStart);
            if ((t.reportFlags & kReportFlagHistoryApplied) != 0 && out.state == TouchStateMove) {
                t.reportFlags |= kReportFlagHistoryShift;
            } else {
                t.reportFlags &= ~kReportFlagHistoryShift;
            }
            out.reportEvent = TouchReportDown;
            t.reportX = out.x;
            t.reportY = out.y;
        } else {
            t.reportActive = false;
            t.reportFlags &= ~kReportFlagReported;
            t.reportFlags &= ~kReportFlagStart;
            t.reportFlags &= ~kReportFlagHistoryShift;
            t.reportFlags &= ~kReportFlagFilterHold;
            t.reportFlags |= kReportFlagSuppressed;
            out.reportEvent = TouchReportIdle;
        }
        out.reportFlags = t.reportFlags;
        t.liftOffDebounceFrames = ComputeLiftOffDebounceFrames(t);

        if (outputContacts.size() < static_cast<size_t>(m_maxTouchCount)) {
            outputContacts.push_back(out);
        }
        if (nextTracks.size() < static_cast<size_t>(m_maxTouchCount)) {
            nextTracks.push_back(t);
        }
    }

    for (size_t pre = 0; pre < previousCount; ++pre) {
        if (previousMatched[pre]) {
            continue;
        }

        TrackState t = m_tracks[pre];
        t.missed += 1;
        t.suppressStreak = 0;
        if (t.stylusSuppressFrames > 0) {
            t.stylusSuppressFrames -= 1;
        }
        const int liftOffDebounce = std::max(0, t.liftOffDebounceFrames);
        const bool lastReported = (t.reportFlags & kReportFlagReported) != 0;
        const bool edgeTrack = IsEdgeTouch(t.x, t.y, kCols, kRows, kEdgeMargin);
        int effectiveLiftOffDebounce = liftOffDebounce;
        if (lastReported) {
            effectiveLiftOffDebounce += std::max(0, m_teLostGraceFrames);
            if (edgeTrack) {
                effectiveLiftOffDebounce += 1;
            }
        }

        if (!t.upEventEmitted && t.missed > effectiveLiftOffDebounce) {
            const bool prevReported = (t.reportFlags & kReportFlagReported) != 0;
            t.reportFlags &= ~kReportFlagReported;
            t.reportFlags &= ~kReportFlagStart;
            t.reportFlags &= ~kReportFlagHistoryApplied;
            t.reportFlags &= ~kReportFlagHistoryShift;
            t.reportFlags &= ~kReportFlagStepBack;
            t.reportFlags &= ~kReportFlagFilterHold;
            if (!prevReported) {
                t.reportFlags |= kReportFlagSuppressed;
            }

            if (prevReported || m_emitUnreportedLiftOff) {
                TouchContact upEvent;
                upEvent.id = t.id;
                upEvent.x = t.x;
                upEvent.y = t.y;
                upEvent.state = TouchStateUp;
                upEvent.area = t.area;
                upEvent.signalSum = t.signalSum;
                upEvent.sizeMm = t.sizeMm;
                upEvent.isEdge = IsEdgeTouch(upEvent.x, upEvent.y, kCols, kRows, kEdgeMargin);
                upEvent.isReported = true;
                upEvent.prevIndex = static_cast<int>(pre);
                upEvent.debugFlags = 0x04;
                upEvent.lifeFlags = TouchLifeLiftOff;
                if (upEvent.isEdge) {
                    upEvent.lifeFlags |= TouchLifeEdge;
                }
                upEvent.reportFlags = t.reportFlags;
                upEvent.reportEvent = TouchReportUp;
                if (outputContacts.size() < static_cast<size_t>(m_maxTouchCount)) {
                    outputContacts.push_back(upEvent);
                }
            }

            if (m_teRecoverEnabled && prevReported) {
                m_recentReleased.erase(
                    std::remove_if(m_recentReleased.begin(), m_recentReleased.end(),
                                   [&](const RecentReleasedTrack& r) { return r.id == t.id; }),
                    m_recentReleased.end());

                RecentReleasedTrack released;
                released.id = t.id;
                released.x = t.x;
                released.y = t.y;
                released.vx = t.vx;
                released.vy = t.vy;
                released.area = t.area;
                released.signalSum = t.signalSum;
                released.sizeMm = t.sizeMm;
                released.wasReported = true;
                released.framesLeft = std::max(0, m_teRecoverWindowFrames);
                m_recentReleased.push_back(released);
            }
            t.upEventEmitted = true;
            t.reportActive = false;
        }

        if (t.missed <= (effectiveLiftOffDebounce + m_liftOffHoldFrames)) {
            t.vx = 0.0f;
            t.vy = 0.0f;

            if (nextTracks.size() < static_cast<size_t>(m_maxTouchCount)) {
                nextTracks.push_back(t);
            }
        }
    }

    if (m_rxGhostFilterEnabled && outputContacts.size() > 1) {
        std::array<uint8_t, 21> removeById{};
        removeById.fill(0);

        for (size_t i = 0; i < outputContacts.size(); ++i) {
            const auto& a = outputContacts[i];
            if (a.state == TouchStateUp || a.id <= 0 || a.id > m_maxTouchCount) {
                continue;
            }

            for (size_t j = i + 1; j < outputContacts.size(); ++j) {
                const auto& b = outputContacts[j];
                if (b.state == TouchStateUp || b.id <= 0 || b.id > m_maxTouchCount) {
                    continue;
                }

                const int lineDelta = std::abs(static_cast<int>(std::lround(a.y)) -
                                               static_cast<int>(std::lround(b.y)));
                if (lineDelta > m_rxGhostLineDelta) {
                    continue;
                }

                const TouchContact* strong = &a;
                const TouchContact* weak = &b;
                if (b.signalSum > a.signalSum) {
                    strong = &b;
                    weak = &a;
                }

                if (weak->signalSum >= static_cast<int>(static_cast<float>(strong->signalSum) * m_rxGhostWeakRatio)) {
                    continue;
                }
                if (m_rxGhostOnlyNew && weak->state != TouchStateDown) {
                    continue;
                }

                removeById[static_cast<size_t>(weak->id)] = 1;
            }
        }

        outputContacts.erase(std::remove_if(outputContacts.begin(), outputContacts.end(),
                           [&](const TouchContact& c) {
                               if (c.state == TouchStateUp || c.id <= 0 || c.id > m_maxTouchCount) {
                                   return false;
                               }
                               return removeById[static_cast<size_t>(c.id)] != 0;
                           }),
                           outputContacts.end());

        nextTracks.erase(std::remove_if(nextTracks.begin(), nextTracks.end(),
                        [&](const TrackState& t) {
                            if (t.id <= 0 || t.id > m_maxTouchCount) {
                                return false;
                            }
                            return removeById[static_cast<size_t>(t.id)] != 0;
                        }),
                        nextTracks.end());
    }

    if (m_enableReportMultiStepBack && !outputContacts.empty()) {
        bool allSuppressedMoves = true;
        int activeMoveCount = 0;

        for (const auto& c : outputContacts) {
            if (c.state == TouchStateUp) {
                continue;
            }
            if (c.state != TouchStateMove) {
                allSuppressedMoves = false;
                break;
            }
            activeMoveCount += 1;
            if (c.isReported) {
                allSuppressedMoves = false;
                break;
            }

            auto it = std::find_if(nextTracks.begin(), nextTracks.end(),
                                   [&](const TrackState& t) { return t.id == c.id; });
            if (it == nextTracks.end() ||
                it->suppressStreak < std::max(1, m_reportMultiStepBackMinStreak)) {
                allSuppressedMoves = false;
                break;
            }
        }

        if (allSuppressedMoves && activeMoveCount > 0) {
            for (auto& c : outputContacts) {
                if (c.state == TouchStateUp) {
                    continue;
                }
                auto it = std::find_if(nextTracks.begin(), nextTracks.end(),
                                       [&](const TrackState& t) { return t.id == c.id; });
                if (it == nextTracks.end()) {
                    continue;
                }

                c.x = it->reportX;
                c.y = it->reportY;
                c.isReported = true;
                c.reportEvent = TouchReportMove;
                c.reportFlags |= (kReportFlagStepBack | kReportFlagReported | kReportFlagStart);
                c.reportFlags &= ~kReportFlagSuppressed;
                c.debugFlags |= 0x20;

                it->reportActive = true;
                it->reportX = c.x;
                it->reportY = c.y;
                it->reportFlags |= (kReportFlagStepBack | kReportFlagReported | kReportFlagStart);
                it->reportFlags &= ~kReportFlagSuppressed;
            }
        }
    }

    if (!m_recentReleased.empty() && !nextTracks.empty()) {
        m_recentReleased.erase(
            std::remove_if(m_recentReleased.begin(), m_recentReleased.end(),
                           [&](const RecentReleasedTrack& r) {
                               return std::any_of(nextTracks.begin(), nextTracks.end(),
                                                  [&](const TrackState& t) { return t.id == r.id; });
                           }),
            m_recentReleased.end());
    }

    frame.contacts = std::move(outputContacts);
    m_tracks = std::move(nextTracks);
    return true;
}

void TouchTracker::DrawConfigUI() {
    ImGui::TextWrapped("IDT/TS/TE/TouchReport-like touch tracking and reporting.");
    ImGui::Checkbox("Use Hungarian Assignment", &m_useHungarian);
    ImGui::SliderFloat("Max Track Distance", &m_maxTrackDistance, 1.0f, 20.0f, "%.1f");
    ImGui::SliderFloat("Always Match Distance", &m_alwaysMatchDistance, 0.5f, 6.0f, "%.2f");
    ImGui::SliderFloat("Edge Track Boost", &m_edgeTrackBoost, 1.0f, 3.0f, "%.2f");
    ImGui::SliderFloat("Acc Threshold Boost", &m_accThresholdBoost, 1.0f, 6.0f, "%.2f");
    ImGui::SliderFloat("Acc Boost Size(mm)", &m_accBoostSizeMm, 0.5f, 4.0f, "%.2f");
    ImGui::SliderFloat("Prediction Scale", &m_predictionScale, 0.0f, 2.0f, "%.2f");
    ImGui::SliderInt("LiftOff Hold Frames", &m_liftOffHoldFrames, 0, 4);
    ImGui::SliderInt("TouchDown Debounce Frames", &m_touchDownDebounceFrames, 0, 3);
    ImGui::Checkbox("Dynamic Debounce", &m_dynamicDebounceEnabled);
    if (m_dynamicDebounceEnabled) {
        ImGui::SliderInt("  TouchDown Max Extra", &m_touchDownDebounceMaxExtra, 0, 4);
        ImGui::SliderInt("  TouchDown Weak Sig", &m_touchDownWeakSignalThreshold, 20, 800);
        ImGui::SliderFloat("  TouchDown Small Size", &m_touchDownSmallSizeThresholdMm, 0.5f, 4.0f, "%.2f");
        ImGui::Checkbox("  TouchDown Reject Weak", &m_touchDownRejectEnabled);
        if (m_touchDownRejectEnabled) {
            ImGui::SliderInt("    Reject Min Signal", &m_touchDownRejectMinSignal, 1, 400);
            ImGui::SliderFloat("    Reject Min Size", &m_touchDownRejectMinSizeMm, 0.3f, 3.0f, "%.2f");
            ImGui::SliderInt("    Edge Reject Min Sig", &m_touchDownEdgeRejectMinSignal, 1, 600);
        }
        ImGui::SliderInt("  LiftOff Base Frames", &m_liftOffDebounceBaseFrames, 0, 3);
        ImGui::SliderInt("  LiftOff Strong Sig", &m_liftOffStrongSignalThreshold, 20, 1200);
        ImGui::SliderFloat("  LiftOff Large Size", &m_liftOffLargeSizeThresholdMm, 0.5f, 6.0f, "%.2f");
        ImGui::SliderInt("  LiftOff Debounce Max", &m_liftOffDebounceMaxFrames, 0, 6);
    }
    ImGui::SliderFloat("Fallback Size(mm)", &m_fallbackSizeMm, 0.1f, 4.0f, "%.2f");
    ImGui::SliderFloat("Size Area Scale", &m_sizeAreaScale, 0.05f, 1.00f, "%.2f");
    ImGui::SliderFloat("Size Signal Scale", &m_sizeSignalScale, 0.05f, 1.20f, "%.2f");
    ImGui::Checkbox("Rx Ghost Filter (approx)", &m_rxGhostFilterEnabled);
    if (m_rxGhostFilterEnabled) {
        ImGui::SliderInt("  Rx Ghost Line Delta", &m_rxGhostLineDelta, 0, 3);
        ImGui::SliderFloat("  Rx Ghost Weak Ratio", &m_rxGhostWeakRatio, 0.10f, 0.95f, "%.2f");
        ImGui::Checkbox("  Rx Ghost Only New", &m_rxGhostOnlyNew);
    }
    ImGui::Checkbox("Enable Report Filter", &m_enableReportFilter);
    ImGui::Checkbox("Report During Down Debounce", &m_reportDuringDownDebounce);
    ImGui::Checkbox("Report Start Gate", &m_reportStartGateEnabled);
    if (m_reportStartGateEnabled) {
        ImGui::SliderInt("  Start Min Signal", &m_reportStartMinSignal, 1, 500);
        ImGui::SliderFloat("  Start Min Size", &m_reportStartMinSizeMm, 0.3f, 4.0f, "%.2f");
        ImGui::SliderInt("  Start Stable Frames", &m_reportStartStableFrames, 1, 4);
        ImGui::Checkbox("  Suppress Edge First", &m_reportSuppressEdgeFirst);
        if (m_reportSuppressEdgeFirst) {
            ImGui::SliderInt("    Edge Start Min Sig", &m_reportEdgeStartMinSignal, 1, 800);
        }
    }
    ImGui::Checkbox("Report Weak Touch Suppress", &m_reportWeakTouchSuppressEnabled);
    if (m_reportWeakTouchSuppressEnabled) {
        ImGui::SliderInt("  Weak Min Signal", &m_reportWeakTouchMinSignal, 1, 600);
        ImGui::SliderFloat("  Weak Min Size", &m_reportWeakTouchMinSizeMm, 0.2f, 3.0f, "%.2f");
        ImGui::SliderFloat("  Weak Min Move", &m_reportWeakTouchMinMove, 0.05f, 3.0f, "%.2f");
    }
    ImGui::Checkbox("Report Edge Weak Suppress", &m_reportEdgeWeakSuppressEnabled);
    if (m_reportEdgeWeakSuppressEnabled) {
        ImGui::SliderInt("  Edge Weak Sig Th", &m_reportEdgeWeakSignalThreshold, 1, 800);
        ImGui::SliderFloat("  Edge Weak Size Th", &m_reportEdgeWeakSizeThresholdMm, 0.2f, 3.0f, "%.2f");
    }
    ImGui::Checkbox("Report History Replay", &m_enableReportHistoryReplay);
    ImGui::Checkbox("Report Multi StepBack", &m_enableReportMultiStepBack);
    if (m_enableReportMultiStepBack) {
        ImGui::SliderInt("  Multi StepBack Min Streak", &m_reportMultiStepBackMinStreak, 1, 16);
    }
    ImGui::Checkbox("Emit Unreported LiftOff", &m_emitUnreportedLiftOff);
    ImGui::SliderInt("Report Suppress Debounce", &m_reportSuppressDebounceFrames, 0, 4);
    if (m_reportSuppressDebounceFrames > 0) {
        ImGui::Checkbox("  Hold Prev Coordinate", &m_reportHoldPrevCoordinate);
    }
    ImGui::Checkbox("Report Edge Jitter Hold", &m_reportEdgeJitterHoldEnabled);
    if (m_reportEdgeJitterHoldEnabled) {
        ImGui::SliderInt("  Edge Jitter Sig Th", &m_reportEdgeJitterSignalThreshold, 1, 800);
        ImGui::SliderInt("  Edge Jitter Max Age", &m_reportEdgeJitterMaxAge, 1, 8);
    }
    ImGui::Checkbox("Report Coordinate Filter", &m_reportCoordinateFilterEnabled);
    if (m_reportCoordinateFilterEnabled) {
        ImGui::SliderFloat("  Coord Filter Dist Th", &m_reportCoordinateFilterDistThreshold, 0.2f, 4.0f, "%.2f");
        ImGui::SliderFloat("  Coord Filter Alpha S", &m_reportCoordinateFilterAlphaSmall, 0.05f, 1.0f, "%.2f");
        ImGui::SliderFloat("  Coord Filter Alpha L", &m_reportCoordinateFilterAlphaLarge, 0.05f, 1.0f, "%.2f");
    }
    if (m_enableReportFilter) {
        ImGui::SliderFloat("Report Move Threshold", &m_reportMoveThreshold, 0.05f, 3.00f, "%.2f");
    }

    ImGui::Separator();
    ImGui::TextWrapped("TS/TE Continuity");
    ImGui::SliderInt("TE Lost Grace Frames", &m_teLostGraceFrames, 0, 6);
    ImGui::Checkbox("TE Recover Recent Up", &m_teRecoverEnabled);
    if (m_teRecoverEnabled) {
        ImGui::SliderInt("  TE Recover Window", &m_teRecoverWindowFrames, 0, 6);
        ImGui::SliderFloat("  TE Recover Distance", &m_teRecoverDistance, 0.5f, 10.0f, "%.2f");
        ImGui::Checkbox("  TE Recover Direction Constraint", &m_teRecoverDirectionConstraint);
        if (m_teRecoverDirectionConstraint) {
            ImGui::SliderFloat("    TE Min Release Speed", &m_teRecoverMinReleaseSpeed, 0.05f, 4.0f, "%.2f");
            ImGui::SliderFloat("    TE Min Recover Cos", &m_teRecoverMinCosine, -1.0f, 1.0f, "%.2f");
        }
    }

    ImGui::Separator();
    ImGui::TextWrapped("Stylus interop parameters were moved to Stylus Pipeline Panel.");
}

void TouchTracker::DrawStylusInteropConfigUI() {
    ImGui::TextWrapped("Touch-side stylus suppression and StylusAFT gating.");

    ImGui::Separator();
    ImGui::TextWrapped("Stylus Suppression (ASA/StylusRecheck mirror)");
    ImGui::Checkbox("Stylus Global Suppress", &m_stylusSuppressGlobalEnabled);
    ImGui::Checkbox("Stylus Local Suppress", &m_stylusSuppressLocalEnabled);
    if (m_stylusSuppressLocalEnabled) {
        ImGui::SliderFloat("  Stylus Local Dist", &m_stylusSuppressLocalDistance, 0.5f, 10.0f, "%.2f");
        ImGui::SliderInt("  Stylus Pen Peak Th", &m_stylusSuppressPenPeakThreshold, 100, 4000);
        ImGui::SliderInt("  Stylus Keep Touch Sig", &m_stylusSuppressTouchSignalKeep, 100, 12000);
        ImGui::SliderInt("  Stylus Keep Touch Area", &m_stylusSuppressTouchAreaKeep, 1, 80);
    }

    ImGui::Separator();
    ImGui::TextWrapped("StylusAFT Suppression (age/domain-lite)");
    ImGui::Checkbox("StylusAFT Enabled", &m_stylusAftEnabled);
    if (m_stylusAftEnabled) {
        ImGui::SliderInt("  StylusAFT Recent Frames", &m_stylusAftRecentFrames, 1, 80);
        ImGui::SliderFloat("  StylusAFT Radius", &m_stylusAftRadius, 0.5f, 12.0f, "%.2f");
        ImGui::SliderInt("  StylusAFT Debounce Frames", &m_stylusAftDebounceFrames, 1, 8);
        ImGui::SliderInt("  StylusAFT Weak Signal", &m_stylusAftWeakSignalThreshold, 1, 1200);
        ImGui::SliderFloat("  StylusAFT Weak Size", &m_stylusAftWeakSizeThresholdMm, 0.2f, 4.0f, "%.2f");
        ImGui::SliderInt("  StylusAFT Suppress Frames", &m_stylusAftSuppressFrames, 1, 120);
        ImGui::SliderInt("  StylusAFT Palm Suppress", &m_stylusAftPalmSuppressFrames, 1, 180);
        ImGui::SliderInt("  StylusAFT Palm Area", &m_stylusAftPalmAreaThreshold, 1, 120);
        ImGui::SliderFloat("  StylusAFT Palm Size", &m_stylusAftPalmSizeThresholdMm, 0.5f, 8.0f, "%.2f");
    }
}

void TouchTracker::SaveConfig(std::ostream& out) const {
    IFrameProcessor::SaveConfig(out);
    out << "UseHungarian=" << (m_useHungarian ? "1" : "0") << "\n";
    out << "MaxTrackDistance=" << m_maxTrackDistance << "\n";
    out << "AlwaysMatchDistance=" << m_alwaysMatchDistance << "\n";
    out << "EdgeTrackBoost=" << m_edgeTrackBoost << "\n";
    out << "AccThresholdBoost=" << m_accThresholdBoost << "\n";
    out << "AccBoostSizeMm=" << m_accBoostSizeMm << "\n";
    out << "PredictionScale=" << m_predictionScale << "\n";
    out << "LiftOffHoldFrames=" << m_liftOffHoldFrames << "\n";
    out << "TouchDownDebounceFrames=" << m_touchDownDebounceFrames << "\n";
    out << "DynamicDebounceEnabled=" << (m_dynamicDebounceEnabled ? "1" : "0") << "\n";
    out << "TouchDownDebounceMaxExtra=" << m_touchDownDebounceMaxExtra << "\n";
    out << "TouchDownWeakSignalThreshold=" << m_touchDownWeakSignalThreshold << "\n";
    out << "TouchDownSmallSizeThresholdMm=" << m_touchDownSmallSizeThresholdMm << "\n";
    out << "TouchDownRejectEnabled=" << (m_touchDownRejectEnabled ? "1" : "0") << "\n";
    out << "TouchDownRejectMinSignal=" << m_touchDownRejectMinSignal << "\n";
    out << "TouchDownRejectMinSizeMm=" << m_touchDownRejectMinSizeMm << "\n";
    out << "TouchDownEdgeRejectMinSignal=" << m_touchDownEdgeRejectMinSignal << "\n";
    out << "LiftOffDebounceBaseFrames=" << m_liftOffDebounceBaseFrames << "\n";
    out << "LiftOffStrongSignalThreshold=" << m_liftOffStrongSignalThreshold << "\n";
    out << "LiftOffLargeSizeThresholdMm=" << m_liftOffLargeSizeThresholdMm << "\n";
    out << "LiftOffDebounceMaxFrames=" << m_liftOffDebounceMaxFrames << "\n";
    out << "FallbackSizeMm=" << m_fallbackSizeMm << "\n";
    out << "SizeAreaScale=" << m_sizeAreaScale << "\n";
    out << "SizeSignalScale=" << m_sizeSignalScale << "\n";
    out << "RxGhostFilterEnabled=" << (m_rxGhostFilterEnabled ? "1" : "0") << "\n";
    out << "RxGhostLineDelta=" << m_rxGhostLineDelta << "\n";
    out << "RxGhostWeakRatio=" << m_rxGhostWeakRatio << "\n";
    out << "RxGhostOnlyNew=" << (m_rxGhostOnlyNew ? "1" : "0") << "\n";
    out << "EnableReportFilter=" << (m_enableReportFilter ? "1" : "0") << "\n";
    out << "ReportDuringDownDebounce=" << (m_reportDuringDownDebounce ? "1" : "0") << "\n";
    out << "ReportStartGateEnabled=" << (m_reportStartGateEnabled ? "1" : "0") << "\n";
    out << "ReportStartMinSignal=" << m_reportStartMinSignal << "\n";
    out << "ReportStartMinSizeMm=" << m_reportStartMinSizeMm << "\n";
    out << "ReportStartStableFrames=" << m_reportStartStableFrames << "\n";
    out << "ReportSuppressEdgeFirst=" << (m_reportSuppressEdgeFirst ? "1" : "0") << "\n";
    out << "ReportEdgeStartMinSignal=" << m_reportEdgeStartMinSignal << "\n";
    out << "ReportWeakTouchSuppressEnabled=" << (m_reportWeakTouchSuppressEnabled ? "1" : "0") << "\n";
    out << "ReportWeakTouchMinSignal=" << m_reportWeakTouchMinSignal << "\n";
    out << "ReportWeakTouchMinSizeMm=" << m_reportWeakTouchMinSizeMm << "\n";
    out << "ReportWeakTouchMinMove=" << m_reportWeakTouchMinMove << "\n";
    out << "ReportEdgeWeakSuppressEnabled=" << (m_reportEdgeWeakSuppressEnabled ? "1" : "0") << "\n";
    out << "ReportEdgeWeakSignalThreshold=" << m_reportEdgeWeakSignalThreshold << "\n";
    out << "ReportEdgeWeakSizeThresholdMm=" << m_reportEdgeWeakSizeThresholdMm << "\n";
    out << "EnableReportHistoryReplay=" << (m_enableReportHistoryReplay ? "1" : "0") << "\n";
    out << "EnableReportMultiStepBack=" << (m_enableReportMultiStepBack ? "1" : "0") << "\n";
    out << "ReportMultiStepBackMinStreak=" << m_reportMultiStepBackMinStreak << "\n";
    out << "EmitUnreportedLiftOff=" << (m_emitUnreportedLiftOff ? "1" : "0") << "\n";
    out << "ReportSuppressDebounceFrames=" << m_reportSuppressDebounceFrames << "\n";
    out << "ReportHoldPrevCoordinate=" << (m_reportHoldPrevCoordinate ? "1" : "0") << "\n";
    out << "ReportEdgeJitterHoldEnabled=" << (m_reportEdgeJitterHoldEnabled ? "1" : "0") << "\n";
    out << "ReportEdgeJitterSignalThreshold=" << m_reportEdgeJitterSignalThreshold << "\n";
    out << "ReportEdgeJitterMaxAge=" << m_reportEdgeJitterMaxAge << "\n";
    out << "ReportCoordinateFilterEnabled=" << (m_reportCoordinateFilterEnabled ? "1" : "0") << "\n";
    out << "ReportCoordinateFilterDistThreshold=" << m_reportCoordinateFilterDistThreshold << "\n";
    out << "ReportCoordinateFilterAlphaSmall=" << m_reportCoordinateFilterAlphaSmall << "\n";
    out << "ReportCoordinateFilterAlphaLarge=" << m_reportCoordinateFilterAlphaLarge << "\n";
    out << "ReportMoveThreshold=" << m_reportMoveThreshold << "\n";
    out << "TELostGraceFrames=" << m_teLostGraceFrames << "\n";
    out << "TERecoverEnabled=" << (m_teRecoverEnabled ? "1" : "0") << "\n";
    out << "TERecoverWindowFrames=" << m_teRecoverWindowFrames << "\n";
    out << "TERecoverDistance=" << m_teRecoverDistance << "\n";
    out << "TERecoverDirectionConstraint=" << (m_teRecoverDirectionConstraint ? "1" : "0") << "\n";
    out << "TERecoverMinReleaseSpeed=" << m_teRecoverMinReleaseSpeed << "\n";
    out << "TERecoverMinCosine=" << m_teRecoverMinCosine << "\n";
    out << "StylusSuppressGlobalEnabled=" << (m_stylusSuppressGlobalEnabled ? "1" : "0") << "\n";
    out << "StylusSuppressLocalEnabled=" << (m_stylusSuppressLocalEnabled ? "1" : "0") << "\n";
    out << "StylusSuppressLocalDistance=" << m_stylusSuppressLocalDistance << "\n";
    out << "StylusSuppressPenPeakThreshold=" << m_stylusSuppressPenPeakThreshold << "\n";
    out << "StylusSuppressTouchSignalKeep=" << m_stylusSuppressTouchSignalKeep << "\n";
    out << "StylusSuppressTouchAreaKeep=" << m_stylusSuppressTouchAreaKeep << "\n";
    out << "StylusAftEnabled=" << (m_stylusAftEnabled ? "1" : "0") << "\n";
    out << "StylusAftRecentFrames=" << m_stylusAftRecentFrames << "\n";
    out << "StylusAftRadius=" << m_stylusAftRadius << "\n";
    out << "StylusAftDebounceFrames=" << m_stylusAftDebounceFrames << "\n";
    out << "StylusAftWeakSignalThreshold=" << m_stylusAftWeakSignalThreshold << "\n";
    out << "StylusAftWeakSizeThresholdMm=" << m_stylusAftWeakSizeThresholdMm << "\n";
    out << "StylusAftSuppressFrames=" << m_stylusAftSuppressFrames << "\n";
    out << "StylusAftPalmSuppressFrames=" << m_stylusAftPalmSuppressFrames << "\n";
    out << "StylusAftPalmAreaThreshold=" << m_stylusAftPalmAreaThreshold << "\n";
    out << "StylusAftPalmSizeThresholdMm=" << m_stylusAftPalmSizeThresholdMm << "\n";
}

void TouchTracker::LoadConfig(const std::string& key, const std::string& value) {
    IFrameProcessor::LoadConfig(key, value);
    if (key == "UseHungarian") {
        m_useHungarian = (value == "1" || value == "true");
    } else if (key == "MaxTrackDistance") {
        m_maxTrackDistance = std::stof(value);
    } else if (key == "AlwaysMatchDistance") {
        m_alwaysMatchDistance = std::stof(value);
    } else if (key == "EdgeTrackBoost") {
        m_edgeTrackBoost = std::stof(value);
    } else if (key == "AccThresholdBoost") {
        m_accThresholdBoost = std::stof(value);
    } else if (key == "AccBoostSizeMm") {
        m_accBoostSizeMm = std::stof(value);
    } else if (key == "PredictionScale") {
        m_predictionScale = std::stof(value);
    } else if (key == "LiftOffHoldFrames") {
        m_liftOffHoldFrames = std::stoi(value);
    } else if (key == "TouchDownDebounceFrames") {
        m_touchDownDebounceFrames = std::stoi(value);
    } else if (key == "DynamicDebounceEnabled") {
        m_dynamicDebounceEnabled = (value == "1" || value == "true");
    } else if (key == "TouchDownDebounceMaxExtra") {
        m_touchDownDebounceMaxExtra = std::stoi(value);
    } else if (key == "TouchDownWeakSignalThreshold") {
        m_touchDownWeakSignalThreshold = std::stoi(value);
    } else if (key == "TouchDownSmallSizeThresholdMm") {
        m_touchDownSmallSizeThresholdMm = std::stof(value);
    } else if (key == "TouchDownRejectEnabled") {
        m_touchDownRejectEnabled = (value == "1" || value == "true");
    } else if (key == "TouchDownRejectMinSignal") {
        m_touchDownRejectMinSignal = std::stoi(value);
    } else if (key == "TouchDownRejectMinSizeMm") {
        m_touchDownRejectMinSizeMm = std::stof(value);
    } else if (key == "TouchDownEdgeRejectMinSignal") {
        m_touchDownEdgeRejectMinSignal = std::stoi(value);
    } else if (key == "LiftOffDebounceBaseFrames") {
        m_liftOffDebounceBaseFrames = std::stoi(value);
    } else if (key == "LiftOffStrongSignalThreshold") {
        m_liftOffStrongSignalThreshold = std::stoi(value);
    } else if (key == "LiftOffLargeSizeThresholdMm") {
        m_liftOffLargeSizeThresholdMm = std::stof(value);
    } else if (key == "LiftOffDebounceMaxFrames") {
        m_liftOffDebounceMaxFrames = std::stoi(value);
    } else if (key == "FallbackSizeMm") {
        m_fallbackSizeMm = std::stof(value);
    } else if (key == "SizeAreaScale") {
        m_sizeAreaScale = std::stof(value);
    } else if (key == "SizeSignalScale") {
        m_sizeSignalScale = std::stof(value);
    } else if (key == "RxGhostFilterEnabled") {
        m_rxGhostFilterEnabled = (value == "1" || value == "true");
    } else if (key == "RxGhostLineDelta") {
        m_rxGhostLineDelta = std::stoi(value);
    } else if (key == "RxGhostWeakRatio") {
        m_rxGhostWeakRatio = std::stof(value);
    } else if (key == "RxGhostOnlyNew") {
        m_rxGhostOnlyNew = (value == "1" || value == "true");
    } else if (key == "EnableReportFilter") {
        m_enableReportFilter = (value == "1" || value == "true");
    } else if (key == "ReportDuringDownDebounce") {
        m_reportDuringDownDebounce = (value == "1" || value == "true");
    } else if (key == "ReportStartGateEnabled") {
        m_reportStartGateEnabled = (value == "1" || value == "true");
    } else if (key == "ReportStartMinSignal") {
        m_reportStartMinSignal = std::stoi(value);
    } else if (key == "ReportStartMinSizeMm") {
        m_reportStartMinSizeMm = std::stof(value);
    } else if (key == "ReportStartStableFrames") {
        m_reportStartStableFrames = std::max(1, std::stoi(value));
    } else if (key == "ReportSuppressEdgeFirst") {
        m_reportSuppressEdgeFirst = (value == "1" || value == "true");
    } else if (key == "ReportEdgeStartMinSignal") {
        m_reportEdgeStartMinSignal = std::stoi(value);
    } else if (key == "ReportWeakTouchSuppressEnabled") {
        m_reportWeakTouchSuppressEnabled = (value == "1" || value == "true");
    } else if (key == "ReportWeakTouchMinSignal") {
        m_reportWeakTouchMinSignal = std::max(1, std::stoi(value));
    } else if (key == "ReportWeakTouchMinSizeMm") {
        m_reportWeakTouchMinSizeMm = std::max(0.0f, std::stof(value));
    } else if (key == "ReportWeakTouchMinMove") {
        m_reportWeakTouchMinMove = std::max(0.0f, std::stof(value));
    } else if (key == "ReportEdgeWeakSuppressEnabled") {
        m_reportEdgeWeakSuppressEnabled = (value == "1" || value == "true");
    } else if (key == "ReportEdgeWeakSignalThreshold") {
        m_reportEdgeWeakSignalThreshold = std::max(1, std::stoi(value));
    } else if (key == "ReportEdgeWeakSizeThresholdMm") {
        m_reportEdgeWeakSizeThresholdMm = std::max(0.0f, std::stof(value));
    } else if (key == "EnableReportHistoryReplay") {
        m_enableReportHistoryReplay = (value == "1" || value == "true");
    } else if (key == "EnableReportMultiStepBack") {
        m_enableReportMultiStepBack = (value == "1" || value == "true");
    } else if (key == "ReportMultiStepBackMinStreak") {
        m_reportMultiStepBackMinStreak = std::max(1, std::stoi(value));
    } else if (key == "EmitUnreportedLiftOff") {
        m_emitUnreportedLiftOff = (value == "1" || value == "true");
    } else if (key == "ReportSuppressDebounceFrames") {
        m_reportSuppressDebounceFrames = std::max(0, std::stoi(value));
    } else if (key == "ReportHoldPrevCoordinate") {
        m_reportHoldPrevCoordinate = (value == "1" || value == "true");
    } else if (key == "ReportEdgeJitterHoldEnabled") {
        m_reportEdgeJitterHoldEnabled = (value == "1" || value == "true");
    } else if (key == "ReportEdgeJitterSignalThreshold") {
        m_reportEdgeJitterSignalThreshold = std::max(1, std::stoi(value));
    } else if (key == "ReportEdgeJitterMaxAge") {
        m_reportEdgeJitterMaxAge = std::max(1, std::stoi(value));
    } else if (key == "ReportCoordinateFilterEnabled") {
        m_reportCoordinateFilterEnabled = (value == "1" || value == "true");
    } else if (key == "ReportCoordinateFilterDistThreshold") {
        m_reportCoordinateFilterDistThreshold = std::max(0.0f, std::stof(value));
    } else if (key == "ReportCoordinateFilterAlphaSmall") {
        m_reportCoordinateFilterAlphaSmall = std::clamp(std::stof(value), 0.0f, 1.0f);
    } else if (key == "ReportCoordinateFilterAlphaLarge") {
        m_reportCoordinateFilterAlphaLarge = std::clamp(std::stof(value), 0.0f, 1.0f);
    } else if (key == "ReportMoveThreshold") {
        m_reportMoveThreshold = std::stof(value);
    } else if (key == "TELostGraceFrames") {
        m_teLostGraceFrames = std::max(0, std::stoi(value));
    } else if (key == "TERecoverEnabled") {
        m_teRecoverEnabled = (value == "1" || value == "true");
    } else if (key == "TERecoverWindowFrames") {
        m_teRecoverWindowFrames = std::max(0, std::stoi(value));
    } else if (key == "TERecoverDistance") {
        m_teRecoverDistance = std::max(0.1f, std::stof(value));
    } else if (key == "TERecoverDirectionConstraint") {
        m_teRecoverDirectionConstraint = (value == "1" || value == "true");
    } else if (key == "TERecoverMinReleaseSpeed") {
        m_teRecoverMinReleaseSpeed = std::max(0.0f, std::stof(value));
    } else if (key == "TERecoverMinCosine") {
        m_teRecoverMinCosine = std::clamp(std::stof(value), -1.0f, 1.0f);
    } else if (key == "StylusSuppressGlobalEnabled") {
        m_stylusSuppressGlobalEnabled = (value == "1" || value == "true");
    } else if (key == "StylusSuppressLocalEnabled") {
        m_stylusSuppressLocalEnabled = (value == "1" || value == "true");
    } else if (key == "StylusSuppressLocalDistance") {
        m_stylusSuppressLocalDistance = std::max(0.1f, std::stof(value));
    } else if (key == "StylusSuppressPenPeakThreshold") {
        m_stylusSuppressPenPeakThreshold = std::max(1, std::stoi(value));
    } else if (key == "StylusSuppressTouchSignalKeep") {
        m_stylusSuppressTouchSignalKeep = std::max(1, std::stoi(value));
    } else if (key == "StylusSuppressTouchAreaKeep") {
        m_stylusSuppressTouchAreaKeep = std::max(1, std::stoi(value));
    } else if (key == "StylusAftEnabled") {
        m_stylusAftEnabled = (value == "1" || value == "true");
    } else if (key == "StylusAftRecentFrames") {
        m_stylusAftRecentFrames = std::clamp(std::stoi(value), 1, 240);
    } else if (key == "StylusAftRadius") {
        m_stylusAftRadius = std::max(0.1f, std::stof(value));
    } else if (key == "StylusAftDebounceFrames") {
        m_stylusAftDebounceFrames = std::clamp(std::stoi(value), 1, 32);
    } else if (key == "StylusAftWeakSignalThreshold") {
        m_stylusAftWeakSignalThreshold = std::max(1, std::stoi(value));
    } else if (key == "StylusAftWeakSizeThresholdMm") {
        m_stylusAftWeakSizeThresholdMm = std::max(0.1f, std::stof(value));
    } else if (key == "StylusAftSuppressFrames") {
        m_stylusAftSuppressFrames = std::max(1, std::stoi(value));
    } else if (key == "StylusAftPalmSuppressFrames") {
        m_stylusAftPalmSuppressFrames = std::max(1, std::stoi(value));
    } else if (key == "StylusAftPalmAreaThreshold") {
        m_stylusAftPalmAreaThreshold = std::max(1, std::stoi(value));
    } else if (key == "StylusAftPalmSizeThresholdMm") {
        m_stylusAftPalmSizeThresholdMm = std::max(0.1f, std::stof(value));
    }
}

} // namespace Engine
