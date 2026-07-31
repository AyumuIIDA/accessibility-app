#include "HandInput/Measurements/gesture_candidate_window_history.h"
#include "HandInput/Measurements/hand_topology_history.h"
#include "HandInput/Measurements/hand_unified_feature_frame.h"
#include "HandInput/Measurements/multi_hand_measurement_stage.h"
#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using ryoiki::hand_perception::HandLandmarkResult;
using ryoiki::hand_perception::HandPerceptionResult;
using ryoiki::hand_perception::Landmark3f;
namespace measurements = ryoiki::hand_input::measurements;

void require(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error{message};
    }
}

void requireNear(const double actual, const double expected, const double tolerance, const char* message)
{
    require(std::isfinite(actual), "Expected a finite value.");
    require(std::abs(actual - expected) <= tolerance, message);
}

// Same synthetic fully-open-hand construction as
// hand_unified_feature_frame_tests.cpp, extended with a wrist translation
// (dx, dy) so successive observations can carry controlled palm velocity.
constexpr std::array<Landmark3f, 5> kFingerBaseOffsets{{
    {-30.0F, -20.0F, 0.0F},
    {40.0F, -70.0F, 0.0F},
    {0.0F, -100.0F, 0.0F},
    {-40.0F, -70.0F, 0.0F},
    {-70.0F, -40.0F, 0.0F}}};
constexpr std::array<std::size_t, 5> kBaseIndices{1, 5, 9, 13, 17};
constexpr std::array<std::size_t, 5> kTipIndices{4, 8, 12, 16, 20};
constexpr Landmark3f kWrist{300.0F, 450.0F, 0.0F};
// Tip = wrist + baseOffset * kOpenMultiplier is a fully open/extended finger
// (see AnalyzeFingerPose's 1.05..1.70 straightness ratio mapping).
constexpr float kOpenMultiplier = 2.0F;

std::array<Landmark3f, 21> makeOpenHandLandmarks(const float dx, const float dy)
{
    std::array<Landmark3f, 21> landmarks{};
    const Landmark3f wrist{kWrist.x + dx, kWrist.y + dy, 0.0F};
    landmarks[0] = wrist;
    for (std::size_t finger = 0; finger < 5; ++finger)
    {
        const Landmark3f base{
            wrist.x + kFingerBaseOffsets[finger].x, wrist.y + kFingerBaseOffsets[finger].y, 0.0F};
        const Landmark3f tip{
            wrist.x + kFingerBaseOffsets[finger].x * kOpenMultiplier,
            wrist.y + kFingerBaseOffsets[finger].y * kOpenMultiplier,
            0.0F};
        landmarks[kBaseIndices[finger]] = base;
        landmarks[kTipIndices[finger]] = tip;
        for (std::size_t joint = 1; joint <= 2; ++joint)
        {
            const float f = static_cast<float>(joint) / 3.0F;
            landmarks[kBaseIndices[finger] + joint] = {
                base.x + (tip.x - base.x) * f, base.y + (tip.y - base.y) * f, 0.0F};
        }
    }
    return landmarks;
}

measurements::UnifiedFeatureObservation makeObservation(
    const std::uint64_t timestampUs,
    const float dx = 0.0F,
    const float dy = 0.0F,
    const float handedness = 0.8F)
{
    measurements::UnifiedFeatureObservation observation{};
    observation.timestampUs = timestampUs;
    observation.imageLandmarks = makeOpenHandLandmarks(dx, dy);
    observation.handedness = handedness;
    observation.boundingBoxArea = 0.0F;
    return observation;
}

HandLandmarkResult makeHand(
    const float dx = 0.0F,
    const float dy = 0.0F,
    const float confidence = 0.9F,
    const float handedness = 0.8F)
{
    HandLandmarkResult hand{};
    hand.detected = true;
    hand.confidence = confidence;
    hand.handedness = handedness;
    hand.landmarks = makeOpenHandLandmarks(dx, dy);
    return hand;
}

// ---------------------------------------------------------------------
// findActiveSegment (extends hand_unified_feature_frame).
// ---------------------------------------------------------------------

