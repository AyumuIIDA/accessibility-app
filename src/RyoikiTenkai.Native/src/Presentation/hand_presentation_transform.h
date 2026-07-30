#pragma once

#include <array>
#include <cstdint>

namespace ryoiki::presentation
{
enum class HandPresentationMode : std::int32_t
{
    MirrorDirect = 0,
    Physical = 1
};

struct Vector3
{
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

// Presentation policy applied only at consumer boundaries. Measurements remain
// in the physical camera/world coordinate contract.
class HandPresentationTransform final
{
public:
    [[nodiscard]] static HandPresentationTransform fromMode(
        HandPresentationMode mode) noexcept;

    [[nodiscard]] HandPresentationMode mode() const noexcept;
    [[nodiscard]] Vector3 transformPoint(Vector3 value) const noexcept;
    [[nodiscard]] Vector3 transformDirection(Vector3 value) const noexcept;
    [[nodiscard]] std::array<float, 9> transformRotation(
        const std::array<float, 9>& columnMajorRotation) const noexcept;
    [[nodiscard]] float transformScreenDeltaX(float value) const noexcept;
    [[nodiscard]] float horizontalSign() const noexcept;

private:
    explicit HandPresentationTransform(HandPresentationMode mode) noexcept;
    HandPresentationMode mode_{HandPresentationMode::MirrorDirect};
};
}
