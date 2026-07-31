#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ryoiki::hand_input::measurements
{
// The rolling recognizer window ported from the custom-gesture prototype is
// 2600 ms. The window is time based so a summary never silently describes a
// truncated segment; the sample capacity is only a high-frame-rate safety
// bound (2600 ms at 60 FPS needs 156 samples).
inline constexpr std::uint64_t kHandTopologyWindowUs = 2600000;
inline constexpr std::size_t kHandTopologyHistoryCapacity = 192;
// A short observation gap must not destroy a multi-second topology segment.
// This mirrors the perception graph's missing-track grace window so a track
// that survives occlusion also keeps its measurement history.
inline constexpr std::uint32_t kHandTopologyGapToleranceFrames = 5;

struct HandTopologyMeasurement
{
    std::uint64_t frameId{0};
    std::uint64_t timestampUs{0};
    std::array<float, 5> fingerStraightness{};
    std::uint32_t fingerStateMask{0};
    float signedPalmArea{0.0F};
    float palmCompression{0.0F};
    float palmDepthRange{0.0F};
    float boundingBoxAspect{0.0F};
    float boundingBoxArea{0.0F};
    float handScale{0.0F};
    float centerX{0.0F};
    float centerY{0.0F};
    float palmAxisRadians{0.0F};
    // Wrist displacement since the previous observation of the same track,
    // divided by the average palm-axis length of the two observations. The
    // value is dimensionless (palm-axis lengths) and is zero on the first
    // sample of a segment, so a summary must skip index 0 when accumulating.
    float translationStepX{0.0F};
    float translationStepY{0.0F};
    // translationStep magnitude per second, in palm-axis lengths per second.
    float palmVelocity{0.0F};
    float confidence{0.0F};
    float handedness{0.0F};
    bool valid{false};
};

struct HandTopologySummary
{
    std::uint64_t firstFrameId{0};
    std::uint64_t lastFrameId{0};
    std::uint64_t durationUs{0};
    std::size_t sampleCount{0};
    std::uint32_t startFingerStateMask{0};
    std::uint32_t endFingerStateMask{0};
    std::uint32_t fingerStateTransitionCount{0};
    std::uint32_t signedPalmAreaSignChanges{0};
    // Accumulated path length of the wrist trajectory, in palm-axis lengths.
    // This is deliberately not the endpoint distance: a gesture that returns
    // to its origin still travels.
    float palmTravel{0.0F};
    float palmOrientationRangeRadians{0.0F};
    float handednessRange{0.0F};
    float handednessMean{0.0F};
    float fingerStraightnessRangeMax{0.0F};
    float signedPalmAreaRange{0.0F};
    float palmCompressionMin{0.0F};
    float palmCompressionMax{0.0F};
    float palmCompressionDrop{0.0F};
    float palmDepthRangeMax{0.0F};
    float palmTurnScore{0.0F};
    float handScaleRatioRange{0.0F};
    float handScaleRatioDelta{0.0F};
    float boundingBoxAreaRatioRange{0.0F};
    float boundingBoxAreaRatioDelta{0.0F};
    // Net displacement over the window, in palm-axis lengths.
    float translationDeltaX{0.0F};
    float translationDeltaY{0.0F};
    float translationDistance{0.0F};
    float topologyChangeScore{0.0F};
    bool valid{false};
};

// Bounded, allocation-free topology history for one tracked hand. Missing
// observations are tolerated for a short grace window instead of discarding
// the segment on the first dropped frame.
class HandTopologyHistory final
{
public:
    void push(const HandTopologyMeasurement& measurement) noexcept;
    // Records one observation gap for this track. The retained segment
    // survives up to kHandTopologyGapToleranceFrames consecutive gaps.
    void noteMissingObservation() noexcept;
    void reset() noexcept;

    [[nodiscard]] HandTopologySummary summarize() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint32_t missingObservationCount() const noexcept;

private:
    [[nodiscard]] const HandTopologyMeasurement& at(
        std::size_t chronologicalIndex) const noexcept;
    void popOldest() noexcept;
    void pruneToWindow() noexcept;

    std::array<HandTopologyMeasurement, kHandTopologyHistoryCapacity> samples_{};
    std::size_t start_{0};
    std::size_t size_{0};
    std::uint32_t missingObservations_{0};
};
}