measurements::UnifiedFeatureFrame frameWithVelocity(
    const double timeOffsetMs, const float centerX, const float palmVelocity)
{
    measurements::UnifiedFeatureFrame frame{};
    frame.timeOffsetMs = timeOffsetMs;
    frame.centerX = centerX;
    frame.centerY = 0.0F;
    frame.palmVelocity = palmVelocity;
    return frame;
}

void testFindActiveSegmentNoActiveMotionReturnsInvalid()
{
    std::vector<measurements::UnifiedFeatureFrame> frames;
    for (int i = 0; i < 10; ++i)
    {
        frames.push_back(frameWithVelocity(i * 100.0, static_cast<float>(i) * 0.01F, 0.01F));
    }
    const auto segment = measurements::findActiveSegment(frames);
    require(!segment.valid, "A stationary sequence must not report an active segment.");
}

void testFindActiveSegmentRequiresMinimumActiveFrames()
{
    std::vector<measurements::UnifiedFeatureFrame> frames;
    for (int i = 0; i < 10; ++i)
    {
        // Only two frames (indices 4, 5) cross the threshold: below the
        // PR's minimum of 3, so the segment must still be invalid.
        const float velocity = (i == 4 || i == 5) ? 0.5F : 0.0F;
        frames.push_back(frameWithVelocity(i * 100.0, 0.0F, velocity));
    }
    const auto segment = measurements::findActiveSegment(frames);
    require(!segment.valid, "Fewer than 3 above-threshold frames must not form an active segment.");
}

void testFindActiveSegmentBoundaryAndPadding()
{
    // Frames 4..6 (inclusive) are the only above-threshold ones amid 15
    // otherwise-stationary frames. The active segment must be
    // [4-2, 6+2] = [2, 8], not the whole sequence and not just [4,6].
    std::vector<measurements::UnifiedFeatureFrame> frames;
    for (int i = 0; i < 15; ++i)
    {
        const bool active = i >= 4 && i <= 6;
        const float velocity = active ? 0.5F : 0.01F;
        // Only the active frames actually move; padding frames keep the
        // same center as their neighbor so the path length is attributable
        // to the active stretch.
        const float centerX = active ? static_cast<float>(i) * 0.2F : static_cast<float>(3) * 0.2F;
        frames.push_back(frameWithVelocity(i * 50.0, centerX, velocity));
    }
    const auto segment = measurements::findActiveSegment(frames);
    require(segment.valid, "A qualifying 3-frame active run must produce a valid segment.");
    require(segment.startIndex == 2, "Active segment start did not apply the expected 2-frame padding.");
    require(segment.endIndex == 8, "Active segment end did not apply the expected 2-frame padding.");
    requireNear(segment.startTimeOffsetMs, frames[2].timeOffsetMs, 1.0e-9,
        "Active segment start time did not match the padded start frame.");
    requireNear(segment.endTimeOffsetMs, frames[8].timeOffsetMs, 1.0e-9,
        "Active segment end time did not match the padded end frame.");
    require(segment.pathVelocity > 0.0F, "A segment with real translation must report positive path velocity.");
}

void testFindActiveSegmentPaddingClampsToSequenceBounds()
{
    // Active frames start at index 0, so the padding (index - 2) must clamp
    // to 0 rather than underflowing; likewise the tail must clamp to the
    // last valid index.
    std::vector<measurements::UnifiedFeatureFrame> frames;
    for (int i = 0; i < 6; ++i)
    {
        frames.push_back(frameWithVelocity(i * 50.0, static_cast<float>(i) * 0.2F, 0.5F));
    }
    const auto segment = measurements::findActiveSegment(frames);
    require(segment.valid, "An entirely active short sequence must produce a valid segment.");
    require(segment.startIndex == 0, "Padding must clamp to index 0, not underflow.");
    require(segment.endIndex == frames.size() - 1, "Padding must clamp to the last frame index.");
}

// ---------------------------------------------------------------------
// GestureCandidateWindowHistory: standalone lifecycle behavior.
// ---------------------------------------------------------------------

