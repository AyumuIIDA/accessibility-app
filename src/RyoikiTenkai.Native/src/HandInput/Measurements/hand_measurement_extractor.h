#pragma once

#include "HandInput/Measurements/hand_topology_history.h"
#include "HandPerception/MediaPipeGraph/hand_landmark_graph.h"

#include <array>
#include <cstdint>

namespace ryoiki::hand_input::measurements
{
struct HandMeasurementVector3
{
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

enum class HandMeasurementQuality : std::uint32_t
{
    Missing = 0,
    InvalidLandmarks = 1,
    DegeneratePalm = 2,
    Valid = 3
};

struct HandMeasurements
{
    std::uint64_t frameId{0};
    std::uint64_t timestampUs{0};
    std::array<HandMeasurementVector3, 21> canonicalLandmarks{};
    HandMeasurementVector3 palmPosition{};
    HandMeasurementVector3 palmXAxis{};
    HandMeasurementVector3 palmYAxis{};
    HandMeasurementVector3 palmZAxis{};
    std::array<float, 5> extension{};
    std::array<float, 5> curl{};
    std::array<float, 4> spread{};
    float palmScale{0.0F};
    float thumbIndexDistance{0.0F};
    float linearSpeed{0.0F};
    float trackingQuality{0.0F};
    float handedness{0.0F};
    bool present{false};
    bool usesWorldLandmarks{false};
    HandMeasurementQuality quality{HandMeasurementQuality::Missing};
};

struct ScreenPalmMeasurement
{
    std::uint64_t frameId{0};
    std::uint64_t timestampUs{0};
    float centerX{0.0F};
    float centerY{0.0F};
    // Palm RMS radius in upright-frame-width units, corrected for first-order
    // palm-plane foreshortening.
    float scale{0.0F};
    bool valid{false};
};

struct HandMeasurementFrame
{
    HandMeasurements hand{};
    ScreenPalmMeasurement screenPalm{};
    HandTopologyMeasurement topology{};
};

class HandMeasurementExtractor final
{
public:
    [[nodiscard]] HandMeasurementFrame extract(
        const hand_perception::HandLandmarkResult& hand,
        std::uint64_t frameId,
        std::uint64_t timestampUs,
        std::uint32_t uprightWidth,
        std::uint32_t uprightHeight) noexcept;

    void reset() noexcept;

private:
    // World-landmark continuity backs the metric-scale linear speed. Image
    // topology continuity is separate because a degenerate world palm does not
    // invalidate the image-space trajectory.
    void resetWorldContinuity() noexcept;
    void resetTopologyContinuity() noexcept;

    HandMeasurementVector3 previousPalmPosition_{};
    std::uint64_t previousTimestampUs_{0};
    float previousPalmScale_{0.0F};
    bool hasPreviousFrame_{false};
    float previousWristX_{0.0F};
    float previousWristY_{0.0F};
    float previousImageHandScale_{0.0F};
    std::uint64_t previousTopologyTimestampUs_{0};
    bool hasPreviousTopology_{false};
};
}
