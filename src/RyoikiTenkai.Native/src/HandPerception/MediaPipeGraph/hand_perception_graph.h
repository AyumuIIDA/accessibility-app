#pragma once

#include "Buffers/frame_buffer.h"
#include "HandPerception/MediaPipeGraph/hand_landmark_graph.h"
#include "HandPerception/MediaPipeGraph/hand_landmarks_to_roi.h"
#include "HandPerception/MediaPipeGraph/palm_detection_graph.h"
#include "HandPerception/MediaPipeGraph/palm_detection_to_roi.h"

#include <array>
#include <memory>
#include <string>

namespace ryoiki::hand_perception
{
inline constexpr std::size_t kMaxPerceivedHands = 2;

struct HandPerceptionResult
{
    PalmDetectionResult palms;
    std::array<HandLandmarkResult, kMaxPerceivedHands> hands{};
    std::array<geometry::RotatedRegion, kMaxPerceivedHands> handRegions{};
    HandLandmarkResult hand;
    geometry::RotatedRegion handRegion;
    std::size_t handCount{0};
    bool usedTracking{false};
};

struct HandPerceptionGraphMetrics
{
    PalmDetectionGraphMetrics palm;
    HandLandmarkGraphMetrics hand;
    double trackingUpdateMs{0.0};
};

class HandPerceptionGraph final
{
public:
    HandPerceptionGraph(
        std::unique_ptr<IPalmDetectionRunner> palmRunner,
        std::unique_ptr<IHandLandmarkRunner> handRunner);
    HandPerceptionGraph(
        std::unique_ptr<IPalmDetectionRunner> palmRunner,
        std::unique_ptr<IHandLandmarkRunner> handRunner,
        std::unique_ptr<geometry::IGeometryProcessor> palmGeometryProcessor,
        std::unique_ptr<geometry::IGeometryProcessor> handGeometryProcessor);

    bool process(
        const buffers::FrameBuffer& frame,
        HandPerceptionResult& result,
        HandPerceptionGraphMetrics& metrics,
        std::string& error);

    [[nodiscard]] std::string providerSummary() const;

private:
    PalmDetectionGraph palmGraph_;
    HandLandmarkGraph handGraph_;
    PalmDetectionToRoiCalculator detectionToRoi_;
    HandLandmarksToRoiCalculator landmarksToRoi_;
    std::array<geometry::RotatedRegion, kMaxPerceivedHands> trackedRegions_{};
    std::size_t trackedRegionCount_{0};
    std::size_t trackedFramesSincePalmDiscovery_{0};
};
}
