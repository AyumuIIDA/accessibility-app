#include "HandPerception/MediaPipeGraph/palm_detection_to_roi.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ryoiki::hand_perception
{
geometry::RotatedRegion PalmDetectionToRoiCalculator::calculate(
    const PalmDetection& detection) const noexcept
{
    constexpr float kScale = 2.6F;
    constexpr float kShiftY = -0.5F;
    const auto& wrist = detection.keypoints[0];
    const auto& middleMcp = detection.keypoints[2];
    const float deltaX = middleMcp.x - wrist.x;
    const float deltaY = middleMcp.y - wrist.y;
    float rotation = std::atan2(deltaX, -deltaY);
    rotation = std::remainder(rotation, 2.0F * std::numbers::pi_v<float>);

    const float width = (std::max)(0.0F, detection.box.right - detection.box.left);
    const float height = (std::max)(0.0F, detection.box.bottom - detection.box.top);
    const float centerX = (detection.box.left + detection.box.right) * 0.5F;
    const float centerY = (detection.box.top + detection.box.bottom) * 0.5F;
    const float sine = std::sin(rotation);
    const float cosine = std::cos(rotation);
    const float side = (std::max)(width, height) * kScale;

    return {
        {
            centerX - sine * height * kShiftY,
            centerY + cosine * height * kShiftY
        },
        side,
        side,
        rotation
    };
}
}
