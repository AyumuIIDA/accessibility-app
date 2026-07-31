#pragma once

#include "HandInput/Measurements/weighted_palm_rotation_tracker.h"

#include <array>
#include <cstdint>

namespace ryoiki::hand_input::measurements
{
// One Euro filter applied directly on SO(3). The cutoff adapts to observed
// angular speed while every orientation update remains a valid rotation.
class PalmRotationOneEuroFilter final
{
public:
    [[nodiscard]] PalmRotationEstimate update(
        const PalmRotationEstimate& observation,
        std::uint64_t timestampUs) noexcept;
    void reset() noexcept;

private:
    std::array<float, 9> orientation_{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    std::array<float, 9> previousObservation_{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    float filteredAngularSpeed_{0.0F};
    std::uint64_t previousTimestampUs_{0};
    bool initialized_{false};
};
}
