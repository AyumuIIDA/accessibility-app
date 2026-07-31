#pragma once

#include "HandInput/Measurements/hand_topology_history.h"
#include "HandInput/Measurements/hand_unified_feature_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ryoiki::hand_input::measurements
{
// Native port of the PR prototype's rolling per-hand candidate-window
// construction, applied independently per stable track. See
// doc/hand-input-architecture.md for the Observation -> Measurement ->
// Recognition boundary this stays inside of: this module produces
// PR-equivalent candidate windows for a future recognition/DTW consumer; it
// does not select a best candidate, compare against a template, run DTW, or
// make a recognition decision.
//
// PR variant note (do not silently combine with the other path):
// origin/pr/1/head has two rolling-window recognizers built on
// GestureFeatureExtractor:
//   1. WindowGestureRecognizer (single-hand): GestureWindowBuffer, a 2600 ms
//      rolling buffer of GestureFrameSample, plus CreateCandidateWindows,
//      which slices that buffer at CandidateDurationsMilliseconds
//      = [1400, 1700, 2100, 2500] ms ending at the latest sample, gates each
//      slice on MinimumCandidateDurationMilliseconds (1400 ms) and
//      MinimumCandidateUsableFrames (25), then runs
//      GestureFeatureExtractor.Extract + BuildUnifiedSequence over the
//      surviving usable samples.
//   2. MultiHandWindowGestureRecognizer (joint two-hand): a materially
//      different feature algorithm (MultiHandGestureFeatureExtractor) that
//      fuses both hands' geometry into one relative-distance/translation
//      sequence for joint two-hand gestures (e.g. a clap). It has no
//      per-hand UnifiedFeatureFrame vector and is not a per-track variant of
//      path (1).
// This module ports ONLY path (1), instantiated once per native stable
// track slot (see MultiHandMeasurementStage), so each of the runtime's
// <= hand_perception::kMaxPerceivedHands independently tracked hands gets
// its own single-hand UnifiedSequence candidate-window history. Porting path
// (2)'s joint two-hand fusion is a distinct future step.
//
// The rolling window reuses hand_topology_history's PR-derived window
// duration/capacity/gap-tolerance constants (kHandTopologyWindowUs,
// kHandTopologyHistoryCapacity, kHandTopologyGapToleranceFrames): both
// histories are the same 2600 ms rolling recognizer window from the
// prototype, applied to different per-observation payloads, so they share
// one source of truth rather than risking silent drift between two copies.
inline constexpr std::array<std::uint32_t, 4> kGestureCandidateDurationsMs{
    1400, 1700, 2100, 2500};
inline constexpr double kGestureMinimumCandidateDurationMs = 1400.0;
inline constexpr std::size_t kGestureMinimumCandidateUsableFrames = 25;
// Matches GestureFeatureExtractor.MinimumConfidence / the same 0.35f
// "usable frame" gate inlined in GestureWindowBuffer/WindowGestureRecognizer.
inline constexpr float kGestureMinimumUsableConfidence = 0.35F;

// One candidate window built from a per-track history, ready for a future
// DTW/template comparison consumer.
struct GestureCandidateWindow
{
    std::uint64_t startTimestampUs{0};
    std::uint64_t endTimestampUs{0};
    double durationMs{0.0};
    // Total observations in [start, end], including any below the usable
    // confidence gate. Mirrors the PR prototype's
    // GestureTemplate.SourceFrameCount for a candidate window.
    std::size_t sourceFrameCount{0};
    std::size_t usableFrameCount{0};
    ActiveSegment activeSegment{};
    std::array<UnifiedFeatureFrame, kUnifiedSequenceLength> unifiedSequence{};
    GestureVisualizationSequence visualization{};
    // Candidate-duration-scoped topology summary (summarizeTopology), built
    // from exactly this window's own usable feature frames (before
    // resampling to unifiedSequence) - mirrors the PR prototype's
    // per-candidate GestureFeatureExtractor.SummarizeTopology(sequence.Frames)
    // call in WindowGestureRecognizer.CreateCandidateWindows. This carries no
    // visibility into observations outside [startTimestampUs,
    // endTimestampUs], unlike a track's full HandTopologyHistory summary.
    HandTopologySummary topology{};
    // Candidate-duration-scoped motion summary (summarizeMotion), same span
    // as topology. Used by template-creation gates (GestureKind
    // classification); not consumed by topologyRejectionReason.
    float motionScore{0.0F};
    float averagePalmVelocity{0.0F};
    float peakPalmVelocity{0.0F};
    bool valid{false};
};

// Bounded, allocation-free rolling history of one stable track's unified
// feature observations, plus PR-equivalent candidate-window construction.
// Missing observations are tolerated for a short grace window (mirroring
// HandTopologyHistory) instead of discarding history on the first dropped
// frame; a track reassignment or an exhausted grace window must call
// reset() (see MultiHandMeasurementStage's existing slot lifecycle, which
// this module is wired into the same way as HandTopologyHistory).
class GestureCandidateWindowHistory final
{
public:
    // Records one observation. `usable` should reflect the same gate the PR
    // prototype applies inside GestureFeatureExtractor.Extract (landmark
    // count and confidence >= kGestureMinimumUsableConfidence); unusable
    // observations are still retained (they count toward a candidate
    // window's time span and sourceFrameCount, exactly as the PR's
    // unfiltered GestureWindowBuffer samples do) but are excluded from the
    // feature sequence a candidate window builds.
    //
    // Defensive addition beyond the PR prototype: an observation whose
    // timestamp is strictly earlier than the most recently pushed one is
    // rejected (the call is a no-op) rather than corrupting the
    // chronological ordering buildCandidateWindows relies on. The PR
    // prototype assumes a monotonic wall-clock caller and has no such guard.
    void push(const UnifiedFeatureObservation& observation, bool usable) noexcept;

    // Records one observation gap for this track. The retained history
    // survives up to kHandTopologyGapToleranceFrames consecutive gaps before
    // resetting, mirroring HandTopologyHistory::noteMissingObservation.
    void noteMissingObservation() noexcept;
    void reset() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint32_t missingObservationCount() const noexcept;

    // Builds every eligible candidate window ending at the most recently
    // pushed observation's timestamp, one attempt per entry of
    // kGestureCandidateDurationsMs (in that order), mirroring
    // WindowGestureRecognizer.CreateCandidateWindows. Returns the number of
    // entries written to the front of outWindows; a duration is skipped
    // (nothing written for it) when its slice does not meet
    // kGestureMinimumCandidateDurationMs or
    // kGestureMinimumCandidateUsableFrames, exactly as the PR prototype
    // skips (continues past) an ineligible duration.
    std::size_t buildCandidateWindows(
        std::array<GestureCandidateWindow, kGestureCandidateDurationsMs.size()>&
            outWindows) const noexcept;

private:
    struct StoredObservation
    {
        UnifiedFeatureObservation observation{};
        bool usable{false};
    };

    [[nodiscard]] const StoredObservation& at(
        std::size_t chronologicalIndex) const noexcept;
    void popOldest() noexcept;
    void pruneToWindow() noexcept;

    std::array<StoredObservation, kHandTopologyHistoryCapacity> samples_{};
    std::size_t start_{0};
    std::size_t size_{0};
    std::uint32_t missingObservations_{0};
};
}
