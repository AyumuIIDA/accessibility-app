#pragma once

#include <array>

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
