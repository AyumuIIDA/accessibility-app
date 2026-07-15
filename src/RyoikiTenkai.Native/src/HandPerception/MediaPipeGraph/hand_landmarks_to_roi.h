#pragma once

#include "Geometry/geometry_types.h"
#include "HandPerception/MediaPipeGraph/hand_landmark_graph.h"

namespace ryoiki::hand_perception
{
class HandLandmarksToRoiCalculator final
{
public:
    [[nodiscard]] geometry::RotatedRegion calculate(const HandLandmarkResult& hand) const noexcept;
};
}
