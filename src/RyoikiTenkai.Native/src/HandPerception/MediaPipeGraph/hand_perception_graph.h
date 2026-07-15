#pragma once

#include "Buffers/frame_buffer.h"
#include "HandPerception/MediaPipeGraph/hand_landmark_graph.h"
#include "HandPerception/MediaPipeGraph/hand_landmarks_to_roi.h"
#include "HandPerception/MediaPipeGraph/palm_detection_graph.h"
#include "HandPerception/MediaPipeGraph/palm_detection_to_roi.h"

#include <memory>
#include <string>

namespace ryoiki::hand_perception
{
struct HandPerceptionResult
{
    PalmDetectionResult palms;
    HandLandmarkResult hand;
    geometry::RotatedRegion handRegion;
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
    geometry::RotatedRegion trackedRegion_{};
    bool hasTrackedRegion_{false};
};
}
