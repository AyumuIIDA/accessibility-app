#pragma once

#include "Geometry/geometry_types.h"
#include "HandPerception/MediaPipeGraph/palm_detection_postprocessor.h"

namespace ryoiki::hand_perception
{
class PalmDetectionToRoiCalculator final
{
public:
    [[nodiscard]] geometry::RotatedRegion calculate(const PalmDetection& detection) const noexcept;
};
}
