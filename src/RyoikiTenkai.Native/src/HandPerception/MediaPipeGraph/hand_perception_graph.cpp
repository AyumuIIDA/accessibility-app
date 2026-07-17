#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"

#include <chrono>
#include <utility>

namespace ryoiki::hand_perception
{
HandPerceptionGraph::HandPerceptionGraph(
    std::unique_ptr<IPalmDetectionRunner> palmRunner,
    std::unique_ptr<IHandLandmarkRunner> handRunner)
    : palmGraph_{std::move(palmRunner)}, handGraph_{std::move(handRunner)}
{
}

HandPerceptionGraph::HandPerceptionGraph(
    std::unique_ptr<IPalmDetectionRunner> palmRunner,
    std::unique_ptr<IHandLandmarkRunner> handRunner,
    std::unique_ptr<geometry::IGeometryProcessor> palmGeometryProcessor,
    std::unique_ptr<geometry::IGeometryProcessor> handGeometryProcessor)
    : palmGraph_{std::move(palmRunner), std::move(palmGeometryProcessor)},
      handGraph_{std::move(handRunner), std::move(handGeometryProcessor)}
{
}

bool HandPerceptionGraph::process(
    const buffers::FrameBuffer& frame,
    HandPerceptionResult& result,
    HandPerceptionGraphMetrics& metrics,
    std::string& error)
{
    result = {};
    metrics = {};
    if (hasTrackedRegion_)
    {
        result.usedTracking = true;
        result.handRegion = trackedRegion_;
        if (!handGraph_.process(
                frame,
                trackedRegion_,
                0.5F,
                result.hand,
                metrics.hand,
                error))
        {
            return false;
        }
        if (!result.hand.detected)
        {
            hasTrackedRegion_ = false;
            error.clear();
            return true;
        }

        const auto trackingStarted = std::chrono::steady_clock::now();
        trackedRegion_ = landmarksToRoi_.calculate(result.hand);
        metrics.trackingUpdateMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - trackingStarted).count();
        return true;
    }

    if (!palmGraph_.process(frame, result.palms, metrics.palm, error))
    {
        return false;
    }
    if (result.palms.size() == 0)
    {
        error.clear();
        return true;
    }

    result.handRegion = detectionToRoi_.calculate(result.palms[0]);
    if (!handGraph_.process(
        frame,
        result.handRegion,
        0.7F,
        result.hand,
        metrics.hand,
        error))
    {
        return false;
    }
    if (result.hand.detected)
    {
        const auto trackingStarted = std::chrono::steady_clock::now();
        trackedRegion_ = landmarksToRoi_.calculate(result.hand);
        hasTrackedRegion_ = true;
        metrics.trackingUpdateMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - trackingStarted).count();
    }
    return true;
}

std::string HandPerceptionGraph::providerSummary() const
{
    return "palm=" + std::string{palmGraph_.providerName()}
        + ", hand=" + std::string{handGraph_.providerName()};
}
}
