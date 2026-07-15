#include "HandPerception/MediaPipeGraph/hand_landmark_graph.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ryoiki::hand_perception
{
HandLandmarkGraph::HandLandmarkGraph(std::unique_ptr<IHandLandmarkRunner> runner)
    : runner_{std::move(runner)}
{
    if (runner_ == nullptr)
    {
        throw std::invalid_argument{"Hand landmark graph requires a model runner."};
    }
}

bool HandLandmarkGraph::process(
    const buffers::FrameBuffer& frame,
    const geometry::RotatedRegion& region,
    const float confidenceThreshold,
    HandLandmarkResult& result,
    HandLandmarkGraphMetrics& metrics,
    std::string& error)
{
    using clock = std::chrono::steady_clock;
    result = {};
    geometry::HandPreprocessResult preprocessResult{};
    const auto preprocessStarted = clock::now();
    if (!geometryProcessor_.preprocessHand(frame, region, inputTensor_, preprocessResult))
    {
        error = "Hand ROI preprocessing failed.";
        return false;
    }
    metrics.roiCropWarpMs = std::chrono::duration<double, std::milli>(
        clock::now() - preprocessStarted).count();

    ModelRunResult runResult{};
    if (!runner_->run(inputTensor_, rawOutput_, runResult, error))
    {
        return false;
    }
    metrics.inferenceMs = runResult.inferenceMs;
    if (!std::isfinite(rawOutput_.presence) || !std::isfinite(rawOutput_.handedness))
    {
        error = "Hand model returned a non-finite scalar output.";
        return false;
    }
    if (rawOutput_.presence < confidenceThreshold)
    {
        error.clear();
        return true;
    }

    const auto postprocessStarted = clock::now();
    const float zScale = (std::max)(region.width, region.height) / 224.0F;
    const float cosine = std::cos(region.rotationRadiansClockwise);
    const float sine = std::sin(region.rotationRadiansClockwise);
    float left = std::numeric_limits<float>::max();
    float top = std::numeric_limits<float>::max();
    float right = std::numeric_limits<float>::lowest();
    float bottom = std::numeric_limits<float>::lowest();

    for (std::size_t index = 0; index < result.landmarks.size(); ++index)
    {
        const auto rawOffset = index * 3;
        const geometry::Point2f sourcePoint = preprocessResult.tensorToSource.transform({
            rawOutput_.imageLandmarks[rawOffset],
            rawOutput_.imageLandmarks[rawOffset + 1]});
        const float z = rawOutput_.imageLandmarks[rawOffset + 2] * zScale;
        const float worldX = rawOutput_.worldLandmarks[rawOffset];
        const float worldY = rawOutput_.worldLandmarks[rawOffset + 1];
        const float worldZ = rawOutput_.worldLandmarks[rawOffset + 2];
        if (!std::isfinite(sourcePoint.x) || !std::isfinite(sourcePoint.y)
            || !std::isfinite(z) || !std::isfinite(worldX)
            || !std::isfinite(worldY) || !std::isfinite(worldZ))
        {
            error = "Hand model returned non-finite landmarks.";
            result = {};
            return false;
        }

        result.landmarks[index] = {sourcePoint.x, sourcePoint.y, z};
        result.worldLandmarks[index] = {
            cosine * worldX - sine * worldY,
            sine * worldX + cosine * worldY,
            worldZ};
        left = (std::min)(left, sourcePoint.x);
        top = (std::min)(top, sourcePoint.y);
        right = (std::max)(right, sourcePoint.x);
        bottom = (std::max)(bottom, sourcePoint.y);
    }

    result.box = {left, top, right, bottom};
    result.confidence = rawOutput_.presence;
    result.handedness = rawOutput_.handedness;
    result.detected = true;
    metrics.postprocessMs = std::chrono::duration<double, std::milli>(
        clock::now() - postprocessStarted).count();
    error.clear();
    return true;
}

std::string_view HandLandmarkGraph::providerName() const noexcept
{
    return runner_->providerName();
}
}
