#pragma once

#include "HandInput/Measurements/weighted_palm_rotation_tracker.h"

#include <array>
#include <cstdint>

namespace ryoiki::hand_input::measurements
{
// Error-state Kalman filter on SO(3). The nominal state is orientation plus
// angular velocity; the six-dimensional error state is [delta-angle, delta-omega].
// Absolute and incremental rotations remain separate observations so their source
// quality can be measured and changed independently.
class PalmRotationEskf final
{
public:
    [[nodiscard]] PalmRotationEstimate update(
        const PalmRotationEstimate& absoluteObservation,
        const PalmRotationEstimate& incrementalObservation,
        std::uint64_t timestampUs) noexcept;

    void reset() noexcept;

private:
    std::array<float, 9> orientation_{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    std::array<float, 3> angularVelocity_{};
    std::array<float, 36> covariance_{};
    std::uint64_t previousTimestampUs_{0};
    bool initialized_{false};
};
}
