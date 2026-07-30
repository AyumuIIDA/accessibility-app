#include "Presentation/hand_presentation_transform.h"

#include <cstddef>

namespace ryoiki::presentation
{
HandPresentationTransform::HandPresentationTransform(
    const HandPresentationMode mode) noexcept
    : mode_{mode}
{
}

HandPresentationTransform HandPresentationTransform::fromMode(
    const HandPresentationMode mode) noexcept
{
    return HandPresentationTransform{mode};
}

HandPresentationMode HandPresentationTransform::mode() const noexcept
{
    return mode_;
}

Vector3 HandPresentationTransform::transformPoint(const Vector3 value) const noexcept
{
    return {horizontalSign() * value.x, value.y, value.z};
}

Vector3 HandPresentationTransform::transformDirection(
    const Vector3 value) const noexcept
{
    return transformPoint(value);
}

std::array<float, 9> HandPresentationTransform::transformRotation(
    const std::array<float, 9>& rotation) const noexcept
{
    const std::array<float, 3> signs{horizontalSign(), 1.0F, 1.0F};
    std::array<float, 9> transformed{};
    for (std::size_t column = 0; column < 3; ++column)
    {
        for (std::size_t row = 0; row < 3; ++row)
        {
            transformed[column * 3 + row] =
                rotation[column * 3 + row] * signs[row] * signs[column];
        }
    }
    return transformed;
}

float HandPresentationTransform::transformScreenDeltaX(
    const float value) const noexcept
{
    return horizontalSign() * value;
}

float HandPresentationTransform::horizontalSign() const noexcept
{
    return mode_ == HandPresentationMode::MirrorDirect ? -1.0F : 1.0F;
}
}
