#include "HandInput/Recognition/gesture_match_stabilizer.h"

namespace ryoiki::hand_input::recognition
{
GestureMatchStabilizationResult GestureMatchStabilizer::process(
    const GestureMatchCandidate* match) noexcept
{
    if (match == nullptr)
    {
        reset();
        return {};
    }
    if (candidateGestureId_ == match->gestureId) ++candidateCount_;
    else { candidateGestureId_ = match->gestureId; candidateCount_ = 1; }

    const auto elapsed = lastTriggeredAtUs_ == 0 || match->timestampUs < lastTriggeredAtUs_
        ? kGestureCooldownUs : match->timestampUs - lastTriggeredAtUs_;
    const auto cooldownRemaining = elapsed >= kGestureCooldownUs
        ? 0 : kGestureCooldownUs - elapsed;
    const bool duplicate = hasTriggeredSegment_
        && candidateGestureId_ == match->gestureId
        && match->segmentStartUs <= lastTriggeredSegmentEndUs_
        && match->segmentEndUs >= lastTriggeredSegmentStartUs_;

    GestureMatchStabilizationResult result{};
    result.consecutiveCount = candidateCount_;
    result.cooldownRemainingUs = cooldownRemaining;
    if (candidateCount_ < kGestureConsecutiveMatchThreshold)
        result.decision = GestureMatchDecision::WaitingForConsecutiveMatch;
    else if (duplicate)
        result.decision = GestureMatchDecision::DuplicateSegmentSuppressed;
    else if (cooldownRemaining > 0)
        result.decision = GestureMatchDecision::Cooldown;
    else
    {
        result.decision = GestureMatchDecision::Confirmed;
        result.confirmed = true;
        lastTriggeredAtUs_ = match->timestampUs;
        lastTriggeredSegmentStartUs_ = match->segmentStartUs;
        lastTriggeredSegmentEndUs_ = match->segmentEndUs;
        hasTriggeredSegment_ = true;
        candidateCount_ = 0;
        result.consecutiveCount = 0;
    }
    return result;
}

void GestureMatchStabilizer::reset() noexcept
{
    candidateGestureId_ = 0;
    candidateCount_ = 0;
}

void GestureMatchStabilizer::resetForRegistryChange() noexcept
{
    reset();
    lastTriggeredAtUs_ = 0;
    lastTriggeredSegmentStartUs_ = 0;
    lastTriggeredSegmentEndUs_ = 0;
    hasTriggeredSegment_ = false;
}
}
