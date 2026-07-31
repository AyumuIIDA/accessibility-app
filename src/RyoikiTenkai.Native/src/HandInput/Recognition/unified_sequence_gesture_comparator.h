#pragma once

#include "HandInput/Measurements/gesture_candidate_window_history.h"
#include "HandInput/Measurements/hand_topology_history.h"
#include "HandInput/Measurements/hand_unified_feature_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ryoiki::hand_input::recognition
{
// Native port of the PR prototype's single-hand UnifiedSequence topology
// rejection gates and DTW template comparison
// (src/RyoikiTenkai/Vision/WindowGestureRecognizer.cs and
// GestureTemplateFactory.cs, commit a5bed23 on origin/pr/1/head). This module
// is the on-demand comparison primitive that sits behind the Measurement ->
// Recognition boundary documented in doc/hand-input-architecture.md: it
// consumes the existing measurement-layer outputs
// (measurements::GestureCandidateWindow's fixed 32-point unified sequence,
// and measurements::HandTopologySummary, both already produced per track by
// MultiHandMeasurementStage/GestureCandidateWindowHistory/HandTopologyHistory)
// and answers "does this candidate window plausibly match this template, and
// with what DTW score/breakdown" for a single stored template. It does not:
//
//   - select among several templates/definitions (WindowGestureRecognizer.
//     FindBestMatch's per-definition iteration and CreateInactiveScore path);
//   - persist, load, or manage template/recording files (GestureDefinition,
//     GestureDefinitionStore, GestureTemplateFactory.TryCreate's recording
//     accept/reject gates);
//   - require consecutive-match confirmation or a trigger cooldown
//     (WindowGestureRecognizer.Recognize's ConsecutiveMatchThreshold/Cooldown/
//     IsDuplicateSegment state machine);
//   - publish a recognition State/Event or cross the C ABI.
//
// Those remain later, separate steps. This module is pure and stateless: it
// performs no steady-state allocation in the per-frame Measurement path (it
// is only ever invoked on demand, once per comparison, matching "comparison
// is on-demand in Recognition" in doc/hand-input-architecture.md), and it
// never mutates measurement-layer state.
//
// ---------------------------------------------------------------------------
// Mapping HandTopologySummary onto the PR's GestureTopologySummary
// ---------------------------------------------------------------------------
// WindowGestureRecognizer.TopologyRejectionReason(candidate, template) reads
// a nullable GestureTopologySummary computed by
// GestureFeatureExtractor.SummarizeTopology(sequence.Frames), where
// sequence.Frames is the *specific candidate duration's own slice*
// (1400/1700/2100/2500 ms) of per-frame features, freshly recomputed for
// every candidate window.
//
// measurements::HandTopologySummary (returned by
// hand_unified_feature_frame's summarizeTopology()) is a verified
// field-for-field and formula-for-formula port of the same
// GestureTopologySummary/SummarizeTopology math. This module's
// compareCandidateToTemplate() below reads candidate.topology directly - the
// GestureCandidateWindow's own summarizeTopology() result over just that
// candidate's usable frames (see gesture_candidate_window_history.cpp) - so
// candidate/template topology comparison is scoped exactly like the PR: a
// shorter candidate duration cannot see motion or topology change from
// outside its own span. This previously reused
// HandTopologyHistory::summarize()'s whole-track-window, differently-sourced
// summary instead; that deviation is resolved by GestureCandidateWindow
// carrying its own candidate-local topology.
//
// A null GestureTopologySummary in the PR (candidate/template topology
// unavailable) maps to HandTopologySummary::valid == false here.
//
// ---------------------------------------------------------------------------
// Constants ported from GestureTemplateFactory (thresholds referenced by
// TopologyRejectionReason).
// ---------------------------------------------------------------------------
inline constexpr float kMinimumTopologyChangeScore = 0.32F;
inline constexpr float kMinimumPalmTravel = 0.22F;
inline constexpr float kMinimumPalmOrientationRangeRadians = 0.45F;
inline constexpr float kMinimumPalmTurnScore = 0.55F;
inline constexpr float kMinimumPalmCompressionDrop = 0.30F;
inline constexpr float kMinimumHandScaleRatioRange = 0.22F;
inline constexpr float kMinimumTranslationDistance = 0.22F;

// Constants ported from WindowGestureRecognizer.
inline constexpr float kConfidenceThreshold = 0.82F;
inline constexpr float kWarpRatioLimit = 2.8F;
inline constexpr float kReverseDtwMargin = 0.94F;

// Ported WindowGestureRecognizer.TopologyRejectionReason. Returns an empty
// string when no gate rejects the candidate (including when `template` has
// too little topology change to be a specific gate, mirroring the PR's
// `template.TopologyChangeScore < MinimumTopologyChangeScore` bypass), or a
// human-readable rejection reason otherwise. `candidate`/`template` map the
// PR's nullable GestureTopologySummary via HandTopologySummary::valid (see
// the module-level mapping note above).
[[nodiscard]] std::string topologyRejectionReason(
    const measurements::HandTopologySummary& candidate,
    const measurements::HandTopologySummary& templateSummary) noexcept;

// One step of a reconstructed DTW alignment path. Mirrors the PR's
// GestureDtwPoint(CandidateIndex, TemplateIndex).
struct DtwPoint
{
    std::size_t candidateIndex{0};
    std::size_t templateIndex{0};
};

// Mirrors the PR's GestureScoreBreakdown record exactly, field for field.
// handednessScore and translationScore are always 0: the PR's
// FeatureFrameDistance assigns `var handedness = 0f;` and
// `var translation = 0f;` and never populates them (dead terminology kept in
// the weighted sum for a future feature, not a bug). This port preserves
// that exactly rather than "fixing" it, per the porting brief.
struct ScoreBreakdown
{
    float jointScore{0.0F};
    float boneScore{0.0F};
    float curlScore{0.0F};
    float fingerStateScore{0.0F};
    float spacingScore{0.0F};
    float motionScore{0.0F};
    float palmTurnScore{0.0F};
    float depthScore{0.0F};
    float handednessScore{0.0F};
    float sizeScore{0.0F};
    float translationScore{0.0F};
    float totalScore{0.0F};
};

// Ported WindowGestureRecognizer.FeatureFrameDistance: the single-frame
// weighted distance (and its component breakdown) between one candidate and
// one template UnifiedFeatureFrame. Weights: joint 0.18, bone 0.12, curl
// 0.08, fingerState 0.20, spacing 0.04, motion 0.10 (itself
// |velocity delta| * 0.12), palmTurn 0.12, depth 0.04, handedness 0.06 (dead,
// always 0), size 0.04, translation 0.02 (dead, always 0).
[[nodiscard]] ScoreBreakdown featureFrameDistance(
    const measurements::UnifiedFeatureFrame& candidate,
    const measurements::UnifiedFeatureFrame& templateFrame) noexcept;

struct DtwResult
{
    // Per-alignment-step average cost (costs[n,m] / path.size()), or
    // FLT_MAX when either sequence is empty or the Sakoe-Chiba band leaves
    // the (n, m) corner unreachable.
    float score{0.0F};
    std::vector<DtwPoint> path;
    // Average FeatureFrameDistance breakdown over the reconstructed path;
    // all-zero (with totalScore == score's sentinel) when path is empty.
    ScoreBreakdown breakdown;
};

// Ported WindowGestureRecognizer.BoundedDtw: a Sakoe-Chiba banded DTW over
// FeatureFrameDistance, with band radius max(bandRadius, |n - m|) so the
// (n, m) corner is always reachable, and path reconstruction that prefers
// diagonal, then up (consuming a candidate frame), then left (consuming a
// template frame) on ties - exactly the PR's
// `diagonal <= up && diagonal <= left` / `up <= left` tie-break order.
[[nodiscard]] DtwResult boundedDtw(
    const std::vector<measurements::UnifiedFeatureFrame>& candidate,
    const std::vector<measurements::UnifiedFeatureFrame>& templateFrames,
    std::size_t bandRadius) noexcept;

struct ComparisonResult
{
    // Forward DTW score. FLT_MAX when rejected before a forward DTW could
    // run (too few frames, opposite time/motion direction).
    float score{0.0F};
    bool eligible{false};
    // Empty only when eligible; otherwise a human-readable rejection reason
    // mirroring the PR's GestureTemplateComparison.Reason strings.
    std::string reason;
    float warpRatio{0.0F};
    // False until a forward DTW actually runs (mirrors the PR's nullable
    // `float? WarpRatio`, which stays null for the same early rejections).
    bool hasWarpRatio{false};
    // Time-reversed-template DTW score, computed alongside the forward score
    // by the same reversed-fits-better gate inside compareUnifiedSequences
    // (not a separate recomputation). Diagnostic only: the PR never exposes
    // this value outside its rejection-reason string; hasReverseScore is
    // false for the same early rejections as hasWarpRatio (too few frames,
    // opposite time/motion direction).
    float reverseScore{0.0F};
    bool hasReverseScore{false};
    // Forward DTW path/breakdown. Empty/zero for early rejections that never
    // reach a forward DTW (too few frames, opposite time/motion direction);
    // populated (with the forward DTW's values, even when reversed fits
    // better or warp is excessive) once a forward DTW has run, matching the
    // PR's GestureTemplateComparison which always carries the forward dtw's
    // Path/Breakdown from that point on.
    std::vector<DtwPoint> path;
    ScoreBreakdown breakdown;
};

// Ported WindowGestureRecognizer.CompareUnified: temporal-direction and
// opposite-motion-direction pre-gates, forward vs. time-reversed-template DTW
// (rejecting when the reverse fits at least as well, within
// kReverseDtwMargin), the warp-ratio eligibility ceiling (kWarpRatioLimit),
// and the final DTW-score-vs-confidence-threshold eligibility check
// (kConfidenceThreshold, via confidenceToScore). candidateFrames/
// templateFrames are typically each exactly
// measurements::kUnifiedSequenceLength (32) frames (a GestureCandidateWindow's
// unifiedSequence and a stored template's resampled sequence), but this
// function itself is length-agnostic, matching the PR's
// IReadOnlyList<GestureFeatureFrame> signature.
[[nodiscard]] ComparisonResult compareUnifiedSequences(
    const std::vector<measurements::UnifiedFeatureFrame>& candidateFrames,
    const std::vector<measurements::UnifiedFeatureFrame>& templateFrames) noexcept;

// Ported WindowGestureRecognizer.ScoreToConfidence /
// (private) ConfidenceToScore.
[[nodiscard]] float scoreToConfidence(float score) noexcept;
[[nodiscard]] float confidenceToScore(float confidence) noexcept;

// An in-memory template sequence sufficient for comparison only: a fixed
// 32-point unified sequence plus the topology summary
// topologyRejectionReason gates against. This is deliberately not the PR's
// full GestureTemplate/GestureDefinition (no recording provenance, no
// persistence, no display metadata) - template recording, quality gates,
// storage, and lifecycle remain a separate future step; this struct exists
// only so the DTW/topology comparison primitives above have a concrete,
// testable "second operand" without requiring that later step first.
struct UnifiedSequenceTemplate
{
    std::array<measurements::UnifiedFeatureFrame, measurements::kUnifiedSequenceLength>
        frames{};
    measurements::HandTopologySummary topology{};
    measurements::GestureVisualizationSequence visualization{};
};

// One end-to-end comparison outcome: the topology gate runs first (mirroring
// FindBestMatch, which never runs CompareUnified/BoundedDtw for a
// topologically rejected candidate), and DTW comparison runs only when the
// topology gate passes.
struct RecognitionOutcome
{
    // Empty when the topology gate passed (regardless of whether the
    // subsequent DTW comparison was eligible).
    std::string topologyRejectionReason;
    // Meaningful only when topologyRejectionReason is empty; otherwise a
    // sentinel matching the PR's synthetic
    // `new GestureTemplateComparison(float.MaxValue, false, topologyReason,
    // null, [], null)` (score = FLT_MAX, eligible = false, reason mirrors
    // topologyRejectionReason, no path/warp ratio).
    ComparisonResult comparison;
};

// Convenience entry point consuming the existing measurement-layer outputs
// directly: `candidate` is a GestureCandidateWindowHistory-produced
// candidate window (its fixed unifiedSequence is read as-is, and its own
// candidate-local `topology` field is the topology gate's candidate operand -
// see the module-level mapping note above); `templateSequence` is the
// in-memory template to compare against.
[[nodiscard]] RecognitionOutcome compareCandidateToTemplate(
    const measurements::GestureCandidateWindow& candidate,
    const UnifiedSequenceTemplate& templateSequence) noexcept;
}
