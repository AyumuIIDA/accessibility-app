#include "HandInput/Recognition/gesture_recording_session.h"

#include "HandInput/Measurements/hand_unified_feature_frame.h"
#include "HandInput/Recognition/gesture_template_repository.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <utility>

namespace ryoiki::hand_input::recognition
{
namespace
{
GestureRecordingProvenance makeProvenance(
    const std::vector<TwoHandFrameSetObservation>& source,
    const std::uint32_t definitionId, const std::uint32_t takeIndex)
{
    GestureRecordingProvenance value;
    value.takeIndex = takeIndex;
    value.capturedAtUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    value.sourceId = std::to_string(definitionId) + "-" + std::to_string(takeIndex)
        + "-" + std::to_string(value.capturedAtUs);
    if (source.empty()) return value;
    const auto firstTimestamp = source.front().timestampUs;
    const auto lastTimestamp = source.back().timestampUs;
    value.durationMs = lastTimestamp >= firstTimestamp
        ? static_cast<double>(lastTimestamp - firstTimestamp) / 1000.0 : 0.0;
    std::array<float, 2> scaleSums{};
    std::array<std::uint32_t, 2> scaleCounts{};
    std::array<hand_perception::Landmark3f, 2> firstCenters{};
    std::array<bool, 2> hasFirst{};
    float confidenceSum = 0.0F;
    std::uint32_t confidenceCount = 0;
    for (const auto& frame : source)
    {
        if (frame.handCount > 0) ++value.quality.validLandmarkFrameCount;
        bool highConfidence = false;
        for (std::size_t index = 0; index < frame.handCount; ++index)
        {
            const auto slot = frame.hands[index].handedness < 0.5F ? 0U : 1U;
            const auto& points = frame.hands[index].imageLandmarks;
            const float dx = points[9].x - points[0].x;
            const float dy = points[9].y - points[0].y;
            scaleSums[slot] += std::sqrt(dx * dx + dy * dy);
            ++scaleCounts[slot];
            if (!hasFirst[slot])
            {
                firstCenters[slot] = {0.5F * (points[0].x + points[9].x),
                    0.5F * (points[0].y + points[9].y),
                    0.5F * (points[0].z + points[9].z)};
                hasFirst[slot] = true;
            }
            confidenceSum += frame.confidences[index];
            ++confidenceCount;
            highConfidence = highConfidence
                || frame.confidences[index] >= measurements::kGestureMinimumUsableConfidence;
        }
        if (highConfidence) ++value.quality.highConfidenceFrameCount;
    }
    std::array<float, 2> scales{1.0F, 1.0F};
    for (std::size_t slot = 0; slot < scales.size(); ++slot)
        if (scaleCounts[slot] != 0)
            scales[slot] = (std::max)(1.0F, scaleSums[slot] / static_cast<float>(scaleCounts[slot]));
    value.frames.reserve(source.size());
    for (const auto& sourceFrame : source)
    {
        GestureRecordingFrame frame;
        frame.timeOffsetMs = sourceFrame.timestampUs >= firstTimestamp
            ? static_cast<double>(sourceFrame.timestampUs - firstTimestamp) / 1000.0 : 0.0;
        frame.handCount = static_cast<std::uint32_t>(sourceFrame.handCount);
        for (std::size_t index = 0; index < sourceFrame.handCount; ++index)
        {
            const auto slot = sourceFrame.hands[index].handedness < 0.5F ? 0U : 1U;
            frame.confidence[slot] = sourceFrame.confidences[index];
            frame.handedness[slot] = sourceFrame.hands[index].handedness;
            for (std::size_t point = 0; point < measurements::kLandmarkCount; ++point)
            {
                const auto& input = sourceFrame.hands[index].imageLandmarks[point];
                frame.normalizedSkeletons[slot][point] = {
                    (input.x - firstCenters[slot].x) / scales[slot],
                    (input.y - firstCenters[slot].y) / scales[slot], input.z / scales[slot]};
            }
        }
        value.frames.push_back(std::move(frame));
    }
    value.averageConfidence = confidenceCount == 0 ? 0.0F
        : confidenceSum / static_cast<float>(confidenceCount);
    value.quality.accepted = true;
    value.quality.sourceFrameCount = static_cast<std::uint32_t>(source.size());
    const double seconds = (std::max)(0.001, value.durationMs / 1000.0);
    value.quality.effectiveFps = static_cast<float>(source.size() / seconds);
    value.quality.usableEffectiveFps = static_cast<float>(
        value.quality.highConfidenceFrameCount / seconds);
    return value;
}
}
void GestureRecordingSession::submitCommand(
    const GestureRecordingCommandKind kind, const std::uint32_t templateId) noexcept
{
    std::lock_guard lock{mailboxMutex_};
    pendingCommand_ = kind;
    pendingTemplateId_ = templateId;
}

void GestureRecordingSession::beginSessionLocked(const std::uint32_t templateId) noexcept
{
    acceptedTrials_ = {};
    acceptedProvenance_ = {};
    acceptedTakeCount_ = 0;
    attemptCount_ = 0;
    sessionTemplateId_ = templateId;
    sessionKind_ = GestureTrialKind::OneHandDynamic;
    beginTakeLocked();
}

void GestureRecordingSession::beginTakeLocked() noexcept
{
    buffer_.reset();
    twoHandFrames_.clear();
    capturedFrameSetCount_ = 0;
    lockedTrackId_ = 0;
    missingFrames_ = 0;
    beganFrameId_ = 0;
    beganTimestampUs_ = 0;
    lastFrameId_ = 0;
    lastTimestampUs_ = 0;
    sampleCount_ = 0;
    usableSampleCount_ = 0;
    lastResultTemplateId_ = 0;
    rejectionReason_.clear();
    currentTake_ = acceptedTakeCount_ + 1;
    ++attemptCount_;
    state_ = GestureRecordingState::AwaitingHand;
}

void GestureRecordingSession::cancelLocked(
    std::string reason, const GestureRecordingState terminalState) noexcept
{
    rejectionReason_ = std::move(reason);
    state_ = terminalState;
}

void GestureRecordingSession::finishLocked(GestureTemplateRegistry& templateRegistry,
    GestureTemplateRepository* repository, const std::string& definitionName,
    const bool persistenceRequired) noexcept
{
    lastResultTemplateId_ = sessionTemplateId_;

    GestureTrial accepted{};
    if (hasPredominantTwoHandCoverage(twoHandFrames_, capturedFrameSetCount_))
    {
        const auto creation = createTwoHandTemplate(twoHandFrames_);
        if (!creation.value.valid)
        {
            rejectionReason_ = creation.rejectionReason;
            state_ = GestureRecordingState::Rejected;
            return;
        }
        accepted = GestureTrial{GestureTrialKind::TwoHandDynamic, creation.value};
    }
    else
    {
        using CandidateArray = std::array<
            measurements::GestureCandidateWindow,
            measurements::kGestureCandidateDurationsMs.size()>;
        CandidateArray windows{};
        const auto written = buffer_.buildCandidateWindows(windows);
        if (written == 0)
        {
            rejectionReason_ = "No eligible candidate window is available yet; keep recording longer.";
            state_ = GestureRecordingState::Rejected;
            return;
        }
        const auto creation = createTemplateFromCandidateWindow(windows[written - 1]);
        if (!creation.created)
        {
            rejectionReason_ = creation.rejectionReason;
            state_ = GestureRecordingState::Rejected;
            return;
        }
        accepted = GestureTrial{GestureTrialKind::OneHandDynamic, creation.sequence};
    }
    if (acceptedTakeCount_ > 0 && accepted.kind != sessionKind_)
    {
        rejectionReason_ = "All three accepted takes must use the same hand count.";
        state_ = GestureRecordingState::Rejected;
        return;
    }
    sessionKind_ = accepted.kind;
    acceptedProvenance_[acceptedTakeCount_] = makeProvenance(
        twoHandFrames_, sessionTemplateId_, acceptedTakeCount_ + 1);
    acceptedTrials_[acceptedTakeCount_] = std::move(accepted);
    ++acceptedTakeCount_;
    if (acceptedTakeCount_ < kRequiredGestureTemplateCount)
    {
        rejectionReason_.clear();
        // Public progress is always the one-based take the user should act
        // on next. acceptedTakeCount_ alone remains the zero-based array
        // insertion index for acceptedTemplates_.
        currentTake_ = acceptedTakeCount_ + 1;
        state_ = GestureRecordingState::AwaitingNextTake;
        return;
    }

    std::string persistenceError;
    if (persistenceRequired && repository == nullptr)
        persistenceError = "Gesture persistence is unavailable.";
    const bool committed = persistenceRequired && repository == nullptr ? false : repository != nullptr
        ? repository->appendThreeTakeSet(templateRegistry, sessionTemplateId_,
            definitionName, acceptedTrials_, acceptedProvenance_, persistenceError)
        : templateRegistry.restoreTrials(sessionTemplateId_,
            std::vector<GestureTrial>{acceptedTrials_.begin(), acceptedTrials_.end()});
    if (!committed)
    {
        rejectionReason_ = persistenceError.empty()
            ? "The template registry is full." : "Persistence failed: " + persistenceError;
        state_ = GestureRecordingState::Rejected;
        return;
    }

    rejectionReason_.clear();
    state_ = GestureRecordingState::Completed;
}

GestureRecordingStatus GestureRecordingSession::processFrame(
    const hand_perception::HandPerceptionResult& perception,
    const std::uint64_t frameId,
    const std::uint64_t timestampUs,
    GestureTemplateRegistry& templateRegistry,
    GestureTemplateRepository* repository,
    const std::string& definitionName,
    const bool persistenceRequired) noexcept
{
    GestureRecordingCommandKind command = GestureRecordingCommandKind::None;
    std::uint32_t templateId = 0;
    {
        std::lock_guard lock{mailboxMutex_};
        command = pendingCommand_;
        templateId = pendingTemplateId_;
        pendingCommand_ = GestureRecordingCommandKind::None;
        pendingTemplateId_ = 0;
    }

    switch (command)
    {
    case GestureRecordingCommandKind::Begin:
        if (state_ == GestureRecordingState::Rejected
            && templateId == sessionTemplateId_
            && acceptedTakeCount_ == kRequiredGestureTemplateCount)
        {
            std::string persistenceError;
            if (persistenceRequired && repository == nullptr)
                persistenceError = "Gesture persistence is unavailable.";
            const bool committed = persistenceRequired && repository == nullptr ? false : repository != nullptr
                ? repository->appendThreeTakeSet(templateRegistry, sessionTemplateId_,
                    definitionName, acceptedTrials_, acceptedProvenance_, persistenceError)
                : templateRegistry.restoreTrials(sessionTemplateId_,
                    std::vector<GestureTrial>{acceptedTrials_.begin(), acceptedTrials_.end()});
            if (committed) { rejectionReason_.clear(); state_ = GestureRecordingState::Completed; }
            else rejectionReason_ = persistenceError.empty() ? "Persistence retry failed." : persistenceError;
            break;
        }
        if ((state_ == GestureRecordingState::AwaitingNextTake
                || state_ == GestureRecordingState::Rejected
                || state_ == GestureRecordingState::TrackLost)
            && templateId == sessionTemplateId_
            && acceptedTakeCount_ < kRequiredGestureTemplateCount)
        {
            beginTakeLocked();
        }
        else
        {
            beginSessionLocked(templateId);
        }
        break;
    case GestureRecordingCommandKind::Cancel:
        if (state_ != GestureRecordingState::Idle)
        {
            cancelLocked("Cancelled by request.", GestureRecordingState::Cancelled);
            acceptedTrials_ = {};
            acceptedProvenance_ = {};
            acceptedTakeCount_ = 0;
            attemptCount_ = 0;
            currentTake_ = 0;
            sessionTemplateId_ = 0;
            sessionKind_ = GestureTrialKind::OneHandDynamic;
            buffer_.reset();
            twoHandFrames_.clear();
            capturedFrameSetCount_ = 0;
            lockedTrackId_ = 0;
            missingFrames_ = 0;
            beganFrameId_ = 0;
            beganTimestampUs_ = 0;
            lastFrameId_ = 0;
            lastTimestampUs_ = 0;
            sampleCount_ = 0;
            usableSampleCount_ = 0;
            lastResultTemplateId_ = 0;
        }
        break;
    case GestureRecordingCommandKind::Finish:
        if (state_ == GestureRecordingState::Recording)
        {
            finishLocked(templateRegistry, repository, definitionName, persistenceRequired);
        }
        else if (state_ == GestureRecordingState::AwaitingHand)
        {
            lastResultTemplateId_ = sessionTemplateId_;
            rejectionReason_ = "No hand was captured; recording never started.";
            state_ = GestureRecordingState::Rejected;
        }
        break;
    case GestureRecordingCommandKind::None:
        break;
    }

    if (state_ == GestureRecordingState::AwaitingHand)
    {
        std::size_t bestIndex = perception.handCount;
        for (std::size_t index = 0; index < perception.handCount; ++index)
            if (perception.hands[index].detected
                && (bestIndex == perception.handCount
                    || perception.hands[index].confidence > perception.hands[bestIndex].confidence))
                bestIndex = index;
        if (bestIndex < perception.handCount
            && perception.hands[bestIndex].confidence >= measurements::kGestureMinimumUsableConfidence)
        {
            lockedTrackId_ = perception.trackIds[bestIndex];
            beganFrameId_ = frameId;
            beganTimestampUs_ = timestampUs;
            missingFrames_ = 0;
            state_ = GestureRecordingState::Recording;
        }
    }

    if (state_ == GestureRecordingState::Recording)
    {
        TwoHandFrameSetObservation frameSet{};
        frameSet.timestampUs = timestampUs;
        frameSet.handCount = (std::min)(perception.handCount, std::size_t{2});
        std::array<std::size_t, hand_perception::kMaxPerceivedHands> order{};
        for (std::size_t index = 0; index < perception.handCount; ++index) order[index] = index;
        std::sort(order.begin(), order.begin() + perception.handCount,
            [&perception](const auto left, const auto right)
            { return perception.hands[left].confidence > perception.hands[right].confidence; });
        for (std::size_t index = 0; index < frameSet.handCount; ++index)
        {
            const auto sourceIndex = order[index];
            frameSet.hands[index] = measurements::makeUnifiedFeatureObservation(
                perception.hands[sourceIndex], timestampUs);
            frameSet.confidences[index] = perception.hands[sourceIndex].confidence;
        }
        if (twoHandFrames_.size() >= kMaxGestureRecordingFrames)
        {
            cancelLocked("Recording exceeded the 360-frame native provenance limit; finish sooner.",
                GestureRecordingState::Rejected);
        }
        else
        {
            twoHandFrames_.push_back(std::move(frameSet));
        }
        ++capturedFrameSetCount_;
        std::size_t observedIndex = perception.handCount;
        for (std::size_t index = 0; index < perception.handCount; ++index)
        {
            if (perception.trackIds[index] == lockedTrackId_)
            {
                observedIndex = index;
                break;
            }
        }

        if (observedIndex < perception.handCount)
        {
            missingFrames_ = 0;
            const auto& observedHand = perception.hands[observedIndex];
            const bool usable = observedHand.detected
                && observedHand.confidence >= measurements::kGestureMinimumUsableConfidence;
            buffer_.push(
                measurements::makeUnifiedFeatureObservation(observedHand, timestampUs), usable);
            ++sampleCount_;
            if (usable)
            {
                ++usableSampleCount_;
            }
            lastFrameId_ = frameId;
            lastTimestampUs_ = timestampUs;
        }
        else if (countUsableTwoHandFrames(twoHandFrames_) == 0)
        {
            // Tolerate the same short missing gap the rolling per-track
            // histories tolerate; do not rebind to a different track.
            buffer_.noteMissingObservation();
            ++missingFrames_;
            if (missingFrames_ > measurements::kHandTopologyGapToleranceFrames)
            {
                cancelLocked(
                    "The recorded hand was lost partway through recording; start again.",
                    GestureRecordingState::TrackLost);
            }
        }
    }

    GestureRecordingStatus status{};
    status.state = state_;
    status.trackId = static_cast<std::uint32_t>(lockedTrackId_);
    status.beganFrameId = beganFrameId_;
    status.beganTimestampUs = beganTimestampUs_;
    status.lastFrameId = lastFrameId_;
    status.lastTimestampUs = lastTimestampUs_;
    status.sampleCount = sampleCount_;
    status.usableSampleCount = usableSampleCount_;
    status.lastResultTemplateId = lastResultTemplateId_;
    status.currentTake = currentTake_;
    status.acceptedTakeCount = acceptedTakeCount_;
    status.requiredTakeCount = static_cast<std::uint32_t>(kRequiredGestureTemplateCount);
    status.attemptCount = attemptCount_;
    status.rejectionReason = rejectionReason_;
    return status;
}

bool GestureRecordingSession::isActive() const noexcept
{
    return state_ == GestureRecordingState::AwaitingHand
        || state_ == GestureRecordingState::Recording;
}
}
