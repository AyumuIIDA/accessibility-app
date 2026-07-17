#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ryoiki::runtime
{
enum class FrameRotation : std::uint16_t
{
    None = 0,
    Clockwise90 = 90,
    Clockwise180 = 180,
    Clockwise270 = 270
};

struct OrientedFrameSize
{
    std::uint32_t width{0};
    std::uint32_t height{0};
};

[[nodiscard]] constexpr OrientedFrameSize orientedSize(
    const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight,
    const FrameRotation rotation) noexcept
{
    return rotation == FrameRotation::Clockwise90 || rotation == FrameRotation::Clockwise270
        ? OrientedFrameSize{sourceHeight, sourceWidth}
        : OrientedFrameSize{sourceWidth, sourceHeight};
}

// Copies storage rows into a tightly packed top-down buffer without applying
// orientation. Rotation remains metadata and is fused into downstream sampling.
[[nodiscard]] bool copyStorageBgra32(
    const std::uint8_t* sourceScanline0,
    std::ptrdiff_t sourceStride,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::vector<std::uint8_t>& destination);

}
