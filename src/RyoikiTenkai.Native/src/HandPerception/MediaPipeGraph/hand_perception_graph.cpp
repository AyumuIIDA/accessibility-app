#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace ryoiki::hand_perception
{
namespace
{
constexpr std::size_t kPalmRediscoveryIntervalFrames = 15;

float intersectionOverUnion(const BoundingBox& first, const BoundingBox& second) noexcept
{
    const float intersectionWidth = (std::max)(0.0F, (std::min)(first.right, second.right)
        - (std::max)(first.left, second.left));
    const float intersectionHeight = (std::max)(0.0F, (std::min)(first.bottom, second.bottom)
        - (std::max)(first.top, second.top));
    const float intersection = intersectionWidth * intersectionHeight;
    const float firstArea = (std::max)(0.0F, first.right - first.left)
        * (std::max)(0.0F, first.bottom - first.top);
    const float secondArea = (std::max)(0.0F, second.right - second.left)
        * (std::max)(0.0F, second.bottom - second.top);
    const float unionArea = firstArea + secondArea - intersection;
    return unionArea > 0.0F ? intersection / unionArea : 0.0F;
}

bool overlapsExistingHand(
    const BoundingBox& box,
    const HandPerceptionResult& result) noexcept
{
    for (std::size_t index = 0; index < result.handCount; ++index)
    {
        if (intersectionOverUnion(box, result.hands[index].box) >= 0.35F)
        {
            return true;
        }
    }
    return false;
}

void addDetectedHand(
    HandPerceptionResult& result,
    const geometry::RotatedRegion& region,
    const HandLandmarkResult& hand)
{
    if (!hand.detected || result.handCount >= kMaxPerceivedHands)
    {
        return;
    }

    result.hands[result.handCount] = hand;
    result.handRegions[result.handCount] = region;
    ++result.handCount;
    if (!result.hand.detected)
    {
        result.hand = hand;
        result.handRegion = region;
    }
}
}

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
    std::array<geometry::RotatedRegion, kMaxPerceivedHands> nextTrackedRegions{};
    std::size_t nextTrackedRegionCount = 0;

    if (trackedRegionCount_ > 0)
    {
        result.usedTracking = true;
        for (std::size_t index = 0;
            index < trackedRegionCount_ && result.handCount < kMaxPerceivedHands;
            ++index)
        {
            HandLandmarkResult trackedHand{};
            HandLandmarkGraphMetrics trackedMetrics{};
            if (!handGraph_.process(
                    frame,
                    trackedRegions_[index],
                    0.5F,
                    trackedHand,
                    trackedMetrics,
                    error))
            {
                return false;
            }

            metrics.hand.roiCropWarpMs += trackedMetrics.roiCropWarpMs;
            metrics.hand.inferenceMs += trackedMetrics.inferenceMs;
            metrics.hand.postprocessMs += trackedMetrics.postprocessMs;
            if (!trackedHand.detected)
            {
                error.clear();
                continue;
            }

            const auto trackingStarted = std::chrono::steady_clock::now();
            nextTrackedRegions[nextTrackedRegionCount++] = landmarksToRoi_.calculate(trackedHand);
            metrics.trackingUpdateMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - trackingStarted).count();
            addDetectedHand(result, trackedRegions_[index], trackedHand);
        }

        if (result.handCount == 0)
        {
            trackedRegions_ = {};
            trackedRegionCount_ = 0;
            trackedFramesSincePalmDiscovery_ = 0;
            error.clear();
            return true;
        }
    }

    const bool shouldRunPalmDetection = result.handCount < kMaxPerceivedHands
        && (!result.usedTracking
            || ++trackedFramesSincePalmDiscovery_ >= kPalmRediscoveryIntervalFrames);
    if (shouldRunPalmDetection)
    {
        trackedFramesSincePalmDiscovery_ = 0;
    }
    if (shouldRunPalmDetection && !palmGraph_.process(frame, result.palms, metrics.palm, error))
    {
        return false;
    }

    for (std::size_t palmIndex = 0;
        shouldRunPalmDetection
        && palmIndex < result.palms.size()
        && result.handCount < kMaxPerceivedHands;
        ++palmIndex)
    {
        if (overlapsExistingHand(result.palms[palmIndex].box, result))
        {
            continue;
        }

        const auto handRegion = detectionToRoi_.calculate(result.palms[palmIndex]);
        HandLandmarkResult palmHand{};
        HandLandmarkGraphMetrics palmMetrics{};
        if (!handGraph_.process(
                frame,
                handRegion,
                0.7F,
                palmHand,
                palmMetrics,
                error))
        {
            return false;
        }

        metrics.hand.roiCropWarpMs += palmMetrics.roiCropWarpMs;
        metrics.hand.inferenceMs += palmMetrics.inferenceMs;
        metrics.hand.postprocessMs += palmMetrics.postprocessMs;
        if (!palmHand.detected)
        {
            error.clear();
            continue;
        }

        const auto trackingStarted = std::chrono::steady_clock::now();
        nextTrackedRegions[nextTrackedRegionCount++] = landmarksToRoi_.calculate(palmHand);
        metrics.trackingUpdateMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - trackingStarted).count();
        addDetectedHand(result, handRegion, palmHand);
    }

    trackedRegions_ = nextTrackedRegions;
    trackedRegionCount_ = nextTrackedRegionCount;
    error.clear();
    return true;
}

std::string HandPerceptionGraph::providerSummary() const
{
    return "palm=" + std::string{palmGraph_.providerName()}
        + ", hand=" + std::string{handGraph_.providerName()};
}
}
