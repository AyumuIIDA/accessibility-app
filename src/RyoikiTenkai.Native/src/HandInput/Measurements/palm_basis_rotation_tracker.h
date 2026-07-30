#pragma once

#include "HandInput/Measurements/hand_measurement_extractor.h"
#include "HandInput/Measurements/weighted_palm_rotation_tracker.h"

#include <array>

namespace ryoiki::hand_input::measurements
{
// Rotation derived from the orthonormal palm basis used by the 3D plot:
// X = index MCP(5) - pinky MCP(17), Y = orthogonalized wrist(0) to
// middle MCP(9), Z = X cross Y.
class PalmBasisRotationTracker final
{
public:
    [[nodiscard]] bool captureReference(const HandMeasurements& measurements) noexcept;
    [[nodiscard]] PalmRotationEstimate estimate(
        const HandMeasurements& measurements) const noexcept;
    void reset() noexcept;

private:
    std::array<float, 9> referenceBasis_{};
    bool hasReference_{false};
};
}
