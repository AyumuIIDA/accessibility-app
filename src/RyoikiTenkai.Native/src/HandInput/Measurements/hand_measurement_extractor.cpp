#include "HandInput/Measurements/hand_measurement_extractor.h"

#include <algorithm>
#include <cmath>

namespace ryoiki::hand_input::measurements
{
namespace
{
using Vector3 = HandMeasurementVector3;

Vector3 toVector(const hand_perception::Landmark3f& value) noexcept
{
    return {value.x, value.y, value.z};
}
Vector3 subtract(const Vector3 left, const Vector3 right) noexcept
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}
Vector3 multiply(const Vector3 value, const float scale) noexcept
{
    return {value.x * scale, value.y * scale, value.z * scale};
}
float dot(const Vector3 left, const Vector3 right) noexcept
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}
Vector3 cross(const Vector3 left, const Vector3 right) noexcept
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x};
}
float length(const Vector3 value) noexcept { return std::sqrt(dot(value, value)); }
float distance(const Vector3 left, const Vector3 right) noexcept
{
    return length(subtract(left, right));
}
bool isFinite(const Vector3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool normalize(const Vector3 value, Vector3& result) noexcept
{
    const float magnitude = length(value);
    if (!std::isfinite(magnitude) || magnitude <= 1.0e-6F) return false;
    result = multiply(value, 1.0F / magnitude);
    return true;
}
float cosine(const Vector3 left, const Vector3 right) noexcept
{
    const float denominator = length(left) * length(right);
    if (!std::isfinite(denominator) || denominator <= 1.0e-6F) return -1.0F;
    return std::clamp(dot(left, right) / denominator, -1.0F, 1.0F);
}
float straightness(
    const std::array<Vector3, 21>& points,
    const std::size_t mcp,
    const std::size_t pip,
    const std::size_t dip,
    const std::size_t tip) noexcept
{
    const float first = cosine(subtract(points[pip], points[mcp]),
        subtract(points[dip], points[pip]));
    const float second = cosine(subtract(points[dip], points[pip]),
        subtract(points[tip], points[dip]));
    return std::clamp((0.5F * (first + second) + 1.0F) * 0.5F, 0.0F, 1.0F);
}
}

