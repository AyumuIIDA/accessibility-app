#pragma once

#include "HandInput/Measurements/hand_topology_history.h"
#include "HandPerception/MediaPipeGraph/hand_landmark_graph.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ryoiki::hand_input::measurements
{
// Native port of the C# prototype's dimensionless, fixed-length single-hand
// feature representation (src/RyoikiTenkai/Vision/GestureFeatureExtractor.cs,
// commit a5bed23 on origin/pr/1/head). This module builds one flat feature
// vector per observation and resamples an ordered sequence of them to a fixed
// sample count for future template/DTW matching. It also locates the PR
// prototype's active segment (findActiveSegment, ported from
// GestureFeatureExtractor.FindActiveSegment) within an already-built
// sequence. It intentionally does not select which observations belong to a
// sequence (candidate-window selection), gate on topology, or perform
// recognition; candidate-window selection over a per-track history is
// implemented separately in gesture_candidate_window_history. See
// doc/hand-input-architecture.md for the Observation -> Measurement ->
// Recognition boundary this stays inside of.
//
// Flat feature vector layout (kUnifiedFeatureVectorLength = 64 floats),
// preserved byte-for-byte from the PR prototype so recorded gesture
// templates and thresholds remain valid:
//
//   [0, 42)   21 landmarks (x, y): wrist-relative, divided by the
//             wrist-to-middle-finger-MCP distance, then rotated so the palm
//             axis (wrist -> middle MCP) points along +X. Dimensionless.
//   [42, 47)  per finger: (finger-base-to-fingertip distance) / scale, clamped to
//             [0, 3]. Index order thumb, index, middle, ring, pinky.
//   [47, 51)  adjacent-fingertip distance / scale, clamped to [0, 3], for
//             (thumb,index), (index,middle), (middle,ring), (ring,pinky).
//   [51, 56)  finger straightness, see analyzeFingerPose. This range starts
//             at kFingerStraightnessOffset.
//   [56]      sin(palm orientation)
//   [57]      cos(palm orientation)
//   [58]      handedness (kHandednessOffset), 0 = left, 1 = right
//   [59]      signed palm area (kSignedPalmAreaOffset), dimensionless
//   [60]      palm compression (kPalmCompressionOffset), dimensionless
//   [61]      palm depth range (kPalmDepthRangeOffset), dimensionless
//   [62]      hand-scale ratio vs. the sequence's first frame
//             (kHandScaleRatioOffset); 1 for a single, unsequenced frame.
//   [63]      bounding-box-area ratio vs. the sequence's first frame
//             (kBoundingBoxAreaRatioOffset); 1 for a single, unsequenced
//             frame.
//
// Deviation from the PR prototype: the prototype also carried a parallel
// `GestureFrameFeatures` record with a few fields never placed in the flat
// vector (bounding-box aspect, raw confidence, raw hand scale, and a
// translation/distance restatement of centerX/centerY). Those fields fed
// only GestureFeatureExtractor.SummarizeTopology/BuildFeatureSummary; they
// are not reproduced here. summarizeTopology() below is the native port of
// SummarizeTopology itself (the fields it actually reads all come from the
// flat value vector and centerX/centerY/palmOrientationRadians, none from
// the dropped GestureFrameFeatures fields), operating directly on a
// buildFeatureSequence()-produced frame list - the same candidate-duration-
// scoped input SummarizeTopology(sequence.Frames) receives in the PR. This is
// a distinct, narrower-scope function from HandTopologyHistory::summarize(),
// which summarizes a different measurement pipeline's entire retained 2600 ms
// rolling window rather than one candidate's own slice; see
// gesture_candidate_window_history.h and unified_sequence_gesture_comparator.h
// for how a candidate window's own summarizeTopology() result plugs into
// topology-gated template comparison. interpolate() below covers both the
// PR's Interpolate and InterpolateFeatures responsibilities for the fields
// this module keeps.
inline constexpr std::size_t kFingerCount = 5;
inline constexpr std::size_t kLandmarkCount = 21;
inline constexpr std::size_t kUnifiedFeatureVectorLength =
    (kLandmarkCount * 2) + kFingerCount + (kFingerCount - 1) + kFingerCount + 2 + 1 + 5;
inline constexpr std::size_t kFingerStraightnessOffset =
    (kLandmarkCount * 2) + kFingerCount + (kFingerCount - 1);
inline constexpr std::size_t kFingerStraightnessLength = kFingerCount;
inline constexpr std::size_t kHandednessOffset =
    kFingerStraightnessOffset + kFingerStraightnessLength + 2;
inline constexpr std::size_t kSignedPalmAreaOffset = kHandednessOffset + 1;
inline constexpr std::size_t kPalmCompressionOffset = kSignedPalmAreaOffset + 1;
inline constexpr std::size_t kPalmDepthRangeOffset = kPalmCompressionOffset + 1;
inline constexpr std::size_t kHandScaleRatioOffset = kPalmDepthRangeOffset + 1;
inline constexpr std::size_t kBoundingBoxAreaRatioOffset = kHandScaleRatioOffset + 1;

// Fixed resample length used by BuildUnifiedSequence in the PR prototype.
inline constexpr std::size_t kUnifiedSequenceLength = 32;

// A finger is treated as open/extended at or above this straightness value.
inline constexpr float kFingerOpenThreshold = 0.55F;

struct FingerPose
{
    std::array<float, kFingerCount> straightness{};
    std::uint32_t stateMask{0};
};

// Per-finger straightness from wrist-relative tip/base distance ratios.
// landmarks are in the same image/upright pixel space as
// hand_perception::HandLandmarkResult::landmarks (not the world-space
// canonicalLandmarks published by HandMeasurementExtractor). This is
// mathematically identical to the finger-straightness computation already
// inlined in HandMeasurementExtractor::extract (which populates
// HandTopologyMeasurement::fingerStraightness/fingerStateMask); it is
// reimplemented here, rather than shared, so this module stays independently
// testable without requiring a valid HandTopologyMeasurement/upright-frame
// context. hand_unified_feature_frame_tests.cpp cross-checks the two against
// the same synthetic input to keep them provably equivalent.
[[nodiscard]] FingerPose analyzeFingerPose(
    const std::array<hand_perception::Landmark3f, kLandmarkCount>& imageLandmarks) noexcept;

// One time-stamped point of the dimensionless single-hand feature sequence.
// Mirrors the PR prototype's GestureFeatureFrame (minus the dropped
// GestureFrameFeatures fields noted above).
struct UnifiedFeatureFrame
{
    double timeOffsetMs{0.0};
    // Cumulative wrist translation since the first frame of the sequence
    // this frame belongs to, in palm-axis-length units. Zero for a frame
    // built standalone by buildFeatureFrame, and for index 0 of any sequence
    // built by buildFeatureSequence.
    float centerX{0.0F};
    float centerY{0.0F};
    float palmOrientationRadians{0.0F};
    // Instantaneous palm-axis-length-per-second speed since the previous
    // frame in the sequence. Zero for a standalone frame, and for index 0 of
    // any sequence built by buildFeatureSequence.
    float palmVelocity{0.0F};
    std::array<float, kUnifiedFeatureVectorLength> values{};
};

// One source observation consumed by buildFeatureFrame/buildFeatureSequence.
struct UnifiedFeatureObservation
{
    std::uint64_t timestampUs{0};
    std::array<hand_perception::Landmark3f, kLandmarkCount> imageLandmarks{};
    float handedness{0.0F};
    // Upright-frame bounding-box area in any consistent unit (pixels or
    // normalized); 0 means unavailable. Only the ratio between an
    // observation's area and its sequence's first observation's area is
    // used, so the unit is irrelevant as long as it is consistent within one
    // sequence. Mirrors the PR prototype's nullable GestureFrameSample.BoundingBox.
    float boundingBoxArea{0.0F};
};

// Bounded in-process debug/playback data corresponding to the PR's
// GestureTemplateSample and GestureSkeletonFrame. Coordinates are relative
// to the first palm centre and divided by the window-average palm scale.
struct GestureVisualizationFrame
{
    double timeOffsetMs{0.0};
    float centerX{0.0F};
    float centerY{0.0F};
    std::array<hand_perception::Landmark3f, kLandmarkCount> skeleton{};
};

struct GestureVisualizationSequence
{
    std::array<GestureVisualizationFrame, kUnifiedSequenceLength> frames{};
    bool valid{false};
};

[[nodiscard]] GestureVisualizationSequence buildGestureVisualizationSequence(
    const std::vector<UnifiedFeatureObservation>& observations) noexcept;

// Builds a UnifiedFeatureObservation from a raw perception-graph result.
// timestampUs must be caller-supplied (HandLandmarkResult carries no
// timestamp).
[[nodiscard]] UnifiedFeatureObservation makeUnifiedFeatureObservation(
    const hand_perception::HandLandmarkResult& hand, std::uint64_t timestampUs) noexcept;

// Builds one raw feature frame for a single observation, independent of any
// sequence. timeOffsetMs, centerX, centerY, and palmVelocity are left at
// their defaults (0); the hand-scale-ratio and bounding-box-area-ratio value
// slots are left at 1 (the single-frame identity value). Use
// buildFeatureSequence to fill in the sequence-relative fields.
[[nodiscard]] UnifiedFeatureFrame buildFeatureFrame(
    const UnifiedFeatureObservation& observation) noexcept;

// Builds the full per-observation feature sequence from a chronologically
// ordered, single-track window of observations: builds each raw frame with
// buildFeatureFrame, then fills in the sequence-relative time offset,
// translation, velocity, hand-scale-ratio, and bounding-box-area-ratio
// fields, exactly as the PR prototype's Extract() second pass does. Callers
// are responsible for selecting and ordering the observations that make up
// one sequence (candidate-window selection is out of scope here). Returns an
// empty vector for empty input.
[[nodiscard]] std::vector<UnifiedFeatureFrame> buildFeatureSequence(
    const std::vector<UnifiedFeatureObservation>& observations) noexcept;

// Linearly interpolates between two feature frames at parameter t in [0, 1],
// clamped. The flat value vector is interpolated element-wise; orientation
// uses shortest-path angular interpolation. The result's finger straightness
// and finger-state slots therefore come from the interpolated flat vector
// (as PR's ReadFingerStraightness/ReadFingerStateMask do), not from a
// separate interpolation of the endpoints' discrete finger states.
[[nodiscard]] UnifiedFeatureFrame interpolate(
    const UnifiedFeatureFrame& a,
    const UnifiedFeatureFrame& b,
    double timeOffsetMs,
    float t) noexcept;

// Resamples an ordered, non-empty feature sequence to exactly `length`
// uniform time samples spanning [0, frames.back().timeOffsetMs -
// frames.front().timeOffsetMs] (output time is always rebased to start at
// zero, regardless of the input's own time origin). A single input frame is
// repeated `length` times unchanged (including its original timeOffsetMs).
// Returns an empty vector when frames is empty or length is 0.
[[nodiscard]] std::vector<UnifiedFeatureFrame> resample(
    const std::vector<UnifiedFeatureFrame>& frames, std::size_t length) noexcept;

// Equivalent of GestureFeatureExtractor.BuildUnifiedSequence: resamples to
// the fixed kUnifiedSequenceLength (32).
[[nodiscard]] std::vector<UnifiedFeatureFrame> buildUnifiedSequence(
    const std::vector<UnifiedFeatureFrame>& frames) noexcept;

// Frame-index padding applied around the contiguous run of above-threshold
// palmVelocity frames. The PR prototype's FindActiveSegment uses this as a
// bare literal (`- 2` / `+ 2`) rather than a named constant; named here for
// clarity, value unchanged.
inline constexpr std::size_t kActiveSegmentPadding = 2;
// A candidate needs at least this many above-threshold frames before it is
// treated as containing real motion, matching the PR prototype's
// `activeIndexes.Count < 3` rejection.
inline constexpr std::size_t kActiveSegmentMinimumActiveFrames = 3;
// Instantaneous palmVelocity at or above this value (palm-axis lengths per
// second) counts as motion. Matches GestureFeatureExtractor.ActiveVelocityThreshold.
inline constexpr float kActiveVelocityThreshold = 0.09F;

// Equivalent of the PR prototype's GestureActiveSegment: the padded index
// range of a feature sequence around above-threshold palm velocity, plus the
// average palm-axis-length-per-second speed over that range. `valid` is
// false where the PR prototype returns a null GestureActiveSegment (fewer
// than kActiveSegmentMinimumActiveFrames frames cross
// kActiveVelocityThreshold).
struct ActiveSegment
{
    std::size_t startIndex{0};
    std::size_t endIndex{0};
    double startTimeOffsetMs{0.0};
    double endTimeOffsetMs{0.0};
    float pathVelocity{0.0F};
    bool valid{false};
};

// Equivalent of GestureFeatureExtractor.FindActiveSegment. frames must
// already be the chronologically ordered, confidence-filtered sequence
// buildFeatureSequence produced; this function does not select which
// observations belong to the sequence (that remains candidate-window
// selection, out of scope for this module) and does not gate on topology.
// Returns an invalid (default-initialized) segment when there is no
// qualifying motion, mirroring the PR prototype's null return.
[[nodiscard]] ActiveSegment findActiveSegment(
    const std::vector<UnifiedFeatureFrame>& frames) noexcept;

// Reader helpers mirroring the PR prototype's Read*(values) helpers. Unlike
// the prototype, the flat vector has a compile-time-fixed length, so no
// bounds fallback is needed.
[[nodiscard]] std::array<float, kFingerStraightnessLength> readFingerStraightness(
    const std::array<float, kUnifiedFeatureVectorLength>& values) noexcept;
[[nodiscard]] std::uint32_t readFingerStateMask(
    const std::array<float, kUnifiedFeatureVectorLength>& values) noexcept;

// Equivalent of GestureFeatureExtractor.SummarizeTopology. `frames` must be
// the chronologically ordered, already-usable-filtered output of
// buildFeatureSequence for exactly the span the caller wants summarized -
// this function does not re-scope or re-filter its input. Passing a whole
// track's multi-second history produces a whole-history summary; passing one
// candidate duration's own slice (as gesture_candidate_window_history.cpp
// does for each of the 1400/1700/2100/2500 ms candidates) produces a
// summary scoped to only that slice's own motion, with no visibility into
// observations outside it. Returns a summary with sampleCount ==
// frames.size(), valid == true always (SummarizeTopology never returns null
// in the PR; only the caller-side GestureTopologySummary? wrapping is
// optional), and all-zero measurement fields for empty input.
// firstFrameId/lastFrameId are always 0 (UnifiedFeatureFrame carries no frame
// ID, only a sequence-relative timeOffsetMs); durationUs is a native-only
// auxiliary computed from the frame span's timeOffsetMs, not part of the PR's
// GestureTopologySummary (which has no duration field).
[[nodiscard]] HandTopologySummary summarizeTopology(
    const std::vector<UnifiedFeatureFrame>& frames) noexcept;

// Equivalent of the motion-score portion of GestureFeatureExtractor.Extract's
// second pass (peakVelocity/velocitySum/motionScore), consumed by
// GestureFeatureExtractor.DetectKind. `frames` must be the same
// buildFeatureSequence-produced, candidate-duration-scoped span passed to
// summarizeTopology (not a resampled sequence); `palmTravel` is that same
// call's HandTopologySummary::palmTravel, which is mathematically identical
// to the PR's Extract()-local `pathLength` (both sum consecutive
// centerX/centerY deltas over the identical frames), so callers that already
// computed a topology summary for this span reuse its palmTravel rather than
// re-deriving path length. All fields are 0 for fewer than 2 frames.
struct MotionSummary
{
    float motionScore{0.0F};
    float averagePalmVelocity{0.0F};
    float peakPalmVelocity{0.0F};
};

[[nodiscard]] MotionSummary summarizeMotion(
    const std::vector<UnifiedFeatureFrame>& frames, float palmTravel) noexcept;

// Equivalent of GestureFeatureExtractor.StaticMotionThreshold /
// GestureFeatureExtractor.DetectKind's threshold.
inline constexpr float kStaticMotionThreshold = 0.18F;

enum class GestureKind : std::uint32_t
{
    Static = 0,
    Dynamic = 1
};

// Equivalent of GestureFeatureExtractor.DetectKind.
[[nodiscard]] inline GestureKind detectGestureKind(const float motionScore) noexcept
{
    return motionScore < kStaticMotionThreshold ? GestureKind::Static : GestureKind::Dynamic;
}
}
