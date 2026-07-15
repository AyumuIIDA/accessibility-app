#include "HandPerception/MediaPipeGraph/hand_landmarks_to_roi.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace ryoiki::hand_perception
{
geometry::RotatedRegion HandLandmarksToRoiCalculator::calculate(
    const HandLandmarkResult& hand) const noexcept
{
    constexpr std::array<std::size_t, 12> kBoundsLandmarks{
        0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18};
    constexpr float kScale = 2.0F;
    constexpr float kShiftY = -0.1F;

    const auto& wrist = hand.landmarks[0];
    const auto& indexMcp = hand.landmarks[5];
    const auto& middleMcp = hand.landmarks[9];
    const auto& ringMcp = hand.landmarks[13];
    const float upperX = ((indexMcp.x + ringMcp.x) * 0.5F + middleMcp.x) * 0.5F;
    const float upperY = ((indexMcp.y + ringMcp.y) * 0.5F + middleMcp.y) * 0.5F;
    float rotation = std::atan2(upperX - wrist.x, -(upperY - wrist.y));
    rotation = std::remainder(rotation, 2.0F * std::numbers::pi_v<float>);

    float axisLeft = std::numeric_limits<float>::max();
    float axisTop = std::numeric_limits<float>::max();
    float axisRight = std::numeric_limits<float>::lowest();
    float axisBottom = std::numeric_limits<float>::lowest();
    for (const auto index : kBoundsLandmarks)
    {
        axisLeft = (std::min)(axisLeft, hand.landmarks[index].x);
        axisTop = (std::min)(axisTop, hand.landmarks[index].y);
        axisRight = (std::max)(axisRight, hand.landmarks[index].x);
        axisBottom = (std::max)(axisBottom, hand.landmarks[index].y);
    }
    const float axisCenterX = (axisLeft + axisRight) * 0.5F;
    const float axisCenterY = (axisTop + axisBottom) * 0.5F;
    const float cosine = std::cos(rotation);
    const float sine = std::sin(rotation);

    float projectedLeft = std::numeric_limits<float>::max();
    float projectedTop = std::numeric_limits<float>::max();
    float projectedRight = std::numeric_limits<float>::lowest();
    float projectedBottom = std::numeric_limits<float>::lowest();
    for (const auto index : kBoundsLandmarks)
    {
        const float x = hand.landmarks[index].x - axisCenterX;
        const float y = hand.landmarks[index].y - axisCenterY;
        const float projectedX = x * cosine + y * sine;
        const float projectedY = -x * sine + y * cosine;
        projectedLeft = (std::min)(projectedLeft, projectedX);
        projectedTop = (std::min)(projectedTop, projectedY);
        projectedRight = (std::max)(projectedRight, projectedX);
        projectedBottom = (std::max)(projectedBottom, projectedY);
    }

    const float projectedCenterX = (projectedLeft + projectedRight) * 0.5F;
    const float projectedCenterY = (projectedTop + projectedBottom) * 0.5F;
    float centerX = projectedCenterX * cosine - projectedCenterY * sine + axisCenterX;
    float centerY = projectedCenterX * sine + projectedCenterY * cosine + axisCenterY;
    const float width = projectedRight - projectedLeft;
    const float height = projectedBottom - projectedTop;
    centerX -= sine * height * kShiftY;
    centerY += cosine * height * kShiftY;
    const float side = (std::max)(width, height) * kScale;
    return {{centerX, centerY}, side, side, rotation};
}
}
