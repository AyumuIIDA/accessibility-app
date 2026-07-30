#pragma once

#include "HandPerception/MediaPipeGraph/hand_landmark_graph.h"

#include <array>

namespace ryoiki::hand_input::measurements
{
struct PalmRotationEstimate
{
    // Column-major rotation matrix mapping the captured reference palm to the
    // current palm.
    std::array<float, 9> rotation{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    float fitError{0.0F};
    bool valid{false};
};

class WeightedPalmRotationTracker final
{
public:
    struct Point
    {
        float x{0.0F};
        float y{0.0F};
        float z{0.0F};
    };

    [[nodiscard]] bool captureReference(
        const hand_perception::HandLandmarkResult& hand) noexcept;

    [[nodiscard]] PalmRotationEstimate estimate(
        const hand_perception::HandLandmarkResult& hand) const noexcept;

    void reset() noexcept;

private:
    std::array<Point, 6> reference_{};
    bool hasReference_{false};
};
}
