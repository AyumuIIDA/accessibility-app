#include "Rendering/hand_3d_plot.h"

#include <algorithm>
#include <cmath>

namespace ryoiki::rendering
{
namespace
{
hand_perception::Landmark3f subtract(
    const hand_perception::Landmark3f left,
    const hand_perception::Landmark3f right) noexcept
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

float dot(
    const hand_perception::Landmark3f left,
    const hand_perception::Landmark3f right) noexcept
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

hand_perception::Landmark3f normalize(
    const hand_perception::Landmark3f value) noexcept
{
    const float length = std::sqrt(dot(value, value));
    if (!std::isfinite(length) || length <= 1.0e-6F)
    {
        return {};
    }
    return {value.x / length, value.y / length, value.z / length};
}

hand_perception::Landmark3f cross(
    const hand_perception::Landmark3f left,
    const hand_perception::Landmark3f right) noexcept
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x};
}
}

hand_perception::Landmark3f calculatePalmNormal(
    const hand_perception::HandLandmarkResult& hand) noexcept
{
    if (!hand.detected)
    {
        return {};
    }
    const auto palmAcross = normalize(subtract(
        hand.worldLandmarks[5], hand.worldLandmarks[17]));
    const auto wristToMiddle = normalize(subtract(
        hand.worldLandmarks[9], hand.worldLandmarks[0]));
    return normalize(cross(palmAcross, wristToMiddle));
}

ProjectedHandPoint projectHand3dPoint(
    const hand_perception::Landmark3f& point,
    const PlotViewport& viewport,
    const Hand3dView& view) noexcept
{
    if (viewport.width() <= 0.0F || viewport.height() <= 0.0F
        || view.halfExtent <= 0.0F
        || !std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
    {
        return {};
    }

    // Model world landmarks retain the image convention: +X points toward
    // storage right and +Y points down. Convert them to a Y-up plot space and
    // apply the same front-camera mirror used by the camera viewport before
    // applying the user-facing plot rotation.
    const auto presentationTransform =
        presentation::HandPresentationTransform::fromMode(view.presentationMode);
    const auto presented = presentationTransform.transformPoint(
        {point.x, point.y, point.z});
    const float plotX = presented.x;
    const float plotY = -presented.y;
    const float plotZ = presented.z;
    const float yawCosine = std::cos(view.yawRadians);
    const float yawSine = std::sin(view.yawRadians);
    const float pitchCosine = std::cos(view.pitchRadians);
    const float pitchSine = std::sin(view.pitchRadians);
    const float yawX = yawCosine * plotX + yawSine * plotZ;
    const float yawZ = -yawSine * plotX + yawCosine * plotZ;
    const float viewY = pitchCosine * plotY - pitchSine * yawZ;
    const float viewZ = pitchSine * plotY + pitchCosine * yawZ;
    const float scale = (std::min)(viewport.width(), viewport.height())
        / (2.0F * view.halfExtent);
    const float centerX = (viewport.left + viewport.right) * 0.5F;
    const float centerY = (viewport.top + viewport.bottom) * 0.5F;
    return {{centerX + yawX * scale, centerY - viewY * scale}, viewZ, true};
}

Hand3dProjection projectHand3d(
    const hand_perception::HandLandmarkResult& hand,
    const PlotViewport& viewport,
    const Hand3dView& view) noexcept
{
    Hand3dProjection result{};
    if (!hand.detected)
    {
        return result;
    }

    result.origin = hand.worldLandmarks[0];
    for (std::size_t index = 0; index < hand.worldLandmarks.size(); ++index)
    {
        const auto& landmark = hand.worldLandmarks[index];
        result.points[index] = projectHand3dPoint(
            {landmark.x - result.origin.x,
                landmark.y - result.origin.y,
                landmark.z - result.origin.z},
            viewport,
            view);
        if (!result.points[index].visible)
        {
            return {};
        }
    }

    constexpr std::array<std::size_t, 5> kPalmIndices{0, 5, 9, 13, 17};
    hand_perception::Landmark3f palmCenter{};
    for (const auto index : kPalmIndices)
    {
        const auto relative = subtract(hand.worldLandmarks[index], result.origin);
        palmCenter.x += relative.x;
        palmCenter.y += relative.y;
        palmCenter.z += relative.z;
    }
    const float inversePalmPointCount = 1.0F / static_cast<float>(kPalmIndices.size());
    palmCenter.x *= inversePalmPointCount;
    palmCenter.y *= inversePalmPointCount;
    palmCenter.z *= inversePalmPointCount;

    const auto palmNormal = calculatePalmNormal(hand);
    constexpr float kPalmDirectionLength = 0.07F;
    const hand_perception::Landmark3f palmDirectionTip{
        palmCenter.x + palmNormal.x * kPalmDirectionLength,
        palmCenter.y + palmNormal.y * kPalmDirectionLength,
        palmCenter.z + palmNormal.z * kPalmDirectionLength};
    result.palmCenter = projectHand3dPoint(palmCenter, viewport, view);
    result.palmDirectionTip = projectHand3dPoint(palmDirectionTip, viewport, view);
    result.hasPalmDirection = result.palmCenter.visible
        && result.palmDirectionTip.visible
        && dot(palmNormal, palmNormal) > 0.5F;
    result.valid = true;
    return result;
}
}
