#pragma once

#include "HandInput/Measurements/gesture_candidate_window_history.h"
#include "HandInput/Measurements/hand_unified_feature_frame.h"
#include "HandInput/Recognition/unified_sequence_gesture_comparator.h"
#include "HandInput/Recognition/multi_hand_gesture_core.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <variant>

namespace ryoiki::hand_input::recognition
{
// Native port of the minimum single-hand template creation/normalization and
// best-match selection needed for real-device validation, from
// GestureTemplateFactory.TryCreate and WindowGestureRecognizer.FindBestMatch
// (src/RyoikiTenkai/Vision/*.cs, commit a5bed23 on origin/pr/1/head). This
// sits behind the Measurement -> Recognition boundary documented in
// doc/hand-input-architecture.md: it is an on-demand comparison/registration
// primitive, not a per-frame Measurement, and is invoked only from the
// throttled recognition scheduling point in ryoiki_native.cpp (see that
// file's perception-loop comment). It does not:
//
//   - persist templates to disk, load/save a GestureDefinition file, or
//     manage recording provenance (GestureDefinition, GestureDefinitionStore,
//     GestureRecording) - there is no existing local recording workflow in
//     this repository's main branch to preserve, so templates are in-memory
//     only, created directly from a live GestureCandidateWindow;
//   - attach display names/persistence to numeric definitions, require
//     consecutive-match confirmation, apply a trigger cooldown,
//     or suppress duplicate segments (WindowGestureRecognizer.Recognize's
//     state machine) - those remain future work, alongside action execution
//     and event publication, which this task explicitly excludes;
//   - fuse two hands' geometry (MultiHandGestureFeatureExtractor) - templates
//     and matching here are single-hand only, per GestureCandidateWindow.
//
// Template creation and best-match selection both build on
// unified_sequence_gesture_comparator's compareCandidateToTemplate as the
// single source of truth for topology-gated DTW comparison; this module only
// adds the "which template(s), and which of this one candidate history's
// four durations" iteration and the recording-acceptance gate on top of it.

// Constants ported from GestureTemplateFactory.TryCreate's recording-
// acceptance gate. Distinct from (and stricter than) the live-matching
// candidate gates in gesture_candidate_window_history.h: a template must be
// a "good enough recording" (>= 1900 ms, >= 32 usable frames, >= 16 usable
// fps), while a live candidate being matched against a stored template only
// needs >= 1400 ms / 25 usable frames.
inline constexpr std::size_t kMinimumTemplateUsableSampleCount = 32;
inline constexpr double kMinimumTemplateDurationMs = 1900.0;
inline constexpr double kMinimumTemplateSampleRateFps = 16.0;
inline constexpr float kMinimumFingerStraightnessRangeForKind = 0.30F;

// Ported GestureFeatureExtractor.DetectKind + GestureTemplateFactory.
// TryCreate's Static -> Dynamic override and HasMeaningfulActionSignal gate,
// applied directly to a GestureCandidateWindow's own candidate-local
// topology/motion fields (see gesture_candidate_window_history.h). Returns
// true when the window would be accepted as a recognizable one-hand dynamic
// gesture template.
[[nodiscard]] bool isRecognizableGestureWindow(
    const measurements::GestureCandidateWindow& window) noexcept;

// Result of attempting to create a template from one candidate window.
// `rejectionReason` mirrors GestureTemplateFactory's failure-reason strings
// (abbreviated to the fields a GestureCandidateWindow retains) and is empty
// exactly when `created` is true.
struct TemplateCreationResult
{
    bool created{false};
    UnifiedSequenceTemplate sequence{};
    std::string rejectionReason;
};

// Ported GestureTemplateFactory.TryCreate, scoped to what a stored template
// needs for DTW comparison (a fixed 32-point sequence plus a candidate-local
// topology summary) rather than the PR's full recording-review record
// (skeleton frames, average confidence, source recording ID). `window` is
// typically the longest eligible candidate from a track's
// GestureCandidateWindowHistory::buildCandidateWindows() output, captured
// on demand when the caller decides "register what is in the rolling buffer
// right now" (see ryoiki_native.cpp's registration request handling).
[[nodiscard]] TemplateCreationResult createTemplateFromCandidateWindow(
    const measurements::GestureCandidateWindow& window) noexcept;

// Bounded, allocation-free-after-construction in-memory definition store.
// Each caller-assigned numeric ID groups separate accepted takes. PR parity
// requires at least three recognizable takes before a definition participates
// in matching; every accepted take remains a separate DTW operand (there is
// no averaged template). Heap-owned backing storage keeps the large bounded
// skeleton/template arrays off callers' stacks.
inline constexpr std::size_t kMaxGestureTemplates = 8;
inline constexpr std::size_t kRequiredGestureTemplateCount = 3;
inline constexpr std::size_t kMaxGestureTemplateTrialsPerDefinition = 9;

enum class GestureTrialKind : std::uint32_t
{
    OneHandDynamic = 0,
    TwoHandDynamic = 1
};

using GestureTrialPayload = std::variant<UnifiedSequenceTemplate, TwoHandTemplate>;

struct GestureTrial
{
    GestureTrialKind kind{GestureTrialKind::OneHandDynamic};
    GestureTrialPayload payload{UnifiedSequenceTemplate{}};
};

struct GestureTemplateRecord
{
    std::uint32_t templateId{0};
    bool active{false};
    std::size_t trialCount{0};
    std::array<GestureTrial, kMaxGestureTemplateTrialsPerDefinition> trials{};
};

class GestureTemplateRegistry final
{
public:
    GestureTemplateRegistry();
    ~GestureTemplateRegistry();
    GestureTemplateRegistry(const GestureTemplateRegistry&) = delete;
    GestureTemplateRegistry& operator=(const GestureTemplateRegistry&) = delete;
    // Returns false (no-op) when templateId is new and the registry is
    // already at kMaxGestureTemplates capacity. Registering an existing ID
    // always succeeds (replacement, not insertion, needs no free slot).
    bool registerTemplate(std::uint32_t templateId, UnifiedSequenceTemplate sequence) noexcept;
    // Atomically appends one accepted three-take session. No partial write is
    // made when the definition/trial capacity is insufficient.
    bool appendTemplateSet(
        std::uint32_t templateId,
        const std::array<UnifiedSequenceTemplate, kRequiredGestureTemplateCount>& templates) noexcept;
    bool appendTwoHandTemplateSet(
        std::uint32_t templateId,
        const std::array<TwoHandTemplate, kRequiredGestureTemplateCount>& templates) noexcept;
    [[nodiscard]] bool canAppendTemplateSet(std::uint32_t templateId) const noexcept;
    bool restoreDefinition(std::uint32_t templateId,
        const std::vector<UnifiedSequenceTemplate>& templates) noexcept;
    bool restoreTrials(std::uint32_t templateId,
        const std::vector<GestureTrial>& trials) noexcept;
    bool removeTemplate(std::uint32_t templateId) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const GestureTemplateRecord* find(std::uint32_t templateId) const noexcept;
    [[nodiscard]] const std::array<GestureTemplateRecord, kMaxGestureTemplates>& records() const noexcept;

private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
};

// One (candidate duration, template) comparison attempt, retained for
// diagnostics regardless of eligibility - satisfies the "eligibility/
// rejection reasoning" and "candidate duration" diagnostic requirements for
// every attempt, not only the eventual best match.
struct GestureTemplateAttempt
{
    std::uint32_t templateId{0};
    std::size_t templateTrialIndex{0};
    std::uint32_t candidateDurationMs{0};
    std::size_t candidateIndex{0};
    RecognitionOutcome outcome{};
};

struct BestMatchResult
{
    bool matched{false};
    std::uint32_t templateId{0};
    std::uint32_t candidateDurationMs{0};
    // Copy of the matched candidate window, so a caller can read
    // usableFrameCount/sourceFrameCount/activeSegment/duration without
    // re-running buildCandidateWindows.
    measurements::GestureCandidateWindow candidate{};
    ComparisonResult comparison{};
    UnifiedSequenceTemplate templateSequence{};
    std::size_t templateTrialIndex{0};
    std::vector<GestureTemplateAttempt> attempts;
};

// Ported WindowGestureRecognizer.FindBestMatch's per-template x per-candidate
// loop: definitions with fewer than three takes are inactive; for every
// active definition each take is compared with every candidate duration and
// the overall lowest eligible score wins. Consecutive-match/cooldown state is
// still outside this stateless primitive.
// `candidates`/`candidateCount` is typically a
// GestureCandidateWindowHistory::buildCandidateWindows() result.
[[nodiscard]] BestMatchResult findBestMatch(
    const std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>&
        candidates,
    std::size_t candidateCount,
    const GestureTemplateRegistry& registry) noexcept;
}
