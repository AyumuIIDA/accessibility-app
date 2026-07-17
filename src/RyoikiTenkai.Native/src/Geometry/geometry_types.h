#pragma once

#include "Runtime/frame_orientation.h"

#include <array>
#include <cstdint>

namespace ryoiki::geometry
{
struct Point2f
{
    float x{0.0F};
    float y{0.0F};
};

struct RotatedRegion
{
    Point2f center;
    float width{0.0F};
    float height{0.0F};
    float rotationRadiansClockwise{0.0F};
};

struct AffineTransform
{
    std::array<float, 6> values{};

    [[nodiscard]] Point2f transform(Point2f point) const noexcept;
};

[[nodiscard]] AffineTransform identityTransform() noexcept;
[[nodiscard]] AffineTransform compose(
    const AffineTransform& first,
    const AffineTransform& second) noexcept;
[[nodiscard]] bool invert(
    const AffineTransform& value,
    AffineTransform& inverse) noexcept;

// Orientation matrices use continuous image-edge coordinates. OpenCV sampling uses
// integer pixel-center coordinates; this adapter performs the explicit +/-0.5 shift.
[[nodiscard]] AffineTransform toPixelCenterTransform(
    const AffineTransform& edgeTransform) noexcept;

[[nodiscard]] AffineTransform createStorageToUprightTransform(
    std::uint32_t storageWidth,
    std::uint32_t storageHeight,
    runtime::FrameRotation rotation) noexcept;

struct LetterboxTransform
{
    float scaleX{1.0F};
    float scaleY{1.0F};
    float padLeft{0.0F};
    float padTop{0.0F};

    [[nodiscard]] Point2f sourceToTensor(Point2f point) const noexcept;
    [[nodiscard]] Point2f tensorToSource(Point2f point) const noexcept;
};

struct PalmPreprocessResult
{
    LetterboxTransform transform;
};

struct HandPreprocessResult
{
    AffineTransform sourceToTensor;
    AffineTransform tensorToSource;
};
}
