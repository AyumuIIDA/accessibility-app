#pragma once

#include "HandInput/Measurements/gesture_candidate_window_history.h"
#include "HandInput/Recognition/gesture_template_registry.h"
#include "HandInput/Recognition/gesture_template_repository.h"
#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ryoiki::hand_input::recognition
{
// Explicit begin/finish/cancel gesture-recording workflow (native ABI
// version 23+). Replaces the previous "stop time, read whatever is in the
// live rolling per-track GestureCandidateWindowHistory" registration path:
// that path could include motion from before the user actually meant to
// start recording, which is why the former WPF workflow needed a 1.5 s
// countdown before Start actually counted (see doc/native-vision-runtime.md
// history). This session instead owns its own dedicated, non-rolling
// GestureCandidateWindowHistory instance that starts empty and is fed only
// with samples observed after Begin locks onto a hand.
//
// Per doc/hand-input-architecture.md, the UI supplies intent/configuration
// only and never selects a perception track: Begin waits for exactly one
// visible, usable hand and locks the session onto that
// hand_perception::HandTrackId internally. The locked track is never
// rebound mid-recording; a short observation gap is tolerated (the same
// grace window HandTopologyHistory/GestureCandidateWindowHistory already
// use), but losing the track beyond that grace window ends the take rather
// than silently continuing with a different hand.
//
// Each accepted Finish retains one separate template. Matching stays
// inactive until three takes have passed the unchanged PR quality gates. A
// failed take keeps earlier accepted takes and retries the same one-based
// take index; the third acceptance atomically commits all three. They are
// never averaged, matching RequiredTemplateCount=3 in the PR.
//
// Finish reuses the existing
// GestureCandidateWindowHistory::buildCandidateWindows,
// createTemplateFromCandidateWindow, and GestureTemplateRegistry algorithms
// (see gesture_template_registry.h) - this module only adds the explicit
// session lifecycle around them, exactly as the old stop-time path did, so
// template acceptance/quality semantics are unchanged.
//
// Thread-safety: Begin/Finish/Cancel commands may be submitted from any
// thread (the WPF UI thread) through submitCommand's small mutex-protected
// mailbox: one coherent pending command (kind + template ID), not several
// independent atomics. processFrame is perception-worker-only, mirroring
// every other per-frame measurement/recognition stage in this runtime; it
// drains at most one pending command per call and returns a plain status
// value the caller (ryoiki_native.cpp) copies into its own
// mutex-guarded polling snapshot, the same pattern already used for
// gestureRecognitionSnapshot/gestureRecognitionMutex.
enum class GestureRecordingState : std::uint32_t
{
    Idle = 0,
    AwaitingHand = 1,
    Recording = 2,
    Completed = 3,
    Rejected = 4,
    Cancelled = 5,
    TrackLost = 6,
    AwaitingNextTake = 7,
};

enum class GestureRecordingCommandKind : std::uint32_t
{
    None = 0,
    Begin = 1,
    Finish = 2,
    Cancel = 3,
};

// Latest-only status snapshot, safe to copy from any thread once returned
// by processFrame.
struct GestureRecordingStatus
{
    GestureRecordingState state{GestureRecordingState::Idle};
    // The perception-graph track the session locked onto; 0 before a track
    // is locked. Diagnostic only - the caller never chooses this value.
    std::uint32_t trackId{0};
    std::uint64_t beganFrameId{0};
    std::uint64_t beganTimestampUs{0};
    std::uint64_t lastFrameId{0};
    std::uint64_t lastTimestampUs{0};
    std::uint32_t sampleCount{0};
    std::uint32_t usableSampleCount{0};
    // The template ID supplied at Begin, echoed back once Finish resolves
    // (Completed or Rejected), so a caller can confirm which take a result
    // belongs to. 0 before any Finish attempt.
    std::uint32_t lastResultTemplateId{0};
    std::uint32_t currentTake{0};
    std::uint32_t acceptedTakeCount{0};
    std::uint32_t requiredTakeCount{static_cast<std::uint32_t>(kRequiredGestureTemplateCount)};
    std::uint32_t attemptCount{0};
    std::string rejectionReason;
};

class GestureRecordingSession final
{
public:
    // Callable from any thread. templateId is only meaningful for Begin
    // (remembered for the eventual Finish) and is ignored for Finish/Cancel.
    void submitCommand(GestureRecordingCommandKind kind, std::uint32_t templateId) noexcept;

    // Perception-worker-only. Drains at most one pending command, advances
    // the AwaitingHand -> Recording lock and the per-frame buffer/gap-
    // tolerance bookkeeping, and returns the resulting status.
    [[nodiscard]] GestureRecordingStatus processFrame(
        const hand_perception::HandPerceptionResult& perception,
        std::uint64_t frameId,
        std::uint64_t timestampUs,
        GestureTemplateRegistry& templateRegistry,
        GestureTemplateRepository* repository = nullptr,
        const std::string& definitionName = {}, bool persistenceRequired = false) noexcept;

    // Perception-worker-only (reads worker-owned state_ without locking).
    // True while an on-demand live recognition pass should be suppressed:
    // the session is waiting for a hand or actively recording.
    [[nodiscard]] bool isActive() const noexcept;

private:
    void beginSessionLocked(std::uint32_t templateId) noexcept;
    void beginTakeLocked() noexcept;
    void cancelLocked(std::string reason, GestureRecordingState terminalState) noexcept;
    void finishLocked(GestureTemplateRegistry& templateRegistry,
        GestureTemplateRepository* repository, const std::string& definitionName,
        bool persistenceRequired) noexcept;

    mutable std::mutex mailboxMutex_;
    GestureRecordingCommandKind pendingCommand_{GestureRecordingCommandKind::None};
    std::uint32_t pendingTemplateId_{0};

    GestureRecordingState state_{GestureRecordingState::Idle};
    measurements::GestureCandidateWindowHistory buffer_;
    hand_perception::HandTrackId lockedTrackId_{0};
    std::uint32_t sessionTemplateId_{0};
    std::uint32_t missingFrames_{0};
    std::uint64_t beganFrameId_{0};
    std::uint64_t beganTimestampUs_{0};
    std::uint64_t lastFrameId_{0};
    std::uint64_t lastTimestampUs_{0};
    std::uint32_t sampleCount_{0};
    std::uint32_t usableSampleCount_{0};
    std::uint32_t lastResultTemplateId_{0};
    std::array<GestureTrial, kRequiredGestureTemplateCount> acceptedTrials_{};
    std::array<GestureRecordingProvenance, kRequiredGestureTemplateCount> acceptedProvenance_{};
    std::vector<TwoHandFrameSetObservation> twoHandFrames_;
    std::uint32_t capturedFrameSetCount_{0};
    GestureTrialKind sessionKind_{GestureTrialKind::OneHandDynamic};
    std::uint32_t acceptedTakeCount_{0};
    std::uint32_t currentTake_{0};
    std::uint32_t attemptCount_{0};
    std::string rejectionReason_;
};
}
