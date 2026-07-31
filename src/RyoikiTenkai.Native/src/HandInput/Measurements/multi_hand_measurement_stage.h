#pragma once

#include "HandInput/Measurements/gesture_candidate_window_history.h"
#include "HandInput/Measurements/hand_measurement_extractor.h"
#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ryoiki::hand_input::measurements
{
// A handedness gap this wide is treated as a reliable left/right ordering
// signal. Below it the pair is ordered by screen position instead. Handedness
// orders a pair; it never identifies a track. Track identity stays with the
// perception graph's stable HandTrackId.
inline constexpr float kReliableHandednessGap = 0.20F;

enum class TwoHandOrdering : std::uint32_t
{
    None = 0,
    Handedness = 1,
    ScreenPosition = 2
};

struct TrackedHandMeasurement
{
    hand_perception::HandTrackId trackId{0};
    HandMeasurements hand{};
    ScreenPalmMeasurement screenPalm{};
    HandTopologyMeasurement topology{};
    HandTopologySummary topologySummary{};
};

// Relative geometry of the two observed hands for one frame. Values are
// expressed in palm-axis lengths (the average of both hands' wrist-to-middle
// -MCP distance), so they are independent of resolution and camera distance.
struct TwoHandRelationMeasurement
{
    std::uint64_t frameId{0};
    std::uint64_t timestampUs{0};
    // firstTrackId is the low-ordered hand and secondTrackId the high-ordered
    // hand under `ordering`. The deltas point from first to second.
    hand_perception::HandTrackId firstTrackId{0};
    hand_perception::HandTrackId secondTrackId{0};
    TwoHandOrdering ordering{TwoHandOrdering::None};
    float deltaX{0.0F};
    float deltaY{0.0F};
    float distance{0.0F};
    float angleRadians{0.0F};
    // Apparent palm-axis length of the second hand relative to the first.
    float scaleRatio{0.0F};
    float quality{0.0F};
    bool valid{false};
};

struct MultiHandMeasurementFrame
{
    std::uint64_t frameId{0};
    std::uint64_t timestampUs{0};
    std::array<TrackedHandMeasurement, hand_perception::kMaxPerceivedHands> hands{};
    std::size_t handCount{0};
    TwoHandRelationMeasurement relation{};
};

class MultiHandMeasurementStage final
{
public:
    [[nodiscard]] MultiHandMeasurementFrame extract(
        const hand_perception::HandPerceptionResult& observations,
        std::uint64_t frameId,
        std::uint64_t timestampUs,
        std::uint32_t uprightWidth,
        std::uint32_t uprightHeight) noexcept;

    void reset() noexcept;

    // Returns the rolling gesture candidate-window history for an active
    // track, or nullptr if trackId is not currently tracked. The pointer is
    // only valid until the next extract() call (a track may be evicted or
    // reassigned). Candidate-window construction is deliberately not run
    // automatically every frame; a future recognition-stage consumer calls
    // GestureCandidateWindowHistory::buildCandidateWindows on the returned
    // history only when it actually needs one, keeping this per-frame
    // Measurement path free of DTW-adjacent cost until a real consumer
    // exists.
    [[nodiscard]] const GestureCandidateWindowHistory* gestureCandidateHistory(
        hand_perception::HandTrackId trackId) const noexcept;

private:
    struct ExtractorSlot
    {
        hand_perception::HandTrackId trackId{0};
        std::uint64_t lastSeenFrameId{0};
        std::uint32_t missingFrameCount{0};
        HandMeasurementExtractor extractor{};
        HandTopologyHistory topologyHistory{};
        GestureCandidateWindowHistory gestureHistory{};
        bool active{false};
    };

    ExtractorSlot& slotFor(
        hand_perception::HandTrackId trackId,
        std::uint64_t frameId) noexcept;
    void ageUnobservedSlots(std::uint64_t frameId) noexcept;

    std::array<ExtractorSlot, hand_perception::kMaxPerceivedHands> slots_{};
};
}
