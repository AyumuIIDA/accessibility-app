#include "HandInput/Recognition/timed_state_stabilizer.h"

#include <algorithm>
#include <cmath>

namespace ryoiki::hand_input::recognition
{
namespace
{
bool elapsed(
    const std::uint64_t nowUs,
    const std::uint64_t sinceUs,
    const std::uint64_t durationUs) noexcept
{
    return nowUs >= sinceUs && nowUs - sinceUs >= durationUs;
}
}

TimedStateStabilizer::TimedStateStabilizer(const TimedStateConfig config) noexcept
    : config_{config}
{
}

HandStateResult TimedStateStabilizer::update(
    const std::uint32_t stateId,
    const bool inputValid,
    const bool conditionMet,
    const float confidence,
    const float inputQuality,
    const std::uint64_t frameId,
    const std::uint64_t timestampUs) noexcept
{
    HandStateResult result{};
    result.id = stateId;
    result.phase = phase_;
    result.confidence = std::isfinite(confidence)
        ? std::clamp(confidence, 0.0F, 1.0F)
        : 0.0F;
    result.inputQuality = std::isfinite(inputQuality)
        ? std::clamp(inputQuality, 0.0F, 1.0F)
        : 0.0F;
    result.beganFrameId = beganFrameId_;
    result.currentFrameId = frameId;
    result.timestampUs = timestampUs;

    if (!inputValid)
    {
        if (phase_ == HandStatePhase::Inactive)
        {
            return result;
        }
        if (missingSinceUs_ == 0)
        {
            missingSinceUs_ = timestampUs;
        }
        if (!elapsed(timestampUs, missingSinceUs_, config_.missingGraceUs))
        {
            result.flags = HandStateFlags::Stale;
            return result;
        }

        reset();
        result.phase = HandStatePhase::Inactive;
        result.transition = HandStateTransition::Cancelled;
        result.beganFrameId = 0;
        return result;
    }

    missingSinceUs_ = 0;
    if (conditionMet)
    {
        exitSinceUs_ = 0;
        if (phase_ == HandStatePhase::Inactive)
        {
            phase_ = HandStatePhase::Candidate;
            candidateSinceUs_ = timestampUs;
        }
        if (phase_ == HandStatePhase::Candidate
            && elapsed(timestampUs, candidateSinceUs_, config_.enterDurationUs))
        {
            phase_ = HandStatePhase::Active;
            beganFrameId_ = frameId;
            result.transition = HandStateTransition::Began;
        }
    }
    else if (phase_ == HandStatePhase::Candidate)
    {
        phase_ = HandStatePhase::Inactive;
        candidateSinceUs_ = 0;
    }
    else if (phase_ == HandStatePhase::Active)
    {
        if (exitSinceUs_ == 0)
        {
            exitSinceUs_ = timestampUs;
        }
        if (elapsed(timestampUs, exitSinceUs_, config_.exitDurationUs))
        {
            phase_ = HandStatePhase::Inactive;
            candidateSinceUs_ = 0;
            exitSinceUs_ = 0;
            beganFrameId_ = 0;
            result.transition = HandStateTransition::Ended;
        }
        else
        {
            result.flags = HandStateFlags::ExitPending;
        }
    }

    result.phase = phase_;
    result.beganFrameId = beganFrameId_;
    return result;
}

void TimedStateStabilizer::reset() noexcept
{
    phase_ = HandStatePhase::Inactive;
    candidateSinceUs_ = 0;
    exitSinceUs_ = 0;
    missingSinceUs_ = 0;
    beganFrameId_ = 0;
}
}