HandMeasurementFrame HandMeasurementExtractor::extract(
    const hand_perception::HandLandmarkResult& hand,
    const std::uint64_t frameId,
    const std::uint64_t timestampUs,
    const std::uint32_t uprightWidth,
    const std::uint32_t uprightHeight) noexcept
{
    HandMeasurementFrame frame{};
    auto& result = frame.hand;
    result.frameId = frameId;
    result.timestampUs = timestampUs;
    result.handedness = hand.handedness;
    frame.screenPalm.frameId = frameId;
    frame.screenPalm.timestampUs = timestampUs;
    if (!hand.detected)
    {
        reset();
        return frame;
    }

    std::array<Vector3, 21> imagePoints{};
    std::array<Vector3, 21> worldPoints{};
    bool imageValid = true;
    bool worldValid = true;
    for (std::size_t index = 0; index < imagePoints.size(); ++index)
    {
        imagePoints[index] = toVector(hand.landmarks[index]);
        worldPoints[index] = toVector(hand.worldLandmarks[index]);
        imageValid = imageValid && isFinite(imagePoints[index]);
        worldValid = worldValid && isFinite(worldPoints[index]);
    }
    if (!imageValid)
    {
        result.quality = HandMeasurementQuality::InvalidLandmarks;
        reset();
        return frame;
    }

    if (uprightWidth > 0 && uprightHeight > 0)
    {
        constexpr std::array<std::size_t, 5> kPalmIndices{0, 5, 9, 13, 17};
        Vector3 screenCenter{};
        for (const auto index : kPalmIndices)
        {
            screenCenter.x += imagePoints[index].x;
            screenCenter.y += imagePoints[index].y;
        }
        screenCenter = multiply(
            screenCenter, 1.0F / static_cast<float>(kPalmIndices.size()));
        float squaredRadius = 0.0F;
        for (const auto index : kPalmIndices)
        {
            const float dx = (imagePoints[index].x - screenCenter.x)
                / static_cast<float>(uprightWidth);
            // Use width for both axes so scale is isotropic. centerY remains
            // normalized by height because it is a screen position.
            const float dy = (imagePoints[index].y - screenCenter.y)
                / static_cast<float>(uprightWidth);
            squaredRadius += dx * dx + dy * dy;
        }
        frame.screenPalm.centerX =
            screenCenter.x / static_cast<float>(uprightWidth);
        frame.screenPalm.centerY =
            screenCenter.y / static_cast<float>(uprightHeight);
        frame.screenPalm.scale =
            std::sqrt(squaredRadius / static_cast<float>(kPalmIndices.size()));
    }

    if (!worldValid)
    {
        result.quality = HandMeasurementQuality::InvalidLandmarks;
        reset();
        return frame;
    }

    result.usesWorldLandmarks = true;
    const auto& points = worldPoints;
    result.palmScale = distance(points[5], points[17]);
    if (!std::isfinite(result.palmScale) || result.palmScale <= 1.0e-5F)
    {
        result.quality = HandMeasurementQuality::DegeneratePalm;
        reset();
        return frame;
    }

    // A palm position is the centroid of the wrist and four MCP anchors, not
    // landmark 0 (the wrist). This is also the center used by screen motion.
    constexpr std::array<std::size_t, 5> kPalmIndices{0, 5, 9, 13, 17};
    for (const auto index : kPalmIndices)
    {
        result.palmPosition.x += points[index].x;
        result.palmPosition.y += points[index].y;
        result.palmPosition.z += points[index].z;
    }
    result.palmPosition = multiply(
        result.palmPosition, 1.0F / static_cast<float>(kPalmIndices.size()));
    if (!normalize(subtract(points[5], points[17]), result.palmXAxis))
    {
        result.quality = HandMeasurementQuality::DegeneratePalm;
        reset();
        return frame;
    }
    const Vector3 wristToMiddle = subtract(points[9], points[0]);
    const Vector3 palmYUnnormalized = subtract(
        wristToMiddle, multiply(result.palmXAxis, dot(wristToMiddle, result.palmXAxis)));
    if (!normalize(palmYUnnormalized, result.palmYAxis)
        || !normalize(cross(result.palmXAxis, result.palmYAxis), result.palmZAxis))
    {
        result.quality = HandMeasurementQuality::DegeneratePalm;
        reset();
        return frame;
    }

    const float inverseScale = 1.0F / result.palmScale;
    for (std::size_t index = 0; index < points.size(); ++index)
    {
        const Vector3 relative = subtract(points[index], points[0]);
        result.canonicalLandmarks[index] = {
            dot(relative, result.palmXAxis) * inverseScale,
            dot(relative, result.palmYAxis) * inverseScale,
            dot(relative, result.palmZAxis) * inverseScale};
    }

    result.extension[0] = straightness(points, 1, 2, 3, 4);
    constexpr std::array<std::array<std::size_t, 4>, 4> fingers{{
        {{5, 6, 7, 8}}, {{9, 10, 11, 12}},
        {{13, 14, 15, 16}}, {{17, 18, 19, 20}}}};
    for (std::size_t index = 0; index < fingers.size(); ++index)
    {
        const auto& finger = fingers[index];
        result.extension[index + 1] = straightness(
            points, finger[0], finger[1], finger[2], finger[3]);
    }
    for (std::size_t index = 0; index < result.curl.size(); ++index)
    {
        result.curl[index] = 1.0F - result.extension[index];
    }
    constexpr std::array<std::size_t, 5> tips{{4, 8, 12, 16, 20}};
    for (std::size_t index = 0; index < result.spread.size(); ++index)
    {
        result.spread[index] = distance(points[tips[index]], points[tips[index + 1]])
            * inverseScale;
    }
    result.thumbIndexDistance = result.spread[0];

    if (hasPreviousFrame_ && timestampUs > previousTimestampUs_)
    {
        const float seconds = static_cast<float>(timestampUs - previousTimestampUs_) * 1.0e-6F;
        const float scale = 0.5F * (result.palmScale + previousPalmScale_);
        if (seconds > 1.0e-6F && scale > 1.0e-5F)
        {
            result.linearSpeed = distance(result.palmPosition, previousPalmPosition_)
                / scale / seconds;
        }
    }
    previousPalmPosition_ = result.palmPosition;
    previousTimestampUs_ = timestampUs;
    previousPalmScale_ = result.palmScale;
    hasPreviousFrame_ = true;

    result.trackingQuality = std::clamp(hand.confidence, 0.0F, 1.0F);
    result.present = true;
    result.quality = HandMeasurementQuality::Valid;
    if (std::isfinite(frame.screenPalm.scale) && frame.screenPalm.scale > 1.0e-5F)
    {
        const float xProjectionSquared =
            result.palmXAxis.x * result.palmXAxis.x
            + result.palmXAxis.y * result.palmXAxis.y;
        const float yProjectionSquared =
            result.palmYAxis.x * result.palmYAxis.x
            + result.palmYAxis.y * result.palmYAxis.y;
        const float projection = std::sqrt(std::max(
            0.5F * (xProjectionSquared + yProjectionSquared), 0.16F));
        frame.screenPalm.scale /= projection;
        frame.screenPalm.valid = std::isfinite(frame.screenPalm.scale);
    }
    return frame;
}

void HandMeasurementExtractor::reset() noexcept
{
    previousPalmPosition_ = {};
    previousTimestampUs_ = 0;
    previousPalmScale_ = 0.0F;
    hasPreviousFrame_ = false;
}
}
