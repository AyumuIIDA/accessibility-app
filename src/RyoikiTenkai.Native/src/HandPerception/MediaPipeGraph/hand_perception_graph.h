#pragma once

#include "Buffers/frame_buffer.h"
#include "HandPerception/MediaPipeGraph/hand_landmark_graph.h"
#include "HandPerception/MediaPipeGraph/hand_landmarks_to_roi.h"
#include "HandPerception/MediaPipeGraph/palm_detection_graph.h"
#include "HandPerception/MediaPipeGraph/palm_detection_to_roi.h"

#include <array>
#include <cstddef>
#include <memory>
#include <cstdint>
#include <string>

namespace ryoiki::hand_perception
{
inline constexpr std::size_t kMaxPerceivedHands = 2;
using HandTrackId = std::uint32_t;

struct HandPerceptionResult
{
    PalmDetectionResult palms;
    std::array<HandLandmarkResult, kMaxPerceivedHands> hands{};
    std::array<geometry::RotatedRegion, kMaxPerceivedHands> handRegions{};
    std::array<HandTrackId, kMaxPerceivedHands> trackIds{};
    std::array<std::uint32_t, kMaxPerceivedHands> handFlags{};
    std::array<float, kMaxPerceivedHands> filteredHandedness{};
    HandLandmarkResult hand;
    geometry::RotatedRegion handRegion;
    std::size_t handCount{0};
    HandTrackId crossingOwnerTrackId{0};
    bool usedTracking{false};
    bool crossingActive{false};
};

struct HandPerceptionGraphMetrics
{
    PalmDetectionGraphMetrics palm;
    HandLandmarkGraphMetrics hand;
    double trackingUpdateMs{0.0};
};

struct HandPerceptionGraphOptions
{
    std::uint64_t calibrationPalmIntervalFrames{0};
    std::uint64_t palmRediscoveryIntervalFrames{15};
    std::uint32_t missingTrackGraceFrames{5};
};

class HandPerceptionGraph final
{
public:
    HandPerceptionGraph(
        std::unique_ptr<IPalmDetectionRunner> palmRunner,
        std::unique_ptr<IHandLandmarkRunner> handRunner,
        HandPerceptionGraphOptions options = {});
    HandPerceptionGraph(
        std::unique_ptr<IPalmDetectionRunner> palmRunner,
        std::unique_ptr<IHandLandmarkRunner> handRunner,
        std::unique_ptr<geometry::IGeometryProcessor> palmGeometryProcessor,
        std::unique_ptr<geometry::IGeometryProcessor> handGeometryProcessor,
        HandPerceptionGraphOptions options = {});

    bool process(
        const buffers::FrameBuffer& frame,
        HandPerceptionResult& result,
        HandPerceptionGraphMetrics& metrics,
        std::string& error);

    [[nodiscard]] std::string providerSummary() const;

private:
    struct TrackedHandSlot
    {
        HandTrackId id{0};
        geometry::RotatedRegion region{};
        BoundingBox lastBox{};
        BoundingBox previousBox{};
        std::uint32_t missingFrameCount{0};
        bool hasPreviousBox{false};
        float filteredHandedness{0.0F};
        bool hasHandedness{false};
        bool active{false};
    };

    struct CrossingState
    {
        HandTrackId visibleOwnerId{0};
        std::uint32_t ageFrames{0};
        bool active{false};
    };

    PalmDetectionGraph palmGraph_;
    HandLandmarkGraph handGraph_;
    PalmDetectionToRoiCalculator detectionToRoi_;
    HandLandmarksToRoiCalculator landmarksToRoi_;
    std::array<TrackedHandSlot, kMaxPerceivedHands> trackedHands_{};
    std::size_t trackedHandCount_{0};
    HandPerceptionGraphOptions options_{};
    std::uint64_t processedFrameCount_{0};
    std::uint64_t framesSincePalmDiscovery_{0};
    HandTrackId nextTrackId_{1};
    CrossingState crossing_{};
};
}
