#include "HandInput/Recognition/swipe_event_recognizer.h"

#include <algorithm>
#include <cmath>

namespace ryoiki::hand_input::recognition
{
namespace
{
float distance(
    const float firstX,
    const float firstY,
    const float secondX,
    const float secondY) noexcept
{
    return std::hypot(secondX - firstX, secondY - firstY);
}

float smoothStep(const float low, const float high, const float value) noexcept
{
    if (!std::isfinite(value) || high <= low) return 0.0F;
    const float t = std::clamp((value - low) / (high - low), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}
}

SwipeEventRecognizer::SwipeEventRecognizer(
    const SwipeEventConfig config) noexcept
    : config_{config}
{
}

std::optional<HandEvent> SwipeEventRecognizer::process(
    const measurements::HandMeasurementFrame& measurements,
    const bool gestureGate,
    const float gestureConfidence) noexcept
{
    const auto& screenPalm = measurements.screenPalm;
    const auto& hand = measurements.hand;
    const bool inputValid = gestureGate
        && screenPalm.valid
        && hand.present
        && hand.quality == measurements::HandMeasurementQuality::Valid
        && hand.trackingQuality >= 0.70F
        && std::isfinite(screenPalm.centerX)
        && std::isfinite(screenPalm.centerY)
        && std::isfinite(screenPalm.scale);
    if (!inputValid)
    {
        reset();
        return std::nullopt;
    }

    if (phase_ == Phase::Cooldown)
    {
        if (screenPalm.timestampUs >= cooldownSinceUs_
            && screenPalm.timestampUs - cooldownSinceUs_ >= config_.cooldownUs)
        {
            captureAnchor(screenPalm);
            phase_ = Phase::Idle;
        }
        return std::nullopt;
    }

    if (beganTimestampUs_ == 0)
    {
        captureAnchor(screenPalm);
        return std::nullopt;
    }

    const std::uint64_t elapsedUs = screenPalm.timestampUs >= beganTimestampUs_
        ? screenPalm.timestampUs - beganTimestampUs_
        : 0;
    const float displacementX = screenPalm.centerX - anchorX_;
    const float displacementY = screenPalm.centerY - anchorY_;
    const float displacement = std::hypot(displacementX, displacementY);

    if (phase_ == Phase::Idle)
    {
        if (elapsedUs > config_.anchorTimeoutUs)
        {
            captureAnchor(screenPalm);
            return std::nullopt;
        }
        if (displacement < config_.motionStartDistance)
        {
            return std::nullopt;
        }
        phase_ = Phase::Tracking;
        pathLength_ = displacement;
        minimumGateConfidence_ = std::clamp(
            gestureConfidence, 0.0F, 1.0F);
    }
    else
    {
        pathLength_ += distance(
            previousX_, previousY_, screenPalm.centerX, screenPalm.centerY);
        minimumGateConfidence_ = std::min(
            minimumGateConfidence_,
            std::clamp(gestureConfidence, 0.0F, 1.0F));
    }
    previousX_ = screenPalm.centerX;
    previousY_ = screenPalm.centerY;

    if (elapsedUs > config_.maximumDurationUs)
    {
        captureAnchor(screenPalm);
        phase_ = Phase::Idle;
        return std::nullopt;
    }

    const float scaleRatio = screenPalm.scale
        / std::max(anchorScale_, 1.0e-5F);
    const float horizontalDistance = std::abs(displacementX);
    const float straightness = horizontalDistance
        / std::max(pathLength_, 1.0e-5F);
    const bool geometryAccepted =
        horizontalDistance >= config_.minimumDisplacement
        && std::abs(displacementY) <= config_.maximumVerticalDisplacement
        && straightness >= config_.minimumStraightness
        && scaleRatio >= config_.minimumScaleRatio
        && scaleRatio <= config_.maximumScaleRatio;
    if (!geometryAccepted || elapsedUs < config_.minimumDurationUs)
    {
        return std::nullopt;
    }

    HandEvent event{};
    event.id = displacementX < 0.0F
        ? kSwipeLeftEventId
        : kSwipeRightEventId;
    const float distanceScore = smoothStep(
        config_.minimumDisplacement,
        config_.minimumDisplacement * 1.65F,
        horizontalDistance);
    const float straightnessScore = smoothStep(
        config_.minimumStraightness, 0.96F, straightness);
    event.confidence = std::clamp(
        minimumGateConfidence_
            * (0.55F + 0.25F * distanceScore + 0.20F * straightnessScore),
        0.0F,
        1.0F);
    event.inputQuality = hand.trackingQuality;
    event.beganFrameId = beganFrameId_;
    event.endedFrameId = screenPalm.frameId;
    event.beganTimestampUs = beganTimestampUs_;
    event.endedTimestampUs = screenPalm.timestampUs;
    event.displacementX = displacementX;
    event.displacementY = displacementY;
    event.durationUs = elapsedUs;

    phase_ = Phase::Cooldown;
    cooldownSinceUs_ = screenPalm.timestampUs;
    return event;
}

void SwipeEventRecognizer::reset() noexcept
{
    phase_ = Phase::Idle;
    anchorX_ = 0.0F;
    anchorY_ = 0.0F;
    anchorScale_ = 0.0F;
    previousX_ = 0.0F;
    previousY_ = 0.0F;
    pathLength_ = 0.0F;
    minimumGateConfidence_ = 1.0F;
    beganFrameId_ = 0;
    beganTimestampUs_ = 0;
    cooldownSinceUs_ = 0;
}

void SwipeEventRecognizer::captureAnchor(
    const measurements::ScreenPalmMeasurement& screenPalm) noexcept
{
    anchorX_ = screenPalm.centerX;
    anchorY_ = screenPalm.centerY;
    anchorScale_ = screenPalm.scale;
    previousX_ = screenPalm.centerX;
    previousY_ = screenPalm.centerY;
    pathLength_ = 0.0F;
    minimumGateConfidence_ = 1.0F;
    beganFrameId_ = screenPalm.frameId;
    beganTimestampUs_ = screenPalm.timestampUs;
}
}
