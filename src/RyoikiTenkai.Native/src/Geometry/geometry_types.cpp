#include "Geometry/geometry_types.h"

namespace ryoiki::geometry
{
Point2f AffineTransform::transform(const Point2f point) const noexcept
{
    return {
        values[0] * point.x + values[1] * point.y + values[2],
        values[3] * point.x + values[4] * point.y + values[5]
    };
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
