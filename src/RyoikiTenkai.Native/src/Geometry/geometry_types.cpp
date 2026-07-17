#include "Geometry/geometry_types.h"

#include <cmath>

namespace ryoiki::geometry
{
Point2f AffineTransform::transform(const Point2f point) const noexcept
{
    return {
        values[0] * point.x + values[1] * point.y + values[2],
        values[3] * point.x + values[4] * point.y + values[5]
    };
}

AffineTransform identityTransform() noexcept
{
    return {{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}};
}

AffineTransform compose(
    const AffineTransform& first,
    const AffineTransform& second) noexcept
{
    const auto& f = first.values;
    const auto& s = second.values;
    return {{
        s[0] * f[0] + s[1] * f[3],
        s[0] * f[1] + s[1] * f[4],
        s[0] * f[2] + s[1] * f[5] + s[2],
        s[3] * f[0] + s[4] * f[3],
        s[3] * f[1] + s[4] * f[4],
        s[3] * f[2] + s[4] * f[5] + s[5]}};
}

bool invert(const AffineTransform& value, AffineTransform& inverse) noexcept
{
    const auto& v = value.values;
    const float determinant = v[0] * v[4] - v[1] * v[3];
    if (std::abs(determinant) < 1.0e-8F)
    {
        return false;
    }

    const float inverseDeterminant = 1.0F / determinant;
    inverse = {{
        v[4] * inverseDeterminant,
        -v[1] * inverseDeterminant,
        (v[1] * v[5] - v[4] * v[2]) * inverseDeterminant,
        -v[3] * inverseDeterminant,
        v[0] * inverseDeterminant,
        (v[3] * v[2] - v[0] * v[5]) * inverseDeterminant}};
    return true;
}

AffineTransform toPixelCenterTransform(const AffineTransform& edgeTransform) noexcept
{
    auto result = edgeTransform;
    result.values[2] += 0.5F * edgeTransform.values[0]
        + 0.5F * edgeTransform.values[1] - 0.5F;
    result.values[5] += 0.5F * edgeTransform.values[3]
        + 0.5F * edgeTransform.values[4] - 0.5F;
    return result;
}

AffineTransform createStorageToUprightTransform(
    const std::uint32_t storageWidth,
    const std::uint32_t storageHeight,
    const runtime::FrameRotation rotation) noexcept
{
    const float width = static_cast<float>(storageWidth);
    const float height = static_cast<float>(storageHeight);
    switch (rotation)
    {
    case runtime::FrameRotation::Clockwise90:
        return {{0.0F, -1.0F, height, 1.0F, 0.0F, 0.0F}};
    case runtime::FrameRotation::Clockwise180:
        return {{-1.0F, 0.0F, width, 0.0F, -1.0F, height}};
    case runtime::FrameRotation::Clockwise270:
        return {{0.0F, 1.0F, 0.0F, -1.0F, 0.0F, width}};
    default:
        return identityTransform();
    }
}

Point2f LetterboxTransform::sourceToTensor(const Point2f point) const noexcept
{
    return {
        (point.x + 0.5F) * scaleX - 0.5F + padLeft,
        (point.y + 0.5F) * scaleY - 0.5F + padTop
    };
}

Point2f LetterboxTransform::tensorToSource(const Point2f point) const noexcept
{
    return {
        (point.x - padLeft + 0.5F) / scaleX - 0.5F,
        (point.y - padTop + 0.5F) / scaleY - 0.5F
    };
}
}
