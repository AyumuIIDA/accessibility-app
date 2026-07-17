#pragma once

#include "Geometry/geometry_types.h"
#include "Runtime/frame_orientation.h"

#include <cstdint>

namespace ryoiki::rendering
{
struct FrameTransforms
{
    geometry::AffineTransform storageToUpright;
    geometry::AffineTransform uprightToViewport;
    geometry::AffineTransform storageToViewport;
    geometry::AffineTransform viewportToUpright;
    geometry::AffineTransform uprightToStorage;
    float contentLeft{0.0F};
    float contentTop{0.0F};
    float contentWidth{0.0F};
    float contentHeight{0.0F};
};

[[nodiscard]] bool createFrameTransforms(
    std::uint32_t storageWidth,
    std::uint32_t storageHeight,
    runtime::FrameRotation rotation,
    std::uint32_t viewportWidth,
    std::uint32_t viewportHeight,
    bool mirrorHorizontally,
    FrameTransforms& result) noexcept;
}
