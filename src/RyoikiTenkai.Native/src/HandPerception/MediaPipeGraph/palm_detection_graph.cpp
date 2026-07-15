#include "HandPerception/MediaPipeGraph/palm_detection_graph.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace ryoiki::hand_perception
{
PalmDetectionGraph::PalmDetectionGraph(std::unique_ptr<IPalmDetectionRunner> runner)
    : runner_{std::move(runner)}
{
    if (runner_ == nullptr)
    {
        throw std::invalid_argument{"Palm detection graph requires a model runner."};
    }
}

bool PalmDetectionGraph::process(
    const buffers::FrameBuffer& frame,
    PalmDetectionResult& result,
    PalmDetectionGraphMetrics& metrics,
    std::string& error)
{
    using clock = std::chrono::steady_clock;
    geometry::PalmPreprocessResult preprocessResult{};
    const auto preprocessStarted = clock::now();
    if (!geometryProcessor_.preprocessPalm(frame, inputTensor_, preprocessResult))
    {
        error = "Palm input preprocessing failed.";
        return false;
    }
    metrics.preprocessMs = std::chrono::duration<double, std::milli>(
        clock::now() - preprocessStarted).count();

    ModelRunResult runResult{};
    if (!runner_->run(inputTensor_, rawOutput_, runResult, error))
    {
        return false;
    }
    metrics.inferenceMs = runResult.inferenceMs;

    const auto postprocessStarted = clock::now();
    postprocessor_.process(rawOutput_, preprocessResult.transform, result);
    metrics.postprocessMs = std::chrono::duration<double, std::milli>(
        clock::now() - postprocessStarted).count();
    error.clear();
    return true;
}

std::string_view PalmDetectionGraph::providerName() const noexcept
{
    return runner_->providerName();
}
}