void testIndependentHistoriesDoNotShareState()
{
    measurements::GestureCandidateWindowHistory trackA;
    measurements::GestureCandidateWindowHistory trackB;
    trackA.push(makeObservation(0, 0.0F, 0.0F, 0.9F), true);
    require(trackA.size() == 1, "Track A did not record its own push.");
    require(trackB.size() == 0, "Track B unexpectedly observed track A's push.");
    trackB.push(makeObservation(0, 0.0F, 0.0F, 0.1F), true);
    trackB.push(makeObservation(50'000, 0.0F, 0.0F, 0.1F), true);
    require(trackA.size() == 1 && trackB.size() == 2,
        "Independent histories must not share internal state.");
}

void testTimeBasedEvictionKeepsWithinRollingWindow()
{
    measurements::GestureCandidateWindowHistory history;
    constexpr int kFrameCount = 60;
    constexpr std::uint64_t kFrameIntervalUs = 100'000; // 100 ms apart -> 5.9 s total span.
    for (int i = 0; i < kFrameCount; ++i)
    {
        history.push(makeObservation(static_cast<std::uint64_t>(i) * kFrameIntervalUs), true);
    }
    require(history.size() < static_cast<std::size_t>(kFrameCount),
        "Time-based pruning did not evict samples older than the 2600 ms rolling window.");
    require(history.size() > 0, "Time-based pruning must not evict the newest sample.");
}

void testCapacityEvictionBoundsMemoryWithoutTimeEviction()
{
    measurements::GestureCandidateWindowHistory history;
    // 1 ms apart: 250 samples span only 249 ms, far inside the 2600 ms
    // window, so only the fixed capacity bound can evict here.
    constexpr int kFrameCount = 250;
    for (int i = 0; i < kFrameCount; ++i)
    {
        history.push(makeObservation(static_cast<std::uint64_t>(i) * 1'000), true);
    }
    require(history.size() == measurements::kHandTopologyHistoryCapacity,
        "A dense push sequence within the time window must saturate at the fixed capacity.");
}

void testMissingObservationGraceWindowPreservesHistory()
{
    measurements::GestureCandidateWindowHistory history;
    history.push(makeObservation(0), true);
    history.push(makeObservation(50'000), true);
    require(history.size() == 2, "Setup: expected two retained observations.");

    for (std::uint32_t i = 0; i < measurements::kHandTopologyGapToleranceFrames; ++i)
    {
        history.noteMissingObservation();
    }
    require(history.size() == 2,
        "A gap within the tolerance window must not discard the retained history.");
    require(history.missingObservationCount() == measurements::kHandTopologyGapToleranceFrames,
        "Missing-observation count did not track the reported gaps.");

    history.push(makeObservation(150'000), true);
    require(history.size() == 3, "A resumed observation after a tolerated gap must extend the history.");
    require(history.missingObservationCount() == 0,
        "A successful push must clear the missing-observation counter.");
}

void testMissingObservationGraceWindowExceededResetsHistory()
{
    measurements::GestureCandidateWindowHistory history;
    history.push(makeObservation(0), true);
    history.push(makeObservation(50'000), true);

    for (std::uint32_t i = 0; i < measurements::kHandTopologyGapToleranceFrames + 1; ++i)
    {
        history.noteMissingObservation();
    }
    require(history.size() == 0,
        "Exceeding the gap-tolerance window must reset (not merely truncate) the history.");
}

void testResetClearsHistoryAndCandidateWindows()
{
    measurements::GestureCandidateWindowHistory history;
    for (int i = 0; i < 30; ++i)
    {
        history.push(makeObservation(static_cast<std::uint64_t>(i) * 20'000), true);
    }
    history.reset();
    require(history.size() == 0, "reset() must clear all retained observations.");
    require(history.missingObservationCount() == 0, "reset() must clear the missing-observation counter.");

    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>
        windows{};
    require(history.buildCandidateWindows(windows) == 0,
        "An empty (reset) history must not produce any candidate windows.");
}

void testNonMonotonicTimestampIsRejectedNotCorrupting()
{
    measurements::GestureCandidateWindowHistory history;
    history.push(makeObservation(100'000), true);
    history.push(makeObservation(200'000), true);
    require(history.size() == 2, "Setup: expected two accepted observations.");

    // Earlier than the most recent (200'000): must be a no-op, not inserted
    // out of order and not corrupting the chronological invariant.
    history.push(makeObservation(150'000), true);
    require(history.size() == 2,
        "A non-monotonic (out-of-order) timestamp must be rejected rather than inserted.");

    // The history must remain usable afterward: a later, monotonic push
    // still succeeds.
    history.push(makeObservation(300'000), true);
    require(history.size() == 3,
        "A defended history must continue to accept later, monotonic observations.");
}

void testBuildCandidateWindowsSkipsShortAndSparseHistories()
{
    measurements::GestureCandidateWindowHistory history;
    // Only 800 ms of history: below MinimumCandidateDurationMilliseconds
    // (1400 ms) for every configured duration.
    for (int i = 0; i < 30; ++i)
    {
        history.push(makeObservation(static_cast<std::uint64_t>(i) * 27'000), true); // ~800 ms total
    }
    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>
        windows{};
    require(history.buildCandidateWindows(windows) == 0,
        "A history shorter than the shortest candidate duration must produce no candidate windows.");
}

void testBuildCandidateWindowsRequiresMinimumUsableFrames()
{
    measurements::GestureCandidateWindowHistory history;
    // 2500 ms of history at ~10 fps (usable count well under 25), all marked
    // usable=false to isolate the usable-frame-count gate from duration.
    constexpr int kFrameCount = 20;
    for (int i = 0; i < kFrameCount; ++i)
    {
        history.push(makeObservation(static_cast<std::uint64_t>(i) * 130'000), false);
    }
    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>
        windows{};
    require(history.buildCandidateWindows(windows) == 0,
        "A history with too few usable frames must produce no candidate windows, even with enough duration.");
}

void testBuildCandidateWindowsCountsUnusableFramesInSourceButNotUsable()
{
    measurements::GestureCandidateWindowHistory history;
    // 60 frames over 2500 ms (well past every candidate duration); every
    // third frame is unusable (low confidence) but there are still >= 25
    // usable frames for every candidate duration.
    constexpr int kFrameCount = 60;
    constexpr std::uint64_t kIntervalUs = 42'000; // ~2478 ms total span.
    for (int i = 0; i < kFrameCount; ++i)
    {
        const bool usable = (i % 3) != 0;
        history.push(makeObservation(static_cast<std::uint64_t>(i) * kIntervalUs), usable);
    }
    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>
        windows{};
    const auto written = history.buildCandidateWindows(windows);
    require(written > 0, "Setup: expected at least one eligible candidate window.");
    for (std::size_t i = 0; i < written; ++i)
    {
        require(windows[i].valid, "A written candidate window must be marked valid.");
        require(windows[i].sourceFrameCount > windows[i].usableFrameCount,
            "Unusable frames must count toward sourceFrameCount but not usableFrameCount.");
        require(windows[i].usableFrameCount >= measurements::kGestureMinimumCandidateUsableFrames,
            "A written candidate window must meet the minimum usable-frame gate.");
    }
}

void testBuildCandidateWindowsProducesExactly32PointSequenceAndEndpoints()
{
    measurements::GestureCandidateWindowHistory history;
    constexpr int kFrameCount = 130;
    constexpr std::uint64_t kIntervalUs = 20'000; // 20 ms apart -> ~2580 ms span, 50 fps.
    for (int i = 0; i < kFrameCount; ++i)
    {
        const std::uint64_t timestampUs = static_cast<std::uint64_t>(i) * kIntervalUs;
        // Steady wrist translation so palmVelocity is well-defined and
        // nonzero throughout.
        history.push(makeObservation(timestampUs, static_cast<float>(i) * 2.0F, 0.0F), true);
    }

    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>
        windows{};
    const auto written = history.buildCandidateWindows(windows);
    require(written == measurements::kGestureCandidateDurationsMs.size(),
        "A long, dense, fully usable history must produce a candidate window for every configured duration.");

    const std::uint64_t now = static_cast<std::uint64_t>(kFrameCount - 1) * kIntervalUs;
    for (std::size_t i = 0; i < written; ++i)
    {
        const auto& window = windows[i];
        require(window.valid, "Every produced candidate window must be valid.");
        require(window.unifiedSequence.size() == measurements::kUnifiedSequenceLength,
            "The fixed unified-sequence array must always be exactly 32 points.");
        require(window.endTimestampUs == now,
            "A candidate window must end at the history's most recently pushed timestamp.");

        const std::uint64_t durationUs =
            static_cast<std::uint64_t>(measurements::kGestureCandidateDurationsMs[i]) * 1000ULL;
        const std::uint64_t expectedStart = now - durationUs;
        // The candidate's actual first sample is the earliest one with
        // timestamp >= expectedStart, so it lands within one sample
        // interval of the nominal cutoff.
        require(window.startTimestampUs >= expectedStart
                && window.startTimestampUs < expectedStart + kIntervalUs,
            "A candidate window's start endpoint did not match its nominal duration cutoff.");

        for (std::size_t p = 1; p < window.unifiedSequence.size(); ++p)
        {
            require(window.unifiedSequence[p].timeOffsetMs >= window.unifiedSequence[p - 1].timeOffsetMs,
                "The 32-point unified sequence must have non-decreasing time offsets.");
        }
        requireNear(window.unifiedSequence.front().timeOffsetMs, 0.0, 1.0e-6,
            "The 32-point unified sequence must start at time zero.");
    }
}

void testBuildCandidateWindowsActiveSegmentReflectsRealMotion()
{
    measurements::GestureCandidateWindowHistory history;
    constexpr int kFrameCount = 130;
    constexpr std::uint64_t kIntervalUs = 20'000;
    for (int i = 0; i < kFrameCount; ++i)
    {
        // Motion only in the middle third of the window; stationary
        // otherwise, so the active segment must be a proper (non-full)
        // sub-range of the resulting 32-point sequence.
        const bool moving = i >= kFrameCount / 3 && i <= 2 * kFrameCount / 3;
        const float dx = moving ? static_cast<float>(i) * 3.0F : static_cast<float>(kFrameCount / 3) * 3.0F;
        history.push(makeObservation(static_cast<std::uint64_t>(i) * kIntervalUs, dx, 0.0F), true);
    }

    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>
        windows{};
    const auto written = history.buildCandidateWindows(windows);
    require(written > 0, "Setup: expected at least one candidate window.");
    for (std::size_t i = 0; i < written; ++i)
    {
        require(windows[i].activeSegment.valid,
            "A candidate window containing real mid-sequence motion must report a valid active segment.");
    }
}

void testBuildCandidateWindowsNoMotionReportsInvalidActiveSegment()
{
    measurements::GestureCandidateWindowHistory history;
    constexpr int kFrameCount = 130;
    constexpr std::uint64_t kIntervalUs = 20'000;
    for (int i = 0; i < kFrameCount; ++i)
    {
        // Identical wrist position every frame: zero palm velocity
        // throughout.
        history.push(makeObservation(static_cast<std::uint64_t>(i) * kIntervalUs, 0.0F, 0.0F), true);
    }

    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>
        windows{};
    const auto written = history.buildCandidateWindows(windows);
    require(written > 0, "Setup: expected at least one candidate window.");
    for (std::size_t i = 0; i < written; ++i)
    {
        require(!windows[i].activeSegment.valid,
            "A stationary candidate window must not report an active segment.");
    }
}

// ---------------------------------------------------------------------
// Candidate-local topology isolation (Stage 1 boundary tests): a candidate
// window's topology/motion summary must come from only that candidate's own
// usable feature frames, never from the full 2600 ms rolling history or a
// longer sibling candidate's span.
// ---------------------------------------------------------------------

void testCandidateLocalTopologyCannotSeeMotionOutsideItsOwnDuration()
{
    measurements::GestureCandidateWindowHistory history;
    constexpr int kFrameCount = 130;
    constexpr std::uint64_t kIntervalUs = 20'000; // ~2580 ms total span.
    // Motion is confined to roughly [300, 700] ms - well inside the 2500 ms
    // candidate's [80, 2580] ms span, but entirely before the 1400 ms
    // candidate's [1180, 2580] ms span even begins.
    constexpr int kMotionStartIndex = 15; // ~300 ms
    constexpr int kMotionEndIndex = 35;   // ~700 ms
    for (int i = 0; i < kFrameCount; ++i)
    {
        float dx = 0.0F;
        if (i > kMotionEndIndex)
        {
            dx = static_cast<float>(kMotionEndIndex - kMotionStartIndex) * 20.0F;
        }
        else if (i >= kMotionStartIndex)
        {
            dx = static_cast<float>(i - kMotionStartIndex) * 20.0F;
        }
        history.push(makeObservation(static_cast<std::uint64_t>(i) * kIntervalUs, dx, 0.0F), true);
    }

    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>
        windows{};
    const auto written = history.buildCandidateWindows(windows);
    require(written == measurements::kGestureCandidateDurationsMs.size(),
        "Setup: expected every candidate duration to be eligible.");

    // kGestureCandidateDurationsMs is [1400, 1700, 2100, 2500]: index 0 is
    // the shortest candidate, index 3 the longest.
    const auto& shortCandidate = windows[0];
    const auto& longCandidate = windows[3];
    require(shortCandidate.topology.valid && longCandidate.topology.valid,
        "Both candidates must produce a valid candidate-local topology summary.");

    require(shortCandidate.topology.palmTravel < 0.05F,
        "The 1400 ms candidate must not see palm travel from motion that ended before its own span began.");
    require(shortCandidate.topology.translationDistance < 0.05F,
        "The 1400 ms candidate must not see net translation from motion outside its own span.");
    require(longCandidate.topology.palmTravel > (shortCandidate.topology.palmTravel + 0.3F) * 5.0F,
        "The 2500 ms candidate, which spans the motion, must report meaningfully more palm travel than the "
        "1400 ms candidate, which does not.");
    require(longCandidate.topology.translationDistance > 0.3F,
        "The 2500 ms candidate must report the net translation produced by motion within its own span.");
}

void testCandidateLocalTopologySampleCountMatchesOwnUsableFramesNotFullHistory()
{
    measurements::GestureCandidateWindowHistory history;
    constexpr int kFrameCount = 130;
    constexpr std::uint64_t kIntervalUs = 20'000; // ~2580 ms total span.
    for (int i = 0; i < kFrameCount; ++i)
    {
        history.push(makeObservation(static_cast<std::uint64_t>(i) * kIntervalUs, 0.0F, 0.0F), true);
    }
    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>
        windows{};
    const auto written = history.buildCandidateWindows(windows);
    require(written == measurements::kGestureCandidateDurationsMs.size(),
        "Setup: expected every candidate duration to be eligible.");
    for (std::size_t i = 0; i < written; ++i)
    {
        require(windows[i].topology.sampleCount == windows[i].usableFrameCount,
            "A candidate's topology sample count must match only its own usable frame count.");
        require(windows[i].topology.sampleCount < static_cast<std::size_t>(kFrameCount),
            "A candidate shorter than the full history must summarize strictly fewer samples than the "
            "full history, proving it cannot see the full rolling window.");
    }
    require(windows[0].topology.sampleCount < windows[3].topology.sampleCount,
        "A shorter candidate duration must summarize strictly fewer samples than a longer one.");
}

void testCandidateLocalTopologyIndependentAcrossDurationsForSameHistory()
{
    // A single motion event spanning nearly the whole history should shift
    // every candidate duration's topologyChangeScore by a different amount,
    // proving each duration recomputes its own summary rather than sharing
    // one whole-history value.
    measurements::GestureCandidateWindowHistory history;
    constexpr int kFrameCount = 130;
    constexpr std::uint64_t kIntervalUs = 20'000;
    for (int i = 0; i < kFrameCount; ++i)
    {
        const float dx = static_cast<float>(i) * 3.0F;
        history.push(makeObservation(static_cast<std::uint64_t>(i) * kIntervalUs, dx, 0.0F), true);
    }
    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>
        windows{};
    const auto written = history.buildCandidateWindows(windows);
    require(written == measurements::kGestureCandidateDurationsMs.size(),
        "Setup: expected every candidate duration to be eligible.");
    for (std::size_t i = 1; i < written; ++i)
    {
        require(windows[i].topology.translationDistance > windows[i - 1].topology.translationDistance,
            "A longer candidate duration spanning strictly more of a continuous motion must report strictly "
            "more net translation than a shorter, nested candidate duration.");
    }
}

// ---------------------------------------------------------------------
// MultiHandMeasurementStage integration: existing stable-track lifecycle.
// ---------------------------------------------------------------------

void testMultiHandStageKeepsIndependentInterleavedTrackHistories()
{
    using ryoiki::hand_input::measurements::MultiHandMeasurementStage;
    MultiHandMeasurementStage stage;

    constexpr int kFrameCount = 90;
    constexpr std::uint64_t kIntervalUs = 27'000;
    for (int i = 0; i < kFrameCount; ++i)
    {
        HandPerceptionResult observations{};
        observations.handCount = 2;
        observations.trackIds = {41, 84};
        // Track 41 moves in +X; track 84 stays still. Interleaved every
        // frame through the same stage instance.
        observations.hands[0] = makeHand(static_cast<float>(i) * 4.0F, 0.0F, 0.9F, 0.9F);
        observations.hands[1] = makeHand(0.0F, 0.0F, 0.9F, 0.1F);
        static_cast<void>(stage.extract(
            observations, static_cast<std::uint64_t>(i) + 1, static_cast<std::uint64_t>(i) * kIntervalUs, 640, 480));
    }

    const auto* historyA = stage.gestureCandidateHistory(41);
    const auto* historyB = stage.gestureCandidateHistory(84);
    require(historyA != nullptr && historyB != nullptr, "Both interleaved tracks must have an active history.");
    require(historyA->size() == kFrameCount && historyB->size() == kFrameCount,
        "Interleaving two tracks every frame must not drop or duplicate either track's observations.");

    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>
        windowsA{};
    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>
        windowsB{};
    const auto writtenA = historyA->buildCandidateWindows(windowsA);
    const auto writtenB = historyB->buildCandidateWindows(windowsB);
    require(writtenA > 0 && writtenB > 0, "Both tracks must produce at least one candidate window.");
    require(windowsA[0].activeSegment.valid, "The moving track's candidate window must report active motion.");
    require(!windowsB[0].activeSegment.valid, "The stationary track's candidate window must not report active motion.");
    requireNear(windowsA[0].unifiedSequence[0].values[measurements::kHandednessOffset], 0.9, 1.0e-3,
        "Track 41's handedness leaked track 84's value.");
    requireNear(windowsB[0].unifiedSequence[0].values[measurements::kHandednessOffset], 0.1, 1.0e-3,
        "Track 84's handedness leaked track 41's value.");
}

void testMultiHandStageResetsHistoryOnTrackRemovalAndSlotReuse()
{
    using ryoiki::hand_input::measurements::MultiHandMeasurementStage;
    MultiHandMeasurementStage stage;

    constexpr std::uint64_t kIntervalUs = 33'000;
    std::uint64_t frameId = 0;
    std::uint64_t timestampUs = 0;

    // Track 7 builds up history for 10 frames.
    for (int i = 0; i < 10; ++i)
    {
        HandPerceptionResult observations{};
        observations.handCount = 1;
        observations.trackIds[0] = 7;
        observations.hands[0] = makeHand(static_cast<float>(i) * 4.0F, 0.0F, 0.9F, 0.8F);
        ++frameId;
        timestampUs += kIntervalUs;
        static_cast<void>(stage.extract(observations, frameId, timestampUs, 640, 480));
    }
    require(stage.gestureCandidateHistory(7) != nullptr, "Track 7 must have an active history after 10 frames.");
    require(stage.gestureCandidateHistory(7)->size() == 10, "Track 7's history did not record every pushed frame.");

    // Track 7 disappears for more than the gap-tolerance window: its slot
    // must evict and reset.
    for (std::uint32_t i = 0; i < measurements::kHandTopologyGapToleranceFrames + 1; ++i)
    {
        HandPerceptionResult observations{};
        observations.handCount = 0;
        ++frameId;
        timestampUs += kIntervalUs;
        static_cast<void>(stage.extract(observations, frameId, timestampUs, 640, 480));
    }
    require(stage.gestureCandidateHistory(7) == nullptr,
        "An evicted track must no longer report an active gesture history.");

    // A new track (9) claims a slot. Its history must start empty, with no
    // trace of track 7's prior motion.
    HandPerceptionResult observations{};
    observations.handCount = 1;
    observations.trackIds[0] = 9;
    observations.hands[0] = makeHand(0.0F, 0.0F, 0.9F, 0.3F);
    ++frameId;
    timestampUs += kIntervalUs;
    static_cast<void>(stage.extract(observations, frameId, timestampUs, 640, 480));

    const auto* historyNine = stage.gestureCandidateHistory(9);
    require(historyNine != nullptr, "The new track must have an active history.");
    require(historyNine->size() == 1,
        "A newly assigned track's history must start fresh, not inherit an evicted track's samples.");
}

void testMultiHandStageWiresConfidenceGateIntoUsableFrameCount()
{
    using ryoiki::hand_input::measurements::MultiHandMeasurementStage;
    MultiHandMeasurementStage stage;

    constexpr std::uint64_t kIntervalUs = 20'000;
    std::uint64_t frameId = 0;
    std::uint64_t timestampUs = 0;
    constexpr int kFrameCount = 100;
    for (int i = 0; i < kFrameCount; ++i)
    {
        HandPerceptionResult observations{};
        observations.handCount = 1;
        observations.trackIds[0] = 3;
        // Every third frame reports a below-gate confidence.
        const float confidence = (i % 3) == 0 ? 0.1F : 0.9F;
        observations.hands[0] = makeHand(static_cast<float>(i) * 2.0F, 0.0F, confidence, 0.5F);
        ++frameId;
        timestampUs += kIntervalUs;
        static_cast<void>(stage.extract(observations, frameId, timestampUs, 640, 480));
    }

    const auto* history = stage.gestureCandidateHistory(3);
    require(history != nullptr, "Track 3 must have an active history.");
    require(history->size() == kFrameCount, "Low-confidence frames must still be retained in the raw history.");

    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>
        windows{};
    const auto written = history->buildCandidateWindows(windows);
    require(written > 0, "Setup: expected at least one candidate window.");
    require(windows[0].sourceFrameCount > windows[0].usableFrameCount,
        "MultiHandMeasurementStage's confidence gate must reduce usableFrameCount below sourceFrameCount.");
}
}

int main()
{
    try
    {
        testFindActiveSegmentNoActiveMotionReturnsInvalid();
        testFindActiveSegmentRequiresMinimumActiveFrames();
        testFindActiveSegmentBoundaryAndPadding();
        testFindActiveSegmentPaddingClampsToSequenceBounds();
        testIndependentHistoriesDoNotShareState();
        testTimeBasedEvictionKeepsWithinRollingWindow();
        testCapacityEvictionBoundsMemoryWithoutTimeEviction();
        testMissingObservationGraceWindowPreservesHistory();
        testMissingObservationGraceWindowExceededResetsHistory();
        testResetClearsHistoryAndCandidateWindows();
        testNonMonotonicTimestampIsRejectedNotCorrupting();
        testBuildCandidateWindowsSkipsShortAndSparseHistories();
        testBuildCandidateWindowsRequiresMinimumUsableFrames();
        testBuildCandidateWindowsCountsUnusableFramesInSourceButNotUsable();
        testBuildCandidateWindowsProducesExactly32PointSequenceAndEndpoints();
        testBuildCandidateWindowsActiveSegmentReflectsRealMotion();
        testBuildCandidateWindowsNoMotionReportsInvalidActiveSegment();
        testCandidateLocalTopologyCannotSeeMotionOutsideItsOwnDuration();
        testCandidateLocalTopologySampleCountMatchesOwnUsableFramesNotFullHistory();
        testCandidateLocalTopologyIndependentAcrossDurationsForSameHistory();
        testMultiHandStageKeepsIndependentInterleavedTrackHistories();
        testMultiHandStageResetsHistoryOnTrackRemovalAndSlotReuse();
        testMultiHandStageWiresConfidenceGateIntoUsableFrameCount();
        std::cout << "GestureCandidateWindowHistory tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
