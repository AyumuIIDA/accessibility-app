#include "HandInput/Recognition/domain_expansion_state_recognizer.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ryoiki::hand_input::recognition
{
namespace
{
using hand_perception::Landmark3f;
struct Point2
{
    float x;
    float y;
};

Point2 point(const Landmark3f value) noexcept { return {value.x, value.y}; }
Point2 subtract(const Point2 left, const Point2 right) noexcept
{
    return {left.x - right.x, left.y - right.y};
}
float dot(const Point2 left, const Point2 right) noexcept
{
    return left.x * right.x + left.y * right.y;
}
float cross(const Point2 left, const Point2 right) noexcept
{
    return left.x * right.y - left.y * right.x;
}
float length(const Point2 value) noexcept { return std::sqrt(dot(value, value)); }
float distance(const Point2 left, const Point2 right) noexcept
{
    return length(subtract(left, right));
}
float smoothStep(const float low, const float high, const float value) noexcept
{
    if (!std::isfinite(value) || high <= low) return 0.0F;
    const float t = std::clamp((value - low) / (high - low), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}
float inverseSmoothStep(const float low, const float high, const float value) noexcept
{
    return 1.0F - smoothStep(low, high, value);
}
float straightness(
    const std::array<Landmark3f, 21>& landmarks,
    const std::size_t mcp,
    const std::size_t pip,
    const std::size_t tip) noexcept
{
    const auto proximal = subtract(point(landmarks[pip]), point(landmarks[mcp]));
    const auto distal = subtract(point(landmarks[tip]), point(landmarks[pip]));
    const float denominator = length(proximal) * length(distal);
    if (denominator <= 1.0e-6F) return 0.0F;
    return smoothStep(0.55F, 0.93F, dot(proximal, distal) / denominator);
}
float curledScore(
    const std::array<Landmark3f, 21>& landmarks,
    const std::size_t tip,
    const std::size_t pip,
    const float palmScale) noexcept
{
    const float reach = distance(point(landmarks[0]), point(landmarks[tip])) / palmScale;
    const float foldedBehindPip = inverseSmoothStep(
        1.02F, 1.30F,
        distance(point(landmarks[0]), point(landmarks[tip]))
            / (distance(point(landmarks[0]), point(landmarks[pip])) + 1.0e-6F));
    return std::max(inverseSmoothStep(1.25F, 1.85F, reach), foldedBehindPip);
}
float pointToSegmentDistance(
    const Point2 value,
    const Point2 start,
    const Point2 end) noexcept
{
    const auto segment = subtract(end, start);
    const float squaredLength = dot(segment, segment);
    if (squaredLength <= 1.0e-8F) return distance(value, start);
    const float t = std::clamp(dot(subtract(value, start), segment) / squaredLength, 0.0F, 1.0F);
    return distance(value, {start.x + segment.x * t, start.y + segment.y * t});
}
int orientation(const Point2 a, const Point2 b, const Point2 c) noexcept
{
    const float value = cross(subtract(b, a), subtract(c, a));
    if (std::abs(value) < 1.0e-5F) return 0;
    return value > 0.0F ? 1 : -1;
}
bool segmentsCross(const Point2 a, const Point2 b, const Point2 c, const Point2 d) noexcept
{
    return orientation(a, b, c) * orientation(a, b, d) < 0
        && orientation(c, d, a) * orientation(c, d, b) < 0;
}
}

DomainExpansionStateResult recognizeDomainExpansionState(
    const hand_perception::HandLandmarkResult& hand) noexcept
{
    DomainExpansionStateResult result{};
    if (!hand.detected) return result;
    for (const auto& landmark : hand.landmarks)
    {
        if (!std::isfinite(landmark.x) || !std::isfinite(landmark.y)) return result;
    }

    const auto& landmarks = hand.landmarks;
    const float palmScale = distance(point(landmarks[0]), point(landmarks[9]));
    if (!std::isfinite(palmScale) || palmScale <= 1.0e-5F) return result;

    const float indexReach = distance(point(landmarks[0]), point(landmarks[8])) / palmScale;
    result.features.indexExtended = 0.55F * smoothStep(1.45F, 1.95F, indexReach)
        + 0.45F * straightness(landmarks, 5, 6, 8);

    const auto indexStart = point(landmarks[5]);
    const auto indexTip = point(landmarks[8]);
    const auto indexAxis = subtract(indexTip, indexStart);
    const float indexLength = length(indexAxis);
    if (indexLength <= 1.0e-5F) return result;
    const float middleMcpSide = cross(indexAxis, subtract(point(landmarks[9]), indexStart))
        / (indexLength * palmScale);
    const float middleTipSide = cross(indexAxis, subtract(point(landmarks[12]), indexStart))
        / (indexLength * palmScale);
    const bool wrapsToOppositeSide = middleMcpSide * middleTipSide < -0.001F;
    const float oppositeSideScore = wrapsToOppositeSide
        ? std::min(smoothStep(0.025F, 0.18F, std::abs(middleMcpSide)),
            smoothStep(0.015F, 0.16F, std::abs(middleTipSide)))
        : 0.0F;
    const float proximityScore = inverseSmoothStep(
        0.10F, 0.48F,
        pointToSegmentDistance(point(landmarks[12]), indexStart, indexTip) / palmScale);
    bool skeletonCrosses = false;
    constexpr std::array<std::size_t, 4> kIndex{5, 6, 7, 8};
    constexpr std::array<std::size_t, 4> kMiddle{9, 10, 11, 12};
    for (std::size_t indexSegment = 0; indexSegment < 3 && !skeletonCrosses; ++indexSegment)
    {
        for (std::size_t middleSegment = 0; middleSegment < 3; ++middleSegment)
        {
            if (segmentsCross(
                    point(landmarks[kIndex[indexSegment]]), point(landmarks[kIndex[indexSegment + 1]]),
                    point(landmarks[kMiddle[middleSegment]]), point(landmarks[kMiddle[middleSegment + 1]])))
            {
                skeletonCrosses = true;
                break;
            }
        }
    }
    result.features.middleWrap = std::clamp(
        0.50F * oppositeSideScore + 0.30F * proximityScore
            + 0.20F * (skeletonCrosses ? 1.0F : 0.0F),
        0.0F, 1.0F);
    result.features.middleCurled = curledScore(landmarks, 12, 10, palmScale);
    result.features.ringCurled = curledScore(landmarks, 16, 14, palmScale);
    result.features.pinkyCurled = curledScore(landmarks, 20, 18, palmScale);
    const float thumbDistance = std::min(
        distance(point(landmarks[4]), point(landmarks[9])),
        distance(point(landmarks[4]), point(landmarks[13]))) / palmScale;
    result.features.thumbTucked = inverseSmoothStep(0.45F, 1.10F, thumbDistance);

    result.confidence = std::clamp(
        0.22F * result.features.indexExtended
        + 0.38F * result.features.middleWrap
        + 0.12F * result.features.middleCurled
        + 0.10F * result.features.ringCurled
        + 0.10F * result.features.pinkyCurled
        + 0.08F * result.features.thumbTucked,
        0.0F, 1.0F);
    result.detected = result.confidence >= 0.68F
        && result.features.indexExtended >= 0.55F
        && result.features.middleWrap >= 0.52F
        && result.features.ringCurled >= 0.40F
        && result.features.pinkyCurled >= 0.40F;
    result.inputValid = true;
    return result;
}

DomainExpansionStateRecognizer::DomainExpansionStateRecognizer() noexcept
    : stabilizer_{TimedStateConfig{
        .enterDurationUs = 150'000,
        .exitDurationUs = 120'000,
        .missingGraceUs = 220'000}}
{
}

HandStateResult DomainExpansionStateRecognizer::process(
    const DomainExpansionStateResult& sample,
    const float inputQuality,
    const std::uint64_t frameId,
    const std::uint64_t timestampUs) noexcept
{
    return stabilizer_.update(
        kDomainExpansionStateId,
        sample.inputValid,
        sample.detected,
        sample.confidence,
        inputQuality,
        frameId,
        timestampUs);
}

void DomainExpansionStateRecognizer::reset() noexcept
{
    stabilizer_.reset();
}
}
