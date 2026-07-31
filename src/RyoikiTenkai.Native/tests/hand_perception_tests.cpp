#include "Buffers/frame_buffer.h"
#include "Buffers/tensor_buffer.h"
#include "Geometry/hand_geometry_processor.h"
#include "Features/Cad/cad_hand_binding.h"
#include "HandPerception/MediaPipeGraph/hand_landmark_graph.h"
#include "HandPerception/MediaPipeGraph/hand_landmarks_to_roi.h"
#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"
#include "HandPerception/MediaPipeGraph/palm_detection_graph.h"
#include "HandPerception/MediaPipeGraph/palm_detection_postprocessor.h"
#include "HandPerception/MediaPipeGraph/palm_detection_to_roi.h"
#include "ryoiki_native.h"

#include "HandInput/Measurements/hand_measurement_extractor.h"
#include "HandInput/Measurements/multi_hand_measurement_stage.h"
#include "HandInput/Measurements/palm_basis_rotation_tracker.h"
#include "HandInput/Measurements/palm_rotation_eskf.h"
#include "HandInput/Measurements/palm_rotation_one_euro_filter.h"
#include "HandInput/Measurements/rotation_observation_gate.h"
#include "HandInput/Measurements/weighted_palm_rotation_tracker.h"
#include "HandInput/Publication/latest_hand_state_slot.h"
#include "HandInput/Publication/ordered_hand_event_ring.h"
#include "HandInput/Recognition/domain_expansion_state_recognizer.h"
#include "HandInput/Recognition/open_palm_state_recognizer.h"
#include "HandInput/Recognition/swipe_event_recognizer.h"
#include "HandInput/Recognition/timed_state_stabilizer.h"
#include "HandPerception/ModelRunners/ort_hand_landmark_runner.h"
#include "HandPerception/ModelRunners/ort_palm_detection_runner.h"
#include "HandPerception/ModelRunners/hand_landmark_model_contract.h"
#if defined(RYOIKI_ORT_DIRECTML)
#include "Geometry/d3d12_hand_geometry_processor.h"
#include "Runtime/d3d11_device.h"
#include "Runtime/directml_runtime.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using ryoiki::hand_perception::PalmDetectionRawOutput;

void require(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error{message};
    }
}

void requireNear(const float actual, const float expected, const float tolerance, const char* message)
{
    require(std::isfinite(actual), "Expected a finite value.");
    require(std::abs(actual - expected) <= tolerance, message);
}

void clearRawOutput(PalmDetectionRawOutput& output)
{
    std::fill(output.regressions().begin(), output.regressions().end(), 0.0F);
    std::fill(output.scores().begin(), output.scores().end(), -100.0F);
}

void setDetection(
    PalmDetectionRawOutput& output,
    const std::size_t anchorIndex,
    const float scoreLogit,
    const float centerOffsetX,
    const float centerOffsetY,
    const float width,
    const float height)
{
    output.scores()[anchorIndex] = scoreLogit;
    const auto offset = anchorIndex * PalmDetectionRawOutput::kRegressionCount;
    output.regressions()[offset] = centerOffsetX;
    output.regressions()[offset + 1] = centerOffsetY;
    output.regressions()[offset + 2] = width;
    output.regressions()[offset + 3] = height;
}

ryoiki::buffers::FrameBuffer createFrame()
{
    ryoiki::buffers::FrameBuffer frame;
    require(frame.prepare(4, 2, 42, 1000), "Test frame preparation failed.");
    auto& pixels = frame.writablePixels();
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4)
    {
        pixels[offset] = 10;
        pixels[offset + 1] = 20;
        pixels[offset + 2] = 30;
        pixels[offset + 3] = 255;
    }
    return frame;
}

void testSigmoidThresholdAndAnchorDecode()
{
    PalmDetectionRawOutput rawOutput;
    clearRawOutput(rawOutput);
    setDetection(rawOutput, 0, -0.01F, 0.0F, 0.0F, 2.0F, 4.0F);
    setDetection(rawOutput, 2, 0.0F, 0.0F, 0.0F, 2.0F, 4.0F);

    ryoiki::hand_perception::PalmDetectionPostprocessor postprocessor{{0.5F, 0.3F}};
    ryoiki::hand_perception::PalmDetectionResult result;
    postprocessor.process(rawOutput, {}, result);

    require(result.size() == 1, "Sigmoid score threshold did not retain exactly one detection.");
    requireNear(result[0].score, 0.5F, 0.00001F, "Zero logit did not decode to a 0.5 score.");
    // Anchor 2 is the next 24x24 cell: center ((1.5 / 24) * 192, 4) = (12, 4).
    requireNear(result[0].box.left, 11.0F, 0.0001F, "Anchor-decoded box left is incorrect.");
    requireNear(result[0].box.top, 2.0F, 0.0001F, "Anchor-decoded box top is incorrect.");
    requireNear(result[0].box.right, 13.0F, 0.0001F, "Anchor-decoded box right is incorrect.");
    requireNear(result[0].box.bottom, 6.0F, 0.0001F, "Anchor-decoded box bottom is incorrect.");
}

void testLetterboxInverseAndKeypointDecode()
{
    PalmDetectionRawOutput rawOutput;
    clearRawOutput(rawOutput);
    setDetection(rawOutput, 0, 4.0F, 6.0F, 8.0F, 4.0F, 8.0F);
    const auto regressionOffset = std::size_t{4};
    rawOutput.regressions()[regressionOffset] = 8.0F;
    rawOutput.regressions()[regressionOffset + 1] = 12.0F;

    const ryoiki::geometry::LetterboxTransform transform{2.0F, 4.0F, 10.0F, 20.0F};
    ryoiki::hand_perception::PalmDetectionPostprocessor postprocessor;
    ryoiki::hand_perception::PalmDetectionResult result;
    postprocessor.process(rawOutput, transform, result);

    require(result.size() == 1, "Letterbox test detection was not selected.");
    requireNear(result[0].box.left, -1.25F, 0.0001F, "Letterbox inverse box left is incorrect.");
    requireNear(result[0].box.top, -3.375F, 0.0001F, "Letterbox inverse box top is incorrect.");
    requireNear(result[0].box.right, 0.75F, 0.0001F, "Letterbox inverse box right is incorrect.");
    requireNear(result[0].box.bottom, -1.375F, 0.0001F, "Letterbox inverse box bottom is incorrect.");
    requireNear(result[0].keypoints[0].x, 0.75F, 0.0001F, "Letterbox inverse keypoint X is incorrect.");
    requireNear(result[0].keypoints[0].y, -1.375F, 0.0001F, "Letterbox inverse keypoint Y is incorrect.");
}

void testNmsAndNonFiniteExclusion()
{
    PalmDetectionRawOutput rawOutput;
    clearRawOutput(rawOutput);
    setDetection(rawOutput, 0, 6.0F, 0.0F, 0.0F, 6.0F, 6.0F);
    setDetection(rawOutput, 1, 5.0F, 0.0F, 0.0F, 6.0F, 6.0F);
    setDetection(rawOutput, 4, 4.0F, 0.0F, 0.0F, 4.0F, 4.0F);
    setDetection(rawOutput, 6, 8.0F, 0.0F, 0.0F, 4.0F, 4.0F);
    rawOutput.regressions()[6 * PalmDetectionRawOutput::kRegressionCount] =
        std::numeric_limits<float>::quiet_NaN();
    rawOutput.scores()[8] = std::numeric_limits<float>::quiet_NaN();

    ryoiki::hand_perception::PalmDetectionPostprocessor postprocessor;
    ryoiki::hand_perception::PalmDetectionResult result;
    postprocessor.process(rawOutput, {}, result);

    require(result.size() == 2, "NMS or non-finite exclusion returned an incorrect count.");
    require(result[0].score > result[1].score, "Postprocessor results are not score-sorted.");
    requireNear(result[0].box.left, 1.0F, 0.0001F, "NMS did not retain the highest-score overlap.");
    requireNear(result[1].box.left, 18.0F, 0.0001F, "Non-overlapping detection was not retained.");
}

void testMaximumDetectionCount()
{
    PalmDetectionRawOutput rawOutput;
    clearRawOutput(rawOutput);
    for (std::size_t detectionIndex = 0; detectionIndex < 5; ++detectionIndex)
    {
        setDetection(rawOutput, detectionIndex * 2, 10.0F - static_cast<float>(detectionIndex),
            0.0F, 0.0F, 2.0F, 2.0F);
    }

    ryoiki::hand_perception::PalmDetectionPostprocessor postprocessor;
    ryoiki::hand_perception::PalmDetectionResult result;
    postprocessor.process(rawOutput, {}, result);
    require(result.size() == ryoiki::hand_perception::PalmDetectionResult::kCapacity,
        "Postprocessor did not cap detections at four.");
    require(result[0].score > result[3].score, "Capped results did not retain score order.");
    requireNear(result[3].box.left, 27.0F, 0.0001F, "Detection cap did not retain the four highest scores.");
}

void testPalmDetectionToUprightRoi()
{
    ryoiki::hand_perception::PalmDetection detection;
    detection.box = {10.0F, 20.0F, 30.0F, 60.0F};
    detection.keypoints[0] = {20.0F, 50.0F};
    detection.keypoints[2] = {20.0F, 30.0F};

    const ryoiki::hand_perception::PalmDetectionToRoiCalculator calculator;
    const auto region = calculator.calculate(detection);

    requireNear(region.center.x, 20.0F, 0.0001F, "Upright ROI center X is incorrect.");
    requireNear(region.center.y, 20.0F, 0.0001F, "Upright ROI center Y shift is incorrect.");
    requireNear(region.width, 104.0F, 0.0001F, "Upright ROI width is incorrect.");
    requireNear(region.height, 104.0F, 0.0001F, "Upright ROI height is incorrect.");
    requireNear(region.rotationRadiansClockwise, 0.0F, 0.0001F,
        "Upright ROI rotation is incorrect.");
}

void testPalmDetectionToRotatedRoi()
{
    ryoiki::hand_perception::PalmDetection detection;
    detection.box = {10.0F, 20.0F, 50.0F, 40.0F};
    detection.keypoints[0] = {20.0F, 30.0F};
    detection.keypoints[2] = {40.0F, 30.0F};

    const ryoiki::hand_perception::PalmDetectionToRoiCalculator calculator;
    const auto region = calculator.calculate(detection);

    requireNear(region.center.x, 40.0F, 0.0001F, "Rotated ROI center X shift is incorrect.");
    requireNear(region.center.y, 30.0F, 0.0001F, "Rotated ROI center Y is incorrect.");
    requireNear(region.width, 104.0F, 0.0001F, "Rotated ROI width is incorrect.");
    requireNear(region.height, 104.0F, 0.0001F, "Rotated ROI height is incorrect.");
    requireNear(region.rotationRadiansClockwise, std::numbers::pi_v<float> / 2.0F, 0.0001F,
        "Rotated ROI rotation sign is incorrect.");
}

void testHandRawCoordinatesMapToSourceRoi()
{
    const auto frame = createFrame();
    ryoiki::geometry::HandGeometryProcessor processor;
    ryoiki::buffers::FloatTensorBuffer input{{1, 224, 224, 3}};
    ryoiki::geometry::HandPreprocessResult preprocessResult{};
    const ryoiki::geometry::RotatedRegion region{{2.0F, 1.0F}, 4.0F, 2.0F, 0.0F};

    require(processor.preprocessHand(frame, region, input, preprocessResult),
        "Hand preprocessing failed while testing raw coordinate mapping.");
    const auto topLeft = preprocessResult.tensorToSource.transform({0.0F, 0.0F});
    const auto center = preprocessResult.tensorToSource.transform({112.0F, 112.0F});
    const auto bottomRight = preprocessResult.tensorToSource.transform({224.0F, 224.0F});

    requireNear(topLeft.x, 0.0F, 0.0002F, "Raw hand X=0 did not map to the ROI left edge.");
    requireNear(topLeft.y, 0.0F, 0.0002F, "Raw hand Y=0 did not map to the ROI top edge.");
    requireNear(center.x, region.center.x, 0.0002F, "Raw hand X=112 did not map to the ROI center.");
    requireNear(center.y, region.center.y, 0.0002F, "Raw hand Y=112 did not map to the ROI center.");
    requireNear(bottomRight.x, 4.0F, 0.0002F, "Raw hand X=224 did not map to the ROI right edge.");
    requireNear(bottomRight.y, 2.0F, 0.0002F, "Raw hand Y=224 did not map to the ROI bottom edge.");
}

void testHandLandmarksToRoiUsesPartialLandmarks()
{
    ryoiki::hand_perception::HandLandmarkResult hand;
    constexpr std::array<std::size_t, 12> kPartialLandmarkIndices{
        0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18};
    for (const auto index : kPartialLandmarkIndices)
    {
        hand.landmarks[index] = {30.0F, 30.0F, 0.0F};
    }
    hand.landmarks[0] = {10.0F, 30.0F, 0.0F};
    hand.landmarks[1] = {10.0F, 20.0F, 0.0F};
    hand.landmarks[2] = {10.0F, 40.0F, 0.0F};
    hand.landmarks[3] = {20.0F, 20.0F, 0.0F};
    hand.landmarks[5] = {50.0F, 20.0F, 0.0F};
    hand.landmarks[6] = {40.0F, 20.0F, 0.0F};
    hand.landmarks[9] = {50.0F, 30.0F, 0.0F};
    hand.landmarks[10] = {40.0F, 40.0F, 0.0F};
    hand.landmarks[13] = {50.0F, 40.0F, 0.0F};
    hand.landmarks[14] = {30.0F, 40.0F, 0.0F};
    hand.landmarks[17] = {20.0F, 40.0F, 0.0F};
    hand.landmarks[18] = {50.0F, 30.0F, 0.0F};

    // Fingertips are intentionally excluded by the MediaPipe partial-landmark index set.
    hand.landmarks[4] = {-1000.0F, -1000.0F, 0.0F};
    hand.landmarks[20] = {1000.0F, 1000.0F, 0.0F};

    const ryoiki::hand_perception::HandLandmarksToRoiCalculator calculator;
    const auto region = calculator.calculate(hand);

    requireNear(region.rotationRadiansClockwise, std::numbers::pi_v<float> / 2.0F, 0.0001F,
        "Landmarks-to-ROI rotation is incorrect.");
    requireNear(region.center.x, 34.0F, 0.0002F,
        "Landmarks-to-ROI rotated Y shift is incorrect.");
    requireNear(region.center.y, 30.0F, 0.0002F,
        "Landmarks-to-ROI center Y is incorrect.");
    requireNear(region.width, 80.0F, 0.0002F,
        "Landmarks-to-ROI scale 2.0 was not applied.");
    requireNear(region.height, 80.0F, 0.0002F,
        "Landmarks-to-ROI did not produce a square region.");
}

class FakeHandLandmarkRunner final : public ryoiki::hand_perception::IHandLandmarkRunner
{
public:
    [[nodiscard]] ryoiki::hand_perception::ExecutionProvider executionProvider() const noexcept override
    {
        return ryoiki::hand_perception::ExecutionProvider::Cpu;
    }

    [[nodiscard]] std::string_view providerName() const noexcept override
    {
        return "FakeHandExecutionProvider";
    }

    bool run(
        const ryoiki::buffers::FloatTensorBuffer& input,
        ryoiki::hand_perception::HandLandmarkRawOutput& output,
        ryoiki::hand_perception::ModelRunResult& result,
        std::string& error) override
    {
        wasCalled = true;
        ++runCallCount;
        receivedExpectedShape = input.shape() == std::array<std::int64_t, 4>{1, 224, 224, 3};
        if (!succeeds || std::ranges::find(failOnCalls, runCallCount) != failOnCalls.end())
        {
            error = "fake hand runner failure";
            return false;
        }
        output = outputs.empty()
            ? rawOutput
            : outputs[(std::min)(runCallCount - 1, outputs.size() - 1)];
        result.inferenceMs = 7.5;
        error.clear();
        return true;
    }

    ryoiki::hand_perception::HandLandmarkRawOutput rawOutput{};
    std::vector<ryoiki::hand_perception::HandLandmarkRawOutput> outputs;
    std::vector<std::size_t> failOnCalls;
    bool succeeds{true};
    bool wasCalled{false};
    bool receivedExpectedShape{false};
    std::size_t runCallCount{0};
};

void testHandLandmarkGraphDecodeAndThreshold()
{
    auto runner = std::make_unique<FakeHandLandmarkRunner>();
    auto* observer = runner.get();
    observer->rawOutput.presence = 0.8F;
    observer->rawOutput.handedness = 0.25F;
    for (std::size_t index = 0; index < 21; ++index)
    {
        const auto offset = index * 3;
        observer->rawOutput.imageLandmarks[offset] = index == 0 ? 0.0F : 224.0F;
        observer->rawOutput.imageLandmarks[offset + 1] = index == 0 ? 0.0F : 224.0F;
        observer->rawOutput.imageLandmarks[offset + 2] = 112.0F;
        observer->rawOutput.worldLandmarks[offset] = 1.0F;
        observer->rawOutput.worldLandmarks[offset + 1] = 2.0F;
        observer->rawOutput.worldLandmarks[offset + 2] = 3.0F;
    }

    ryoiki::hand_perception::HandLandmarkGraph graph{std::move(runner)};
    const auto frame = createFrame();
    ryoiki::hand_perception::HandLandmarkResult result;
    ryoiki::hand_perception::HandLandmarkGraphMetrics metrics{};
    std::string error;
    require(graph.process(frame, {{2.0F, 1.0F}, 4.0F, 2.0F, 0.0F}, 0.7F,
        result, metrics, error), "Hand landmark graph failed with valid fake output.");
    require(observer->wasCalled && observer->receivedExpectedShape,
        "Hand landmark graph did not invoke its runner with [1,224,224,3].");
    require(result.detected, "Hand landmark graph did not accept presence above threshold.");
    requireNear(result.landmarks[0].x, 0.0F, 0.0002F, "Decoded landmark left edge is incorrect.");
    requireNear(result.landmarks[0].y, 0.0F, 0.0002F, "Decoded landmark top edge is incorrect.");
    requireNear(result.landmarks[0].z, 2.0F, 0.0002F, "Decoded landmark Z scale is incorrect.");
    requireNear(result.landmarks[1].x, 4.0F, 0.0002F, "Decoded landmark right edge is incorrect.");
    requireNear(result.landmarks[1].y, 2.0F, 0.0002F, "Decoded landmark bottom edge is incorrect.");
    requireNear(result.worldLandmarks[0].x, 1.0F, 0.0002F, "World landmark X is incorrect.");
    requireNear(result.worldLandmarks[0].y, 2.0F, 0.0002F, "World landmark Y is incorrect.");
    requireNear(result.worldLandmarks[0].z, 3.0F, 0.0002F, "World landmark Z is incorrect.");
    requireNear(result.box.left, 0.0F, 0.0002F, "Decoded hand box left is incorrect.");
    requireNear(result.box.bottom, 2.0F, 0.0002F, "Decoded hand box bottom is incorrect.");
    requireNear(result.confidence, 0.8F, 0.0001F, "Hand confidence was not propagated.");
    requireNear(result.handedness, 0.25F, 0.0001F, "Handedness was not propagated.");
    requireNear(static_cast<float>(metrics.inferenceMs), 7.5F, 0.0001F,
        "Hand inference timing was not propagated.");

    observer->rawOutput.presence = 0.69F;
    require(graph.process(frame, {{2.0F, 1.0F}, 4.0F, 2.0F, 0.0F}, 0.7F,
        result, metrics, error), "Low-confidence hand output should not fail the graph.");
    require(!result.detected && error.empty(),
        "Low-confidence hand output was not returned as a clean no-detection result.");
}

void testHandLandmarkGraphRejectsInvalidOutput()
{
    auto runner = std::make_unique<FakeHandLandmarkRunner>();
    auto* observer = runner.get();
    observer->rawOutput.presence = 0.8F;
    observer->rawOutput.handedness = 0.5F;
    observer->rawOutput.imageLandmarks[0] = std::numeric_limits<float>::quiet_NaN();
    ryoiki::hand_perception::HandLandmarkGraph graph{std::move(runner)};
    const auto frame = createFrame();
    ryoiki::hand_perception::HandLandmarkResult result;
    ryoiki::hand_perception::HandLandmarkGraphMetrics metrics{};
    std::string error;
    require(!graph.process(frame, {{2.0F, 1.0F}, 4.0F, 2.0F, 0.0F}, 0.7F,
        result, metrics, error), "Hand landmark graph accepted non-finite landmarks.");
    require(!result.detected && !error.empty(),
        "Invalid hand output did not produce a clean failure result.");
}

class FakePalmDetectionRunner final : public ryoiki::hand_perception::IPalmDetectionRunner
{
public:
    explicit FakePalmDetectionRunner(const bool succeeds) : succeeds_{succeeds}
    {
    }

    [[nodiscard]] ryoiki::hand_perception::ExecutionProvider executionProvider() const noexcept override
    {
        return ryoiki::hand_perception::ExecutionProvider::Cpu;
    }

    [[nodiscard]] std::string_view providerName() const noexcept override
    {
        return "FakeExecutionProvider";
    }

    bool run(
        const ryoiki::buffers::FloatTensorBuffer& input,
        PalmDetectionRawOutput& output,
        ryoiki::hand_perception::ModelRunResult& result,
        std::string& error) override
    {
        wasCalled = true;
        ++runCallCount;
        receivedExpectedShape = input.shape() == std::array<std::int64_t, 4>{1, 192, 192, 3};
        if (!succeeds_ || std::ranges::find(failOnCalls, runCallCount) != failOnCalls.end())
        {
            error = "fake runner failure";
            return false;
        }

        clearRawOutput(output);
        for (const auto anchorIndex : detectionAnchorIndices)
        {
            setDetection(output, anchorIndex, 5.0F, 0.0F, 0.0F, 2.0F, 2.0F);
        }
        result.inferenceMs = 12.5;
        error.clear();
        return true;
    }

    bool wasCalled{false};
    bool receivedExpectedShape{false};
    std::size_t runCallCount{0};
    std::vector<std::size_t> failOnCalls;
    std::vector<std::size_t> detectionAnchorIndices{0};

private:
    bool succeeds_{false};
};

void testPalmDetectionGraphSuccess()
{
    auto runner = std::make_unique<FakePalmDetectionRunner>(true);
    auto* runnerObserver = runner.get();
    ryoiki::hand_perception::PalmDetectionGraph graph{std::move(runner)};
    require(graph.providerName() == "FakeExecutionProvider", "Graph did not expose its runner provider name.");

    auto frame = createFrame();
    ryoiki::hand_perception::PalmDetectionResult result;
    ryoiki::hand_perception::PalmDetectionGraphMetrics metrics{};
    std::string error{"stale error"};
    require(graph.process(frame, result, metrics, error), "Graph failed with a successful fake runner.");
    require(runnerObserver->wasCalled && runnerObserver->receivedExpectedShape,
        "Graph did not invoke the fake runner with the expected input shape.");
    require(result.size() == 1, "Graph did not postprocess the fake runner output.");
    require(error.empty(), "Successful graph processing did not clear the error.");
    require(metrics.preprocessMs >= 0.0, "Graph reported a negative preprocess duration.");
    require(metrics.inferenceMs == 12.5, "Graph did not propagate runner inference timing.");
    require(metrics.postprocessMs >= 0.0, "Graph reported a negative postprocess duration.");
}

void testPalmDetectionGraphFailure()
{
    auto runner = std::make_unique<FakePalmDetectionRunner>(false);
    auto* runnerObserver = runner.get();
    ryoiki::hand_perception::PalmDetectionGraph graph{std::move(runner)};
    auto frame = createFrame();
    ryoiki::hand_perception::PalmDetectionResult result;
    ryoiki::hand_perception::PalmDetectionGraphMetrics metrics{};
    std::string error;

    require(!graph.process(frame, result, metrics, error), "Graph hid a fake runner failure.");
    require(runnerObserver->wasCalled, "Graph failure path did not invoke the runner.");
    require(error == "fake runner failure", "Graph did not preserve the runner error.");
}

ryoiki::hand_perception::HandLandmarkRawOutput createTrackedHandOutput(const float presence)
{
    ryoiki::hand_perception::HandLandmarkRawOutput output;
    output.presence = presence;
    output.handedness = 0.75F;
    for (std::size_t index = 0; index < 21; ++index)
    {
        const auto offset = index * 3;
        output.imageLandmarks[offset] = 112.0F;
        output.imageLandmarks[offset + 1] = 112.0F;
    }

    const auto setPoint = [&output](const std::size_t index, const float x, const float y)
    {
        const auto offset = index * 3;
        output.imageLandmarks[offset] = x;
        output.imageLandmarks[offset + 1] = y;
    };
    setPoint(0, 112.0F, 184.0F);
    setPoint(1, 80.0F, 152.0F);
    setPoint(2, 72.0F, 120.0F);
    setPoint(3, 64.0F, 88.0F);
    setPoint(5, 80.0F, 72.0F);
    setPoint(6, 72.0F, 56.0F);
    setPoint(9, 112.0F, 64.0F);
    setPoint(10, 112.0F, 48.0F);
    setPoint(13, 144.0F, 72.0F);
    setPoint(14, 152.0F, 56.0F);
    setPoint(17, 160.0F, 96.0F);
    setPoint(18, 168.0F, 80.0F);
    return output;
}

ryoiki::hand_perception::HandLandmarkRawOutput createTrackedHandOutput(
    const float presence,
    const float handedness)
{
    auto output = createTrackedHandOutput(presence);
    output.handedness = handedness;
    return output;
}

void testHandPerceptionTrackingAndPalmFallback()
{
    auto palmRunner = std::make_unique<FakePalmDetectionRunner>(true);
    auto* palmObserver = palmRunner.get();
    auto handRunner = std::make_unique<FakeHandLandmarkRunner>();
    auto* handObserver = handRunner.get();
    handObserver->outputs = {
        createTrackedHandOutput(0.8F),
        createTrackedHandOutput(0.49F),
        createTrackedHandOutput(0.8F)};

    ryoiki::hand_perception::HandPerceptionGraph graph{
        std::move(palmRunner), std::move(handRunner)};
    require(graph.providerSummary()
            == "palm=FakeExecutionProvider, hand=FakeHandExecutionProvider",
        "Hand perception graph provider summary is incorrect.");

    const auto frame = createFrame();
    ryoiki::hand_perception::HandPerceptionResult result;
    ryoiki::hand_perception::HandPerceptionGraphMetrics metrics{};
    std::string error;

    require(graph.process(frame, result, metrics, error),
        "Initial palm-to-hand perception frame failed.");
    require(!result.usedTracking && result.palms.size() == 1 && result.hand.detected,
        "Initial frame did not use palm detection and establish a hand.");
    require(palmObserver->runCallCount == 1 && handObserver->runCallCount == 1,
        "Initial frame invoked an incorrect model runner sequence.");
    const auto initialTrackId = result.trackIds[0];
    require(initialTrackId != 0,
        "Initial palm-to-hand perception did not assign a track ID.");

    require(graph.process(frame, result, metrics, error),
        "Tracked hand perception frame failed.");
    require(result.usedTracking && !result.hand.detected,
        "Tracking confidence below 0.5 did not produce a tracking miss.");
    require(palmObserver->runCallCount == 1 && handObserver->runCallCount == 2,
        "Tracking frame unexpectedly invoked palm detection.");

    require(graph.process(frame, result, metrics, error),
        "Palm fallback frame failed.");
    require(!result.usedTracking && result.palms.size() == 1 && result.hand.detected,
        "Frame after a tracking miss did not fall back to palm detection.");
    require(palmObserver->runCallCount == 2 && handObserver->runCallCount == 3,
        "Palm fallback invoked an incorrect model runner sequence.");
    require(result.trackIds[0] == initialTrackId,
        "Palm fallback replaced the temporarily lost hand's track ID.");
}

void testCalibrationPalmProbeKeepsTrackedRoi()
{
    auto palmRunner = std::make_unique<FakePalmDetectionRunner>(true);
    auto* palmObserver = palmRunner.get();
    auto handRunner = std::make_unique<FakeHandLandmarkRunner>();
    auto* handObserver = handRunner.get();
    handObserver->rawOutput = createTrackedHandOutput(0.8F);

    ryoiki::hand_perception::HandPerceptionGraph graph{
        std::move(palmRunner),
        std::move(handRunner),
        {.calibrationPalmIntervalFrames = 2}};
    const auto frame = createFrame();
    ryoiki::hand_perception::HandPerceptionResult result;
    ryoiki::hand_perception::HandPerceptionGraphMetrics metrics{};
    std::string error;

    require(graph.process(frame, result, metrics, error),
        "Calibration palm probe test could not establish tracking.");
    require(palmObserver->runCallCount == 1 && handObserver->runCallCount == 1,
        "Initial calibration probe test frame invoked an incorrect model sequence.");

    require(graph.process(frame, result, metrics, error),
        "Periodic calibration palm probe failed.");
    require(result.usedTracking && result.hand.detected && result.palms.size() == 0,
        "Calibration palm probe replaced or exposed the tracked ROI result.");
    require(palmObserver->runCallCount == 2 && handObserver->runCallCount == 2,
        "Periodic calibration palm probe did not invoke both model runners.");
    require(metrics.palm.inferenceMs == 12.5,
        "Calibration palm probe timing was not exposed in graph metrics.");
}

void testHandPerceptionTracksTwoHandsWithSequentialInference()
{
    auto palmRunner = std::make_unique<FakePalmDetectionRunner>(true);
    auto* palmObserver = palmRunner.get();
    palmObserver->detectionAnchorIndices = {0, 200};
    auto handRunner = std::make_unique<FakeHandLandmarkRunner>();
    auto* handObserver = handRunner.get();
    handObserver->rawOutput = createTrackedHandOutput(0.8F);

    ryoiki::hand_perception::HandPerceptionGraph graph{
        std::move(palmRunner),
        std::move(handRunner)};
    const auto frame = createFrame();
    ryoiki::hand_perception::HandPerceptionResult result;
    ryoiki::hand_perception::HandPerceptionGraphMetrics metrics{};
    std::string error;

    require(graph.process(frame, result, metrics, error),
        "Two-hand palm discovery failed.");
    require(result.handCount == 2 && result.hand.detected,
        "Two-hand palm discovery did not publish both hands and a primary hand.");
    require(handObserver->runCallCount == 2,
        "Two-hand discovery did not run landmark inference once per hand.");
    require(result.trackIds[0] != 0
            && result.trackIds[1] != 0
            && result.trackIds[0] != result.trackIds[1],
        "Two-hand discovery did not assign distinct track IDs.");
    const auto firstTrackIds = result.trackIds;

    require(graph.process(frame, result, metrics, error),
        "Two-hand ROI tracking failed.");
    require(result.usedTracking && result.handCount == 2,
        "Two-hand ROI tracking did not retain both hands.");
    require(palmObserver->runCallCount == 1 && handObserver->runCallCount == 4,
        "Two-hand tracking did not use sequential landmark-only inference.");
    require(result.trackIds[0] == firstTrackIds[0]
            && result.trackIds[1] == firstTrackIds[1],
        "Two-hand ROI tracking did not preserve track IDs.");
}

void testHandPerceptionUsesReliableHandednessAcrossPositionConflict()
{
    auto palmRunner = std::make_unique<FakePalmDetectionRunner>(true);
    auto* palmObserver = palmRunner.get();
    palmObserver->detectionAnchorIndices = {0, 200};
    auto handRunner = std::make_unique<FakeHandLandmarkRunner>();
    auto* handObserver = handRunner.get();
    handObserver->outputs = {
        createTrackedHandOutput(0.8F, 0.05F),
        createTrackedHandOutput(0.8F, 0.95F),
        createTrackedHandOutput(0.8F, 0.95F),
        createTrackedHandOutput(0.8F, 0.05F)};

    ryoiki::hand_perception::HandPerceptionGraph graph{
        std::move(palmRunner), std::move(handRunner)};
    const auto frame = createFrame();
    ryoiki::hand_perception::HandPerceptionResult result;
    ryoiki::hand_perception::HandPerceptionGraphMetrics metrics{};
    std::string error;

    require(graph.process(frame, result, metrics, error)
            && result.handCount == 2,
        "Reliable-handedness test could not establish both tracks.");
    const auto lowHandednessTrackId = result.trackIds[0];
    const auto highHandednessTrackId = result.trackIds[1];
    require(result.hands[0].handedness <= 0.20F
            && result.hands[1].handedness >= 0.80F,
        "Reliable-handedness test did not establish complementary identities.");

    require(graph.process(frame, result, metrics, error)
            && result.handCount == 2,
        "Reliable-handedness conflict frame failed.");
    require(result.trackIds[0] == lowHandednessTrackId
            && result.hands[0].handedness <= 0.20F,
        "Strong position/source evidence replaced the low-handedness identity.");
    require(result.trackIds[1] == highHandednessTrackId
            && result.hands[1].handedness >= 0.80F,
        "Strong position/source evidence replaced the high-handedness identity.");
}

void testHandPerceptionReacquiresOneOfTwoTracksWithoutReplacingItsId()
{
    auto palmRunner = std::make_unique<FakePalmDetectionRunner>(true);
    auto* palmObserver = palmRunner.get();
    palmObserver->detectionAnchorIndices = {0, 200};
    auto handRunner = std::make_unique<FakeHandLandmarkRunner>();
    auto* handObserver = handRunner.get();
    handObserver->outputs = {
        createTrackedHandOutput(0.8F),
        createTrackedHandOutput(0.8F),
        createTrackedHandOutput(0.8F),
        createTrackedHandOutput(0.49F),
        createTrackedHandOutput(0.8F)};

    ryoiki::hand_perception::HandPerceptionGraph graph{
        std::move(palmRunner), std::move(handRunner)};
    const auto frame = createFrame();
    ryoiki::hand_perception::HandPerceptionResult result;
    ryoiki::hand_perception::HandPerceptionGraphMetrics metrics{};
    std::string error;

    require(graph.process(frame, result, metrics, error)
            && result.handCount == 2,
        "Two-track reacquisition test could not establish both tracks.");
    const auto initialTrackIds = result.trackIds;

    require(graph.process(frame, result, metrics, error)
            && result.handCount == 2,
        "Collision-mode palm rediscovery did not restore the missing track.");
    require(result.trackIds[0] == initialTrackIds[0]
            && result.trackIds[1] == initialTrackIds[1],
        "Palm rediscovery replaced or swapped an existing track ID.");
    require(palmObserver->runCallCount == 2
            && handObserver->runCallCount == 5,
        "Two-track reacquisition invoked an incorrect model sequence.");
}

void testHandPerceptionContinuesAfterSingleFrameRunnerFailures()
{
    const auto frame = createFrame();

    {
        auto palmRunner = std::make_unique<FakePalmDetectionRunner>(true);
        auto* palmObserver = palmRunner.get();
        palmObserver->failOnCalls = {1};
        auto handRunner = std::make_unique<FakeHandLandmarkRunner>();
        auto* handObserver = handRunner.get();
        handObserver->rawOutput = createTrackedHandOutput(0.8F);
        ryoiki::hand_perception::HandPerceptionGraph graph{
            std::move(palmRunner), std::move(handRunner)};
        ryoiki::hand_perception::HandPerceptionResult result;
        ryoiki::hand_perception::HandPerceptionGraphMetrics metrics{};
        std::string error;

        require(!graph.process(frame, result, metrics, error),
            "One-frame palm runner failure was not surfaced as recoverable work failure.");
        require(!error.empty(), "One-frame palm runner failure did not retain its diagnostic.");
        require(graph.process(frame, result, metrics, error),
            "Perception graph did not continue after a one-frame palm runner failure.");
        require(result.hand.detected && !result.usedTracking,
            "Frame after palm runner recovery did not establish a tracked hand.");
        require(palmObserver->runCallCount == 2 && handObserver->runCallCount == 1,
            "Palm runner recovery invoked an incorrect model sequence.");
    }

    {
        auto palmRunner = std::make_unique<FakePalmDetectionRunner>(true);
        auto* palmObserver = palmRunner.get();
        auto handRunner = std::make_unique<FakeHandLandmarkRunner>();
        auto* handObserver = handRunner.get();
        handObserver->rawOutput = createTrackedHandOutput(0.8F);
        handObserver->failOnCalls = {2};
        ryoiki::hand_perception::HandPerceptionGraph graph{
            std::move(palmRunner), std::move(handRunner)};
        ryoiki::hand_perception::HandPerceptionResult result;
        ryoiki::hand_perception::HandPerceptionGraphMetrics metrics{};
        std::string error;

        require(graph.process(frame, result, metrics, error) && result.hand.detected,
            "Hand runner recovery test could not establish initial tracking state.");
        require(!graph.process(frame, result, metrics, error),
            "One-frame tracked hand runner failure was not surfaced.");
        require(!error.empty(), "One-frame hand runner failure did not retain its diagnostic.");
        require(graph.process(frame, result, metrics, error),
            "Perception graph did not continue after a one-frame hand runner failure.");
        require(result.usedTracking && result.hand.detected,
            "Tracked ROI state was lost after a recoverable hand runner failure.");
        require(palmObserver->runCallCount == 1 && handObserver->runCallCount == 3,
            "Hand runner recovery unexpectedly reran palm detection or skipped a hand frame.");
    }
}

ryoiki::hand_perception::HandLandmarkResult createOpenHandStateTestInput()
{
    ryoiki::hand_perception::HandLandmarkResult hand{};
    hand.detected = true;
    hand.confidence = 0.9F;
    hand.handedness = 0.8F;
    auto& points = hand.worldLandmarks;
    points[0] = {0.0F, 0.0F, 0.0F};
    points[1] = {-0.20F, 0.24F, 0.0F}; points[2] = {-0.45F, 0.42F, 0.0F};
    points[3] = {-0.70F, 0.56F, 0.0F}; points[4] = {-0.94F, 0.68F, 0.0F};
    points[5] = {-0.42F, 0.82F, 0.0F}; points[6] = {-0.43F, 1.22F, 0.0F};
    points[7] = {-0.44F, 1.57F, 0.0F}; points[8] = {-0.45F, 1.88F, 0.0F};
    points[9] = {0.0F, 0.94F, 0.0F}; points[10] = {0.0F, 1.39F, 0.0F};
    points[11] = {0.0F, 1.78F, 0.0F}; points[12] = {0.0F, 2.12F, 0.0F};
    points[13] = {0.34F, 0.84F, 0.0F}; points[14] = {0.36F, 1.25F, 0.0F};
    points[15] = {0.37F, 1.60F, 0.0F}; points[16] = {0.38F, 1.91F, 0.0F};
    points[17] = {0.68F, 0.68F, 0.0F}; points[18] = {0.72F, 1.02F, 0.0F};
    points[19] = {0.75F, 1.31F, 0.0F}; points[20] = {0.78F, 1.56F, 0.0F};
    hand.landmarks = points;
    return hand;
}

void testHandMeasurementExtractorCanonicalization()
{
    using ryoiki::hand_input::measurements::HandMeasurementExtractor;
    using ryoiki::hand_input::measurements::HandMeasurementQuality;

    const auto original = createOpenHandStateTestInput();
    HandMeasurementExtractor extractor;
    const auto firstFrame = extractor.extract(original, 10, 1'000'000, 640, 480);
    const auto& first = firstFrame.hand;
    require(first.present && first.quality == HandMeasurementQuality::Valid,
        "Hand-state extractor rejected a valid open hand.");
    require(firstFrame.screenPalm.valid,
        "Hand-state extractor rejected a valid screen palm measurement.");
    require(firstFrame.topology.valid
            && firstFrame.topology.fingerStateMask == 0b1'1111U,
        "Gesture topology extraction did not identify the open hand.");
    requireNear(first.palmPosition.x, 0.12F, 1.0e-6F,
        "Palm position was not the wrist/MCP centroid.");
    requireNear(first.palmPosition.y, 0.656F, 1.0e-6F,
        "Palm position was not the wrist/MCP centroid.");
    require(first.usesWorldLandmarks, "Hand-state extractor ignored valid world landmarks.");
    requireNear(first.trackingQuality, 0.9F, 1.0e-6F,
        "Hand-state tracking quality did not preserve hand confidence.");
    for (const float extension : first.extension)
    {
        require(extension >= 0.95F, "Straight test finger produced a low extension score.");
    }

    auto transformed = original;
    constexpr float scale = 2.75F;
    constexpr ryoiki::hand_perception::Landmark3f translation{3.2F, -1.7F, 0.4F};
    for (std::size_t index = 0; index < transformed.worldLandmarks.size(); ++index)
    {
        const auto source = original.worldLandmarks[index];
        transformed.worldLandmarks[index] = {
            source.x * scale + translation.x,
            source.y * scale + translation.y,
            source.z * scale + translation.z};
    }
    const auto secondFrame = extractor.extract(transformed, 11, 1'033'333, 640, 480);
    const auto& second = secondFrame.hand;
    require(second.present, "Transformed hand-state input was rejected.");
    for (std::size_t index = 0; index < first.canonicalLandmarks.size(); ++index)
    {
        requireNear(second.canonicalLandmarks[index].x, first.canonicalLandmarks[index].x,
            1.0e-5F, "Canonical X changed after scale/translation.");
        requireNear(second.canonicalLandmarks[index].y, first.canonicalLandmarks[index].y,
            1.0e-5F, "Canonical Y changed after scale/translation.");
        requireNear(second.canonicalLandmarks[index].z, first.canonicalLandmarks[index].z,
            1.0e-5F, "Canonical Z changed after scale/translation.");
    }

    auto invalid = original;
    invalid.worldLandmarks[8].x = std::numeric_limits<float>::quiet_NaN();
    invalid.landmarks[8].x = std::numeric_limits<float>::quiet_NaN();
    const auto rejected =
        extractor.extract(invalid, 12, 1'066'666, 640, 480).hand;
    require(!rejected.present && rejected.quality == HandMeasurementQuality::InvalidLandmarks,
        "Hand-state extractor accepted a non-finite landmark.");

    auto missing = original;
    missing.detected = false;
    static_cast<void>(extractor.extract(missing, 13, 1'100'000, 640, 480));
    const auto reacquired =
        extractor.extract(transformed, 14, 2'000'000, 640, 480).hand;
    requireNear(reacquired.linearSpeed, 0.0F, 1.0e-6F,
        "Tracking loss did not reset palm velocity history.");

    auto invalidWorld = original;
    invalidWorld.worldLandmarks[8].x =
        std::numeric_limits<float>::quiet_NaN();
    const auto noCoordinateFallback =
        extractor.extract(invalidWorld, 15, 2'033'333, 640, 480).hand;
    require(!noCoordinateFallback.present
            && noCoordinateFallback.quality == HandMeasurementQuality::InvalidLandmarks,
        "World measurement silently changed to image coordinates.");
}

void testHandTopologyHistorySummarizesAndBounds()
{
    using namespace ryoiki::hand_input::measurements;
    HandTopologyHistory history;
    HandTopologyMeasurement first{};
    first.frameId = 10;
    first.timestampUs = 1'000'000;
    first.fingerStraightness.fill(0.9F);
    first.fingerStateMask = 0b1'1111U;
    first.signedPalmArea = 0.08F;
    first.palmCompression = 1.0F;
    first.palmDepthRange = 0.1F;
    first.handScale = 0.10F;
    first.centerX = 0.30F;
    first.centerY = 0.40F;
    first.valid = true;
    history.push(first);

    auto middle = first;
    middle.frameId = 11;
    middle.timestampUs = 1'033'333;
    middle.fingerStraightness.fill(0.45F);
    middle.fingerStateMask = 0b0'0011U;
    middle.signedPalmArea = 0.0F;
    middle.palmCompression = 0.35F;
    middle.palmDepthRange = 0.45F;
    middle.handScale = 0.13F;
    middle.centerX = 0.40F;
    middle.translationStepX = 0.10F;
    history.push(middle);

    auto last = middle;
    last.frameId = 12;
    last.timestampUs = 1'066'666;
    last.fingerStraightness.fill(0.1F);
    last.fingerStateMask = 0;
    last.signedPalmArea = -0.07F;
    last.palmCompression = 0.95F;
    last.palmDepthRange = 0.2F;
    last.handScale = 0.15F;
    last.centerX = 0.55F;
    last.centerY = 0.35F;
    last.translationStepX = 0.15F;
    last.translationStepY = -0.05F;
    history.push(last);

    const auto summary = history.summarize();
    require(summary.valid && summary.sampleCount == 3,
        "Topology history did not summarize all valid samples.");
    require(summary.startFingerStateMask == 0b1'1111U
            && summary.endFingerStateMask == 0
            && summary.fingerStateTransitionCount == 5,
        "Topology history did not retain finger-state transitions.");
    require(summary.fingerStraightnessRangeMax > 0.79F
            && summary.palmCompressionDrop > 0.64F
            && summary.palmDepthRangeMax == 0.45F,
        "Topology history did not retain finger or palm-turn evidence.");
    require(summary.signedPalmAreaRange > 0.14F
            && summary.translationDeltaX > 0.24F
            && summary.handScaleRatioDelta > 0.49F,
        "Topology history did not retain palm, translation, or scale change.");

    for (std::size_t index = 0;
        index < kHandTopologyHistoryCapacity + 5;
        ++index)
    {
        last.frameId++;
        // Keep the synthetic sequence inside the 2600 ms rolling window so
        // this assertion exercises the fixed-capacity bound specifically.
        last.timestampUs += 10'000;
        history.push(last);
    }
    require(history.size() == kHandTopologyHistoryCapacity,
        "Topology history exceeded its fixed capacity.");

    // A single missing observation is within the grace window and must not
    // discard the segment; only exceeding the tolerance should reset it.
    HandTopologyMeasurement invalid{};
    history.push(invalid);
    require(history.size() == kHandTopologyHistoryCapacity,
        "A single missing observation incorrectly broke the temporal segment.");
    for (std::uint32_t index = 0;
        index < kHandTopologyGapToleranceFrames;
        ++index)
    {
        history.push(invalid);
    }
    require(history.size() == 0 && !history.summarize().valid,
        "Exceeding the gap tolerance did not reset the temporal segment.");
}

void testHandTopologyHistoryTolerateShortGapAndAccumulatePath()
{
    using namespace ryoiki::hand_input::measurements;
    HandTopologyHistory history;

    // Three samples that move away from the origin and then back to it. A
    // correct accumulator reports the traveled path length, not the (near
    // zero) net displacement between the first and last sample.
    HandTopologyMeasurement start{};
    start.frameId = 1;
    start.timestampUs = 0;
    start.valid = true;
    history.push(start);

    HandTopologyMeasurement outbound{};
    outbound.frameId = 2;
    outbound.timestampUs = 33'333;
    outbound.translationStepX = 0.30F;
    outbound.translationStepY = 0.0F;
    outbound.valid = true;
    history.push(outbound);

    HandTopologyMeasurement returned{};
    returned.frameId = 3;
    returned.timestampUs = 66'666;
    returned.translationStepX = -0.30F;
    returned.translationStepY = 0.0F;
    returned.valid = true;
    history.push(returned);

    const auto roundTripSummary = history.summarize();
    require(roundTripSummary.valid, "Round-trip topology summary was invalid.");
    requireNear(roundTripSummary.palmTravel, 0.60F, 1.0e-4F,
        "Accumulated palm travel did not sum per-step path length.");
    requireNear(roundTripSummary.translationDistance, 0.0F, 1.0e-4F,
        "Net translation distance should cancel for a round trip.");
    require(roundTripSummary.palmTravel > roundTripSummary.translationDistance,
        "Palm travel must exceed endpoint distance for a non-monotonic path.");

    // Missing observations up to the tolerance keep the segment (and its
    // sample count) intact; the next valid sample continues the same
    // segment instead of starting a new one.
    require(history.size() == 3,
        "Setup for the gap-tolerance check lost samples unexpectedly.");
    for (std::uint32_t index = 0;
        index < kHandTopologyGapToleranceFrames;
        ++index)
    {
        history.noteMissingObservation();
        require(history.size() == 3,
            "A within-tolerance gap discarded the retained segment.");
    }
    require(history.missingObservationCount() == kHandTopologyGapToleranceFrames,
        "Gap counter did not track consecutive missing observations.");

    HandTopologyMeasurement continued{};
    continued.frameId = 4;
    continued.timestampUs = 500'000;
    continued.valid = true;
    history.push(continued);
    require(history.size() == 4 && history.missingObservationCount() == 0,
        "A valid sample after a short gap did not continue the same segment.");

    // One more missing observation than the tolerance allows must discard
    // the segment entirely, mirroring the perception graph's own
    // missing-track grace window.
    for (std::uint32_t index = 0;
        index <= kHandTopologyGapToleranceFrames;
        ++index)
    {
        history.noteMissingObservation();
    }
    require(history.size() == 0 && !history.summarize().valid,
        "Exceeding the missing-track grace window did not reset the segment.");
}

void testHandTopologyHistoryPrunesToRollingTimeWindow()
{
    using namespace ryoiki::hand_input::measurements;
    HandTopologyHistory history;

    // At a 60 FPS-equivalent 16.66 ms cadence the 2600 ms window elapses
    // long before the 192-sample capacity does, so the oldest samples must
    // be evicted by elapsed time, not only by the fixed-size ring buffer.
    constexpr std::uint64_t kFrameIntervalUs = 16'666;
    constexpr std::size_t kFrameCount = 220;
    for (std::size_t index = 0; index < kFrameCount; ++index)
    {
        HandTopologyMeasurement sample{};
        sample.frameId = index;
        sample.timestampUs = index * kFrameIntervalUs;
        sample.valid = true;
        history.push(sample);
    }

    require(history.size() < kHandTopologyHistoryCapacity,
        "Time-based pruning did not evict samples before capacity was reached.");
    const auto summary = history.summarize();
    require(summary.valid, "Time-windowed topology summary was invalid.");
    require(summary.durationUs <= kHandTopologyWindowUs,
        "Retained topology history exceeded the 2600 ms rolling window.");
    require(summary.lastFrameId == kFrameCount - 1,
        "Time-based pruning discarded the newest sample.");
}

void testMultiHandRelationOrderingIsDeterministic()
{
    using ryoiki::hand_input::measurements::MultiHandMeasurementStage;
    using ryoiki::hand_input::measurements::TwoHandOrdering;

    // Identical geometry for both cases below: hands[0] (track 5) sits to
    // the right of hands[1] (track 9). Only the filtered handedness gap
    // differs, so any ordering difference is attributable to it alone.
    const auto buildObservations = [](const float handedness0, const float handedness1)
    {
        ryoiki::hand_perception::HandPerceptionResult observations{};
        observations.handCount = 2;
        observations.trackIds = {5, 9};
        observations.filteredHandedness = {handedness0, handedness1};
        observations.hands[0] = createOpenHandStateTestInput();
        observations.hands[1] = createOpenHandStateTestInput();
        for (auto& landmark : observations.hands[0].landmarks)
        {
            landmark.x += 500.0F;
        }
        for (auto& landmark : observations.hands[1].landmarks)
        {
            landmark.x += 100.0F;
        }
        return observations;
    };

    // Reliable handedness gap (>= kReliableHandednessGap), deliberately
    // opposite to screen position: the lower-handedness hand (track 5) is
    // on the right. Ordering must still follow handedness, not position.
    {
        auto observations = buildObservations(0.10F, 0.90F);
        MultiHandMeasurementStage stage;
        const auto frame = stage.extract(observations, 1, 1'000'000, 640, 480);
        require(frame.relation.valid && frame.relation.ordering == TwoHandOrdering::Handedness,
            "A reliable handedness gap did not select handedness ordering.");
        require(frame.relation.firstTrackId == 5 && frame.relation.secondTrackId == 9,
            "Handedness ordering did not place the lower handedness hand first.");
        require(frame.relation.deltaX < 0.0F,
            "Handedness ordering did not override the conflicting screen-space delta sign.");
    }

    // Ambiguous handedness gap (< kReliableHandednessGap) with the same
    // geometry: ordering must fall back to screen position (leftmost
    // first), flipping both the track order and the delta sign versus the
    // handedness-reliable case above.
    {
        auto observations = buildObservations(0.52F, 0.50F);
        MultiHandMeasurementStage stage;
        const auto frame = stage.extract(observations, 1, 1'000'000, 640, 480);
        require(frame.relation.valid
                && frame.relation.ordering == TwoHandOrdering::ScreenPosition,
            "An ambiguous handedness gap did not fall back to screen-position ordering.");
        require(frame.relation.firstTrackId == 9 && frame.relation.secondTrackId == 5,
            "Screen-position ordering did not place the leftmost hand first.");
        require(frame.relation.deltaX > 0.0F,
            "Screen-position ordering produced a non-positive delta for the rightmost second hand.");
    }
}

void testMultiHandMeasurementStageKeepsPerTrackHistory()
{
    using ryoiki::hand_input::measurements::MultiHandMeasurementStage;
    ryoiki::hand_perception::HandPerceptionResult observations{};
    observations.handCount = 2;
    observations.trackIds = {41, 84};
    observations.hands[0] = createOpenHandStateTestInput();
    observations.hands[1] = createOpenHandStateTestInput();
    for (auto& landmark : observations.hands[0].landmarks)
    {
        landmark.x = landmark.x * 40.0F + 160.0F;
        landmark.y = landmark.y * 40.0F + 120.0F;
        landmark.z *= 40.0F;
    }
    for (auto& landmark : observations.hands[1].landmarks)
    {
        landmark.x = landmark.x * 40.0F + 480.0F;
        landmark.y = landmark.y * 40.0F + 120.0F;
        landmark.z *= 40.0F;
    }

    MultiHandMeasurementStage stage;
    const auto first = stage.extract(
        observations, 1, 1'000'000, 640, 480);
    require(first.handCount == 2 && first.relation.valid,
        "Multi-hand measurement stage did not produce a two-hand relation.");
    require(first.hands[0].trackId == 41 && first.hands[1].trackId == 84,
        "Multi-hand measurement stage did not preserve track IDs.");
    require(first.hands[0].topologySummary.sampleCount == 1
            && first.hands[1].topologySummary.sampleCount == 1,
        "Multi-hand measurement stage did not start independent topology histories.");
    requireNear(first.relation.deltaX, 320.0F / 37.6F, 0.02F,
        "Two-hand palm-axis-normalized separation is incorrect.");

    std::swap(observations.hands[0], observations.hands[1]);
    std::swap(observations.trackIds[0], observations.trackIds[1]);
    const auto second = stage.extract(
        observations, 2, 1'033'333, 640, 480);
    require(second.hands[0].trackId == 84 && second.hands[1].trackId == 41,
        "Multi-hand measurement stage reordered track identities.");
    require(second.hands[0].topologySummary.sampleCount == 2
            && second.hands[1].topologySummary.sampleCount == 2,
        "Track-ID reordering reset or mixed topology histories.");
    require(second.hands[0].hand.linearSpeed < 0.001F
            && second.hands[1].hand.linearSpeed < 0.001F,
        "Per-hand temporal measurement history leaked between track IDs.");

    observations.handCount = 1;
    static_cast<void>(stage.extract(
        observations, 3, 1'066'666, 640, 480));
    observations.handCount = 2;
    const auto afterMissing = stage.extract(
        observations, 4, 1'099'999, 640, 480);
    require(afterMissing.hands[0].topologySummary.sampleCount == 4
            && afterMissing.hands[1].topologySummary.sampleCount == 3,
        "A one-frame hand gap did not preserve both per-track topology segments.");
}

void testRyoikiHandTopologyAbiStructSizesAreBlittable()
{
    // These mirror the static_asserts in ryoiki_native.h/.cpp. Duplicating
    // them as a runtime-visible test keeps the C ABI's blittable contract
    // with NativeVisionInterop.cs (managed [StructLayout(Sequential)]
    // mirrors) from silently drifting when a field is added or reordered.
    static_assert(sizeof(RyoikiHandTopologySummary) == 136,
        "RyoikiHandTopologySummary size no longer matches the documented ABI.");
    static_assert(sizeof(RyoikiTwoHandRelation) == 40,
        "RyoikiTwoHandRelation size no longer matches the documented ABI.");
    static_assert(sizeof(RyoikiHandTopologySnapshot) == 344,
        "RyoikiHandTopologySnapshot size no longer matches the documented ABI.");
    static_assert(
        sizeof(RyoikiHandTopologySnapshot)
            == 2 * sizeof(std::uint32_t)
                + 2 * sizeof(std::uint64_t)
                + 2 * sizeof(std::uint32_t)
                + kRyoikiMaxHands * sizeof(RyoikiHandTopologySummary)
                + sizeof(RyoikiTwoHandRelation),
        "RyoikiHandTopologySnapshot layout no longer matches its documented header/body shape.");

    require(kRyoikiAbiVersion >= 21,
        "ABI version was not advanced for the hand topology snapshot export.");

    RyoikiHandTopologySnapshot snapshot{};
    snapshot.abi_version = kRyoikiAbiVersion;
    snapshot.struct_size = static_cast<std::uint32_t>(sizeof(RyoikiHandTopologySnapshot));
    require(snapshot.abi_version == kRyoikiAbiVersion
            && snapshot.struct_size == sizeof(RyoikiHandTopologySnapshot),
        "RyoikiHandTopologySnapshot did not round-trip its own ABI/struct-size fields.");
}

void testWeightedPalmRotationTracker()
{
    using ryoiki::hand_input::measurements::WeightedPalmRotationTracker;
    auto reference = createOpenHandStateTestInput();
    WeightedPalmRotationTracker tracker;
    require(tracker.captureReference(reference),
        "Weighted palm tracker rejected its reference.");

    auto transformed = reference;
    constexpr float angle = 0.55F;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    for (std::size_t index = 0; index < transformed.worldLandmarks.size(); ++index)
    {
        const auto point = reference.worldLandmarks[index];
        transformed.worldLandmarks[index] = {
            (cosine * point.x - sine * point.y) * 2.4F + 3.0F,
            (sine * point.x + cosine * point.y) * 2.4F - 1.0F,
            point.z * 2.4F + 0.7F};
    }
    const auto estimate = tracker.estimate(transformed);
    require(estimate.valid, "Weighted palm tracker failed to estimate rotation.");
    requireNear(estimate.rotation[0], cosine, 2.0e-3F,
        "Weighted palm rotation recovered an incorrect R00.");
    requireNear(estimate.rotation[1], sine, 2.0e-3F,
        "Weighted palm rotation recovered an incorrect R10.");
    requireNear(estimate.rotation[3], -sine, 2.0e-3F,
        "Weighted palm rotation recovered an incorrect R01.");
    requireNear(estimate.rotation[4], cosine, 2.0e-3F,
        "Weighted palm rotation recovered an incorrect R11.");
    require(estimate.fitError < 2.0e-3F,
        "Weighted palm rotation retained scale or translation error.");

    transformed.worldLandmarks[13].x += 0.08F;
    const auto noisy = tracker.estimate(transformed);
    require(noisy.valid && noisy.fitError > estimate.fitError,
        "Weighted palm fit error did not expose a landmark outlier.");
    requireNear(noisy.rotation[0], cosine, 2.5e-2F,
        "Robust palm rotation was pulled too far by one landmark outlier.");
}

void testWeightedPalmRotationTrackerMixedRotationAndDegeneracy()
{
    using ryoiki::hand_input::measurements::WeightedPalmRotationTracker;
    auto reference = createOpenHandStateTestInput();
    WeightedPalmRotationTracker tracker;
    require(tracker.captureReference(reference), "Palm tracker rejected mixed-rotation reference.");

    constexpr float xAngle = 0.42F;
    constexpr float yAngle = -0.63F;
    const float cx = std::cos(xAngle);
    const float sx = std::sin(xAngle);
    const float cy = std::cos(yAngle);
    const float sy = std::sin(yAngle);
    const std::array<float, 9> rotation{
        cy, 0.0F, -sy,
        sy * sx, cx, cy * sx,
        sy * cx, -sx, cy * cx};
    auto transformed = reference;
    for (std::size_t index = 0; index < transformed.worldLandmarks.size(); ++index)
    {
        const auto point = reference.worldLandmarks[index];
        transformed.worldLandmarks[index] = {
            rotation[0] * point.x + rotation[3] * point.y + rotation[6] * point.z + 0.7F,
            rotation[1] * point.x + rotation[4] * point.y + rotation[7] * point.z - 1.2F,
            rotation[2] * point.x + rotation[5] * point.y + rotation[8] * point.z + 0.3F};
    }
    const auto estimate = tracker.estimate(transformed);
    require(estimate.valid, "Palm tracker rejected a mixed X/Y rotation.");
    for (std::size_t index = 0; index < rotation.size(); ++index)
    {
        requireNear(estimate.rotation[index], rotation[index], 3.0e-3F,
            "Palm tracker recovered an incorrect mixed rotation.");
    }

    auto collinear = reference;
    constexpr std::array<std::size_t, 6> selected{0, 5, 9, 13, 17, 1};
    for (std::size_t index = 0; index < selected.size(); ++index)
    {
        collinear.worldLandmarks[selected[index]] = {
            static_cast<float>(index), 0.0F, 0.0F};
    }
    require(tracker.captureReference(collinear),
        "Normalization unexpectedly rejected finite collinear points.");
    require(!tracker.estimate(collinear).valid,
        "Palm tracker accepted rotationally unobservable collinear geometry.");
}

void testPalmRotationEskfSuppressesJitterAndTracksMotion()
{
    using ryoiki::hand_input::measurements::PalmRotationEskf;
    using ryoiki::hand_input::measurements::PalmRotationEstimate;
    const auto yawRotation = [](const float angle)
    {
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        return std::array<float, 9>{
            cosine, 0.0F, -sine,
            0.0F, 1.0F, 0.0F,
            sine, 0.0F, cosine};
    };
    const auto observation = [&yawRotation](const float angle, const float fitError)
    {
        PalmRotationEstimate value{};
        value.rotation = yawRotation(angle);
        value.fitError = fitError;
        value.valid = true;
        return value;
    };
    const auto yaw = [](const PalmRotationEstimate& value)
    {
        return std::atan2(value.rotation[6], value.rotation[0]);
    };

    PalmRotationEskf stationary;
    std::uint64_t timestamp = 1'000'000;
    auto previousAngle = 0.0F;
    static_cast<void>(stationary.update(
        observation(0.0F, 0.01F), observation(0.0F, 0.01F), timestamp));
    float rawMagnitude = 0.0F;
    float filteredMagnitude = 0.0F;
    for (std::size_t frame = 1; frame <= 40; ++frame)
    {
        timestamp += 33'333;
        const float angle = frame % 2 == 0 ? 0.035F : -0.035F;
        const auto filtered = stationary.update(
            observation(angle, 0.08F),
            observation(angle - previousAngle, 0.08F),
            timestamp);
        require(filtered.valid, "Palm ESKF rejected a finite jitter observation.");
        rawMagnitude += std::abs(angle);
        filteredMagnitude += std::abs(yaw(filtered));
        previousAngle = angle;
    }
    require(filteredMagnitude < rawMagnitude * 0.65F,
        "Palm ESKF did not materially suppress stationary angular jitter.");

    PalmRotationEskf moving;
    timestamp = 2'000'000;
    static_cast<void>(moving.update(
        observation(0.0F, 0.01F), observation(0.0F, 0.01F), timestamp));
    PalmRotationEstimate filtered{};
    constexpr float step = 0.012F;
    for (std::size_t frame = 1; frame <= 60; ++frame)
    {
        timestamp += 33'333;
        filtered = moving.update(
            observation(step * static_cast<float>(frame), 0.015F),
            observation(step, 0.015F),
            timestamp);
    }
    require(filtered.valid, "Palm ESKF rejected smooth rotational motion.");
    requireNear(yaw(filtered), 60.0F * step, 0.08F,
        "Palm ESKF introduced excessive lag during smooth motion.");
}

void testPalmBasisRotationMatchesRigidLandmarkRotation()
{
    using ryoiki::hand_input::measurements::HandMeasurementExtractor;
    using ryoiki::hand_input::measurements::PalmBasisRotationTracker;
    auto reference = createOpenHandStateTestInput();
    HandMeasurementExtractor extractor;
    const auto referenceMeasurements =
        extractor.extract(reference, 1, 1'000'000, 640, 480).hand;
    PalmBasisRotationTracker tracker;
    require(tracker.captureReference(referenceMeasurements),
        "Palm basis tracker rejected a valid reference basis.");

    constexpr float xAngle = 0.31F;
    constexpr float yAngle = -0.47F;
    const float cx = std::cos(xAngle);
    const float sx = std::sin(xAngle);
    const float cy = std::cos(yAngle);
    const float sy = std::sin(yAngle);
    const std::array<float, 9> rotation{
        cy, 0.0F, -sy,
        sy * sx, cx, cy * sx,
        sy * cx, -sx, cy * cx};
    auto transformed = reference;
    for (std::size_t index = 0; index < transformed.worldLandmarks.size(); ++index)
    {
        const auto point = reference.worldLandmarks[index];
        transformed.worldLandmarks[index] = {
            rotation[0] * point.x + rotation[3] * point.y + rotation[6] * point.z + 0.4F,
            rotation[1] * point.x + rotation[4] * point.y + rotation[7] * point.z - 0.8F,
            rotation[2] * point.x + rotation[5] * point.y + rotation[8] * point.z + 0.2F};
    }
    const auto currentMeasurements =
        extractor.extract(transformed, 2, 1'033'333, 640, 480).hand;
    const auto estimate = tracker.estimate(currentMeasurements);
    require(estimate.valid, "Palm basis tracker rejected a rigidly rotated palm.");
    for (std::size_t index = 0; index < rotation.size(); ++index)
    {
        requireNear(estimate.rotation[index], rotation[index], 3.0e-3F,
            "Palm basis tracker recovered an incorrect rigid rotation.");
    }
}

void testPalmRotationOneEuroSuppressesJitterWithLowLag()
{
    using ryoiki::hand_input::measurements::PalmRotationEstimate;
    using ryoiki::hand_input::measurements::PalmRotationOneEuroFilter;
    const auto observation = [](const float angle)
    {
        PalmRotationEstimate value{};
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        value.rotation = {
            cosine, 0.0F, -sine,
            0.0F, 1.0F, 0.0F,
            sine, 0.0F, cosine};
        value.valid = true;
        return value;
    };
    const auto yaw = [](const PalmRotationEstimate& value)
    {
        return std::atan2(value.rotation[6], value.rotation[0]);
    };

    PalmRotationOneEuroFilter stationary;
    std::uint64_t timestamp = 1'000'000;
    static_cast<void>(stationary.update(observation(0.0F), timestamp));
    float rawMagnitude = 0.0F;
    float filteredMagnitude = 0.0F;
    for (std::size_t frame = 1; frame <= 40; ++frame)
    {
        timestamp += 33'333;
        const float angle = frame % 2 == 0 ? 0.035F : -0.035F;
        const auto filtered = stationary.update(observation(angle), timestamp);
        rawMagnitude += std::abs(angle);
        filteredMagnitude += std::abs(yaw(filtered));
    }
    // Tightened when the cutoff pair was derived from recorded speeds. The
    // previous 0.65 bound also passed with a filter whose speed term never
    // engaged, so it could not detect that regression.
    require(filteredMagnitude < rawMagnitude * 0.45F,
        "Palm One Euro filter did not suppress stationary angular jitter.");

    PalmRotationOneEuroFilter moving;
    timestamp = 2'000'000;
    static_cast<void>(moving.update(observation(0.0F), timestamp));
    PalmRotationEstimate filtered{};
    constexpr float step = 0.012F;
    for (std::size_t frame = 1; frame <= 60; ++frame)
    {
        timestamp += 33'333;
        filtered = moving.update(
            observation(step * static_cast<float>(frame)), timestamp);
    }
    requireNear(yaw(filtered), 60.0F * step, 0.035F,
        "Palm One Euro filter introduced excessive lag during smooth motion.");
}

void testRotationObservationGateHoldsAndReacquiresContinuously()
{
    using ryoiki::hand_input::measurements::PalmRotationEstimate;
    using ryoiki::hand_input::measurements::RotationObservationGate;
    using ryoiki::hand_input::measurements::RotationObservationGateState;
    const auto observation = [](const float angleDegrees)
    {
        const float angle =
            angleDegrees * 3.14159265358979323846F / 180.0F;
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        PalmRotationEstimate result{};
        result.rotation = {
            cosine, 0.0F, -sine,
            0.0F, 1.0F, 0.0F,
            sine, 0.0F, cosine};
        result.valid = true;
        return result;
    };
    const auto yawDegrees = [](const PalmRotationEstimate& value)
    {
        return std::atan2(value.rotation[6], value.rotation[0])
            * 180.0F / 3.14159265358979323846F;
    };

    RotationObservationGate gate;
    auto output = gate.update(observation(0.0F));
    require(output.observation.valid
            && output.state == RotationObservationGateState::Tracking,
        "Rotation gate did not initialize into Tracking.");
    output = gate.update(observation(60.0F));
    require(output.observation.valid
            && output.state == RotationObservationGateState::Tracking,
        "Rotation gate rejected a valid 60-degree consecutive delta.");
    requireNear(yawDegrees(output.observation), 60.0F, 1.0e-3F,
        "Rotation gate changed a valid 60-degree observation.");

    output = gate.update(observation(-60.0F));
    require(!output.observation.valid
            && output.rejection
                == ryoiki::hand_input::measurements::
                    RotationObservationRejection::AngularJump,
        "Rotation gate published a greater-than-90-degree angular jump.");
    require(output.state == RotationObservationGateState::Tracking
            && output.outlierFrames == 1,
        "Rotation gate abandoned its trajectory on the first outlier.");

    // The implausible branch persists, so the trajectory really moved and the
    // gate falls back to continuity-preserving recovery.
    for (std::size_t frame = 2; frame <= 5; ++frame)
    {
        output = gate.update(observation(-60.0F));
        require(output.state == RotationObservationGateState::Tracking
                && output.outlierFrames == frame,
            "Rotation gate left Tracking inside the outlier budget.");
    }
    output = gate.update(observation(-60.0F));
    require(!output.observation.valid
            && output.state == RotationObservationGateState::Holding,
        "Rotation gate did not abandon a persistent implausible branch.");

    output = gate.update(observation(-60.0F));
    require(!output.observation.valid
            && output.state == RotationObservationGateState::Reacquiring,
        "Rotation gate did not begin a recovery candidate.");
    output = gate.update(observation(-57.0F));
    require(!output.observation.valid && output.stableCandidateFrames == 2,
        "Rotation gate accepted a recovery candidate too early.");
    output = gate.update(observation(-54.0F));
    require(output.observation.valid && output.reacquired
            && output.state == RotationObservationGateState::Tracking,
        "Rotation gate did not accept a stable recovery sequence.");
    requireNear(yawDegrees(output.observation), 66.0F, 1.0e-3F,
        "Rotation gate did not rebase recovery onto the held output.");

    output = gate.update(observation(-51.0F));
    requireNear(yawDegrees(output.observation), 69.0F, 1.0e-3F,
        "Rotation gate failed to preserve motion after reacquisition.");

    // A missing inference breaks continuity before the next finite trajectory.
    PalmRotationEstimate missing{};
    output = gate.update(missing);
    require(!output.observation.valid
            && output.state == RotationObservationGateState::Holding,
        "Rotation gate did not hold a missing inference.");
    output = gate.update(observation(0.0F));
    require(!output.observation.valid
            && output.state == RotationObservationGateState::Reacquiring,
        "Rotation gate did not begin inference-gap reacquisition.");
    output = gate.update(observation(2.0F));
    require(!output.observation.valid && output.stableCandidateFrames == 2,
        "Rotation gate accepted an inference-gap candidate too early.");
    output = gate.update(observation(4.0F));
    require(output.observation.valid && output.reacquired,
        "Rotation gate did not complete inference-gap reacquisition.");
    requireNear(yawDegrees(output.observation), 73.0F, 1.0e-3F,
        "Rotation gate introduced a discontinuity across an inference gap.");

    // Explicit track-id rebinding follows the same path and does not depend on
    // camera timestamps.
    gate.beginReacquisition();
    output = gate.update(observation(0.0F));
    require(!output.observation.valid
            && output.state == RotationObservationGateState::Reacquiring,
        "Rotation gate did not begin an explicit track handoff.");
    output = gate.update(observation(2.0F));
    output = gate.update(observation(4.0F));
    require(output.observation.valid && output.reacquired,
        "Rotation gate did not complete an explicit track handoff.");
    requireNear(yawDegrees(output.observation), 77.0F, 1.0e-3F,
        "Rotation gate introduced a discontinuity during track handoff.");
}

void testRotationObservationGateAbsorbsTransientFlips()
{
    using ryoiki::hand_input::measurements::PalmRotationEstimate;
    using ryoiki::hand_input::measurements::RotationObservationGate;
    using ryoiki::hand_input::measurements::RotationObservationGateState;
    const auto observation = [](const float angleDegrees)
    {
        const float angle =
            angleDegrees * 3.14159265358979323846F / 180.0F;
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        PalmRotationEstimate result{};
        result.rotation = {
            cosine, 0.0F, -sine,
            0.0F, 1.0F, 0.0F,
            sine, 0.0F, cosine};
        result.valid = true;
        return result;
    };
    const auto yawDegrees = [](const PalmRotationEstimate& value)
    {
        return std::atan2(value.rotation[6], value.rotation[0])
            * 180.0F / 3.14159265358979323846F;
    };

    // Recorded perception failures alternate between the tracked branch and a
    // branch roughly 180 degrees away for one or two frames at a time. The gate
    // must absorb those without rebasing, otherwise every flip injects a
    // permanent offset between the hand and the manipulated model.
    RotationObservationGate gate;
    auto output = gate.update(observation(0.0F));
    output = gate.update(observation(10.0F));
    output = gate.update(observation(-170.0F));
    require(!output.observation.valid
            && output.state == RotationObservationGateState::Tracking,
        "Rotation gate abandoned its trajectory on a one-frame flip.");
    output = gate.update(observation(14.0F));
    require(output.observation.valid && !output.reacquired
            && output.state == RotationObservationGateState::Tracking,
        "Rotation gate did not resume directly after a one-frame flip.");
    requireNear(yawDegrees(output.observation), 14.0F, 1.0e-3F,
        "Rotation gate altered the observation after absorbing a flip.");
    requireNear(output.rebaseOffsetDegrees, 0.0F, 1.0e-3F,
        "Rotation gate injected drift while absorbing a one-frame flip.");

    // Motion during the flip is preserved because the trajectory anchor was
    // never abandoned.
    output = gate.update(observation(-172.0F));
    output = gate.update(observation(-174.0F));
    output = gate.update(observation(20.0F));
    require(output.observation.valid && !output.reacquired,
        "Rotation gate did not resume after a two-frame flip.");
    requireNear(yawDegrees(output.observation), 20.0F, 1.0e-3F,
        "Rotation gate lost real motion across an absorbed flip.");
    requireNear(output.rebaseOffsetDegrees, 0.0F, 1.0e-3F,
        "Rotation gate injected drift across a two-frame flip.");
}

void testRotationObservationGateReportsRebaseDrift()
{
    using ryoiki::hand_input::measurements::PalmRotationEstimate;
    using ryoiki::hand_input::measurements::RotationObservationGate;
    const auto observation = [](const float angleDegrees)
    {
        const float angle =
            angleDegrees * 3.14159265358979323846F / 180.0F;
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        PalmRotationEstimate result{};
        result.rotation = {
            cosine, 0.0F, -sine,
            0.0F, 1.0F, 0.0F,
            sine, 0.0F, cosine};
        result.valid = true;
        return result;
    };

    RotationObservationGate gate;
    auto output = gate.update(observation(0.0F));
    requireNear(output.rebaseOffsetDegrees, 0.0F, 1.0e-3F,
        "Rotation gate reported drift before any recovery.");
    output = gate.update(observation(60.0F));
    requireNear(output.rebaseOffsetDegrees, 0.0F, 1.0e-3F,
        "Rotation gate reported drift while continuously tracking.");
    requireNear(output.rebaseStepDegrees, 0.0F, 1.0e-3F,
        "Rotation gate reported a rebase step without a recovery.");

    // A persistent flip past the 90-degree rule. The outlier budget is spent
    // first, then recovery anchors the output to the first candidate (-60 raw
    // held against 60 published), so it permanently injects 120 degrees of
    // divergence. Motion during the candidate frames is preserved, which is why
    // the published yaw reaches 66.
    for (std::size_t frame = 0; frame <= 5; ++frame)
    {
        output = gate.update(observation(-60.0F));
        requireNear(output.rebaseOffsetDegrees, 0.0F, 1.0e-3F,
            "Rotation gate injected drift while absorbing an outlier.");
    }
    output = gate.update(observation(-60.0F));
    output = gate.update(observation(-57.0F));
    output = gate.update(observation(-54.0F));
    require(output.reacquired, "Rotation gate did not complete the recovery.");
    requireNear(output.rebaseStepDegrees, 120.0F, 1.0e-2F,
        "Rotation gate did not report the offset injected by one recovery.");
    requireNear(output.rebaseOffsetDegrees, 120.0F, 1.0e-2F,
        "Rotation gate did not report accumulated drift after one recovery.");

    output = gate.update(observation(-51.0F));
    requireNear(output.rebaseOffsetDegrees, 120.0F, 1.0e-2F,
        "Rotation gate changed accumulated drift while tracking.");
    requireNear(output.rebaseStepDegrees, 0.0F, 1.0e-3F,
        "Rotation gate repeated a rebase step on a tracking frame.");

    // A second recovery accumulates onto the first. Tracking loss abandons the
    // trajectory immediately instead of consuming the outlier budget.
    PalmRotationEstimate missing{};
    output = gate.update(missing);
    requireNear(output.rebaseOffsetDegrees, 120.0F, 1.0e-2F,
        "Rotation gate lost accumulated drift while holding.");
    output = gate.update(observation(-81.0F));
    output = gate.update(observation(-79.0F));
    output = gate.update(observation(-77.0F));
    require(output.reacquired,
        "Rotation gate did not complete the second recovery.");
    requireNear(output.rebaseStepDegrees, 30.0F, 1.0e-2F,
        "Rotation gate did not report the second injected offset.");
    requireNear(output.rebaseOffsetDegrees, 150.0F, 1.0e-2F,
        "Rotation gate did not accumulate drift across two recoveries.");
}

void testNativeCadHandBindingRezeroesAfterTrackingGap()
{
    using ryoiki::features::cad::CadHandBinding;
    using ryoiki::features::cad::CadHandInput;
    using ryoiki::features::cad::HandInteractionMode;
    using ryoiki::features::cad::HandInteractionState;
    using ryoiki::presentation::HandPresentationMode;
    using ryoiki::rendering::CadView;
    const auto yawRotation = [](const float angle)
    {
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        return std::array<float, 9>{
            cosine, 0.0F, -sine,
            0.0F, 1.0F, 0.0F,
            sine, 0.0F, cosine};
    };
    auto start = std::chrono::steady_clock::time_point{
        std::chrono::milliseconds{1000}};
    CadHandInput input{};
    input.frameId = 1;
    input.captureTimestampUs = 1'000'000;
    input.trackingQuality = 0.95F;
    input.screenCenterX = 0.5F;
    input.screenCenterY = 0.5F;
    input.screenScale = 0.1F;
    input.relativeRotation = yawRotation(0.0F);
    input.handPresent = true;
    input.screenPalmValid = true;
    input.relativeRotationValid = true;

    CadHandBinding binding;
    const CadView initial{};
    auto output = binding.update(
        input, initial, HandInteractionMode::Rotate,
        HandPresentationMode::MirrorDirect, 1.0F, start);
    require(output.referenceCaptureRequested,
        "Native CAD binding did not request a reference at clutch-down.");

    input.frameId = 2;
    input.relativeRotation = yawRotation(0.30F);
    output = binding.update(
        input, initial, HandInteractionMode::Rotate,
        HandPresentationMode::MirrorDirect, 1.0F,
        start + std::chrono::milliseconds{33});
    require(output.viewChanged && !output.referenceCaptureRequested,
        "Native CAD binding re-requested a reference while tracking.");
    const auto heldView = output.view;

    // Tracking is lost well beyond the grace period while the clutch is held.
    input.handPresent = false;
    input.relativeRotationValid = false;
    input.screenPalmValid = false;
    for (std::size_t frame = 0; frame < 30; ++frame)
    {
        input.frameId = 3 + frame;
        output = binding.update(
            input, heldView, HandInteractionMode::Rotate,
            HandPresentationMode::MirrorDirect, 1.0F,
            start + std::chrono::milliseconds{
                66 + static_cast<int>(frame) * 33});
        require(output.state != HandInteractionState::AwaitingRelease,
            "Native CAD binding still demands a clutch release after a gap.");
        require(!output.viewChanged,
            "Native CAD binding moved the view while tracking was lost.");
    }

    // The hand returns at a completely different pose. The session must re-zero
    // rather than snap the view by the pre-gap relative rotation.
    input.handPresent = true;
    input.relativeRotationValid = true;
    input.screenPalmValid = true;
    input.screenCenterX = 0.2F;
    input.frameId = 100;
    input.relativeRotation = yawRotation(2.0F);
    output = binding.update(
        input, heldView, HandInteractionMode::Rotate,
        HandPresentationMode::MirrorDirect, 1.0F,
        start + std::chrono::milliseconds{1200});
    require(output.state == HandInteractionState::Rotating,
        "Native CAD binding did not resume while the clutch was held.");
    require(!output.viewChanged,
        "Native CAD binding moved the view on the re-zero frame.");
    require(output.referenceCaptureRequested,
        "Native CAD binding resumed without a fresh rotation reference.");
    requireNear(output.view.yawRadians, heldView.yawRadians, 1.0e-4F,
        "Native CAD binding did not preserve the view across a gap.");
}

void testNativeCadHandBindingSensitivityAndPresentation()
{
    using ryoiki::features::cad::CadHandBinding;
    using ryoiki::features::cad::CadHandInput;
    using ryoiki::features::cad::HandInteractionMode;
    using ryoiki::presentation::HandPresentationMode;
    using ryoiki::rendering::CadView;
    const auto start = std::chrono::steady_clock::time_point{
        std::chrono::milliseconds{1000}};
    CadHandInput input{};
    input.frameId = 1;
    input.captureTimestampUs = 1'000'000;
    input.trackingQuality = 0.95F;
    input.screenCenterX = 0.5F;
    input.screenCenterY = 0.5F;
    input.screenScale = 0.1F;
    input.relativeRotation = {
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    input.handPresent = true;
    input.screenPalmValid = true;
    input.relativeRotationValid = true;

    CadHandBinding normal;
    CadHandBinding fast;
    const CadView initial{};
    static_cast<void>(normal.update(
        input, initial, HandInteractionMode::Rotate,
        HandPresentationMode::MirrorDirect, 1.0F, start));
    static_cast<void>(fast.update(
        input, initial, HandInteractionMode::Rotate,
        HandPresentationMode::MirrorDirect, 2.0F, start));
    constexpr float angle = 0.25F;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    input.relativeRotation = {
        cosine, 0.0F, -sine,
        0.0F, 1.0F, 0.0F,
        sine, 0.0F, cosine};
    input.frameId = 2;
    input.captureTimestampUs += 33'333;
    const auto normalOutput = normal.update(
        input, initial, HandInteractionMode::Rotate,
        HandPresentationMode::MirrorDirect, 1.0F,
        start + std::chrono::milliseconds{33});
    const auto fastOutput = fast.update(
        input, initial, HandInteractionMode::Rotate,
        HandPresentationMode::MirrorDirect, 2.0F,
        start + std::chrono::milliseconds{33});
    require(normalOutput.viewChanged && fastOutput.viewChanged,
        "Native CAD binding did not publish a rotation view.");
    requireNear(
        fastOutput.yawDeltaDegrees,
        normalOutput.yawDeltaDegrees * 2.0F,
        1.0e-3F,
        "Native CAD sensitivity did not scale rotation.");

    CadHandBinding pitchBinding;
    input.relativeRotation = {
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    input.frameId = 3;
    input.captureTimestampUs += 33'333;
    static_cast<void>(pitchBinding.update(
        input, initial, HandInteractionMode::Rotate,
        HandPresentationMode::Physical, 1.0F,
        start + std::chrono::milliseconds{66}));
    input.relativeRotation = {
        1.0F, 0.0F, 0.0F,
        0.0F, cosine, sine,
        0.0F, -sine, cosine};
    input.frameId = 4;
    input.captureTimestampUs += 33'333;
    const auto pitchOutput = pitchBinding.update(
        input, initial, HandInteractionMode::Rotate,
        HandPresentationMode::Physical, 1.0F,
        start + std::chrono::milliseconds{99});
    requireNear(
        std::abs(pitchOutput.pitchDeltaDegrees),
        angle * 1.60F * 180.0F / 3.14159265358979323846F,
        1.0e-3F,
        "Native CAD binding did not apply the independent pitch-axis gain.");
    requireNear(pitchOutput.yawDeltaDegrees, 0.0F, 1.0e-3F,
        "Pitch-axis gain leaked into CAD yaw.");

    CadHandBinding mirrorPan;
    CadHandBinding physicalPan;
    input.relativeRotation = {
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    input.frameId = 10;
    static_cast<void>(mirrorPan.update(
        input, initial, HandInteractionMode::Pan,
        HandPresentationMode::MirrorDirect, 1.0F, start));
    static_cast<void>(physicalPan.update(
        input, initial, HandInteractionMode::Pan,
        HandPresentationMode::Physical, 1.0F, start));
    input.frameId = 11;
    input.screenCenterX += 0.1F;
    const auto mirrorOutput = mirrorPan.update(
        input, initial, HandInteractionMode::Pan,
        HandPresentationMode::MirrorDirect, 1.0F,
        start + std::chrono::milliseconds{33});
    const auto physicalOutput = physicalPan.update(
        input, initial, HandInteractionMode::Pan,
        HandPresentationMode::Physical, 1.0F,
        start + std::chrono::milliseconds{33});
    requireNear(mirrorOutput.view.panX, -physicalOutput.view.panX, 1.0e-6F,
        "Mirror and physical pan did not produce opposite horizontal directions.");
}

void testDomainExpansionGestureGeometry()
{
    ryoiki::hand_perception::HandLandmarkResult sign{};
    sign.detected = true;
    auto& points = sign.landmarks;
    points[0] = {0.0F, 0.0F, 0.0F};
    points[1] = {-0.32F, 0.35F, 0.0F}; points[2] = {-0.28F, 0.58F, 0.0F};
    points[3] = {-0.12F, 0.72F, 0.0F}; points[4] = {0.12F, 0.82F, 0.0F};
    points[5] = {-0.35F, 0.85F, 0.0F}; points[6] = {-0.35F, 1.30F, 0.0F};
    points[7] = {-0.35F, 1.68F, 0.0F}; points[8] = {-0.35F, 2.02F, 0.0F};
    points[9] = {0.0F, 1.0F, 0.0F}; points[10] = {-0.02F, 1.42F, -0.05F};
    points[11] = {-0.28F, 1.58F, -0.08F}; points[12] = {-0.55F, 1.44F, -0.10F};
    points[13] = {0.30F, 0.88F, 0.0F}; points[14] = {0.38F, 1.16F, 0.0F};
    points[15] = {0.28F, 1.02F, 0.0F}; points[16] = {0.14F, 0.86F, 0.0F};
    points[17] = {0.52F, 0.72F, 0.0F}; points[18] = {0.62F, 0.96F, 0.0F};
    points[19] = {0.48F, 0.84F, 0.0F}; points[20] = {0.34F, 0.68F, 0.0F};

    const auto detected =
        ryoiki::hand_input::recognition::recognizeDomainExpansionState(sign);
    require(detected.detected, "Crossed index/middle domain sign was not detected.");
    require(detected.features.middleWrap >= 0.52F,
        "Domain sign did not score the middle-finger wrap strongly enough.");

    auto openPalm = sign;
    openPalm.landmarks[10] = {0.0F, 1.38F, 0.0F};
    openPalm.landmarks[11] = {0.0F, 1.72F, 0.0F};
    openPalm.landmarks[12] = {0.0F, 2.02F, 0.0F};
    openPalm.landmarks[14] = {0.30F, 1.28F, 0.0F};
    openPalm.landmarks[15] = {0.30F, 1.62F, 0.0F};
    openPalm.landmarks[16] = {0.30F, 1.92F, 0.0F};
    openPalm.landmarks[18] = {0.52F, 1.12F, 0.0F};
    openPalm.landmarks[19] = {0.52F, 1.45F, 0.0F};
    openPalm.landmarks[20] = {0.52F, 1.72F, 0.0F};
    const auto rejected =
        ryoiki::hand_input::recognition::recognizeDomainExpansionState(openPalm);
    require(!rejected.detected, "Open palm was incorrectly detected as the domain sign.");
    require(rejected.features.middleWrap < detected.features.middleWrap,
        "Straight middle finger did not reduce the wrap score.");
}

void testOpenPalmStateRecognition()
{
    using namespace ryoiki::hand_input;
    measurements::HandMeasurements open{};
    open.present = true;
    open.quality = measurements::HandMeasurementQuality::Valid;
    open.trackingQuality = 0.92F;
    open.extension = {0.70F, 0.91F, 0.94F, 0.92F, 0.89F};
    open.thumbIndexDistance = 0.48F;

    const auto accepted = recognition::recognizeOpenPalmState(open);
    require(accepted.inputValid && accepted.detected,
        "Open Palm recognizer rejected an extended hand.");
    require(accepted.confidence > 0.70F,
        "Open Palm confidence was unexpectedly weak.");

    auto pointing = open;
    pointing.extension = {0.75F, 0.94F, 0.20F, 0.18F, 0.15F};
    require(!recognition::recognizeOpenPalmState(pointing).detected,
        "Open Palm recognizer accepted a pointing hand.");

    auto closed = open;
    closed.extension = {0.15F, 0.20F, 0.18F, 0.22F, 0.17F};
    closed.thumbIndexDistance = 0.10F;
    require(!recognition::recognizeOpenPalmState(closed).detected,
        "Open Palm recognizer accepted a closed hand.");

    auto missing = open;
    missing.quality = measurements::HandMeasurementQuality::Missing;
    require(!recognition::recognizeOpenPalmState(missing).inputValid,
        "Open Palm recognizer treated missing measurements as a negative sample.");

    recognition::OpenPalmStateRecognizer recognizer;
    auto state = recognizer.process(
        accepted, open.trackingQuality, 1, 1'000'000);
    require(state.phase == recognition::HandStatePhase::Candidate,
        "Open Palm did not enter Candidate.");
    state = recognizer.process(
        accepted, open.trackingQuality, 2, 1'120'000);
    require(state.phase == recognition::HandStatePhase::Active
            && state.transition == recognition::HandStateTransition::Began,
        "Open Palm did not become Active after its enter duration.");
}

ryoiki::hand_input::measurements::HandMeasurementFrame swipeFrame(
    const std::uint64_t frameId,
    const std::uint64_t timestampUs,
    const float x,
    const float y,
    const float scale = 0.10F)
{
    using namespace ryoiki::hand_input::measurements;
    HandMeasurementFrame frame{};
    frame.hand.frameId = frameId;
    frame.hand.timestampUs = timestampUs;
    frame.hand.present = true;
    frame.hand.quality = HandMeasurementQuality::Valid;
    frame.hand.trackingQuality = 0.92F;
    frame.screenPalm = {
        .frameId = frameId,
        .timestampUs = timestampUs,
        .centerX = x,
        .centerY = y,
        .scale = scale,
        .valid = true};
    return frame;
}

void testSwipeEventRecognition()
{
    using namespace ryoiki::hand_input::recognition;
    SwipeEventRecognizer recognizer;
    require(!recognizer.process(
        swipeFrame(1, 1'000'000, 0.70F, 0.50F), true, 0.9F).has_value(),
        "Swipe recognizer emitted from its anchor.");
    require(!recognizer.process(
        swipeFrame(2, 1'080'000, 0.65F, 0.505F), true, 0.9F).has_value(),
        "Swipe recognizer emitted before minimum duration/displacement.");
    const auto left = recognizer.process(
        swipeFrame(3, 1'180'000, 0.50F, 0.51F), true, 0.88F);
    require(left.has_value() && left->id == kSwipeLeftEventId,
        "A valid left swipe was not recognized.");
    require(left->displacementX < -0.19F && left->durationUs == 180'000,
        "Swipe Event did not preserve trajectory diagnostics.");
    require(!recognizer.process(
        swipeFrame(4, 1'220'000, 0.40F, 0.51F), true, 0.9F).has_value(),
        "Swipe cooldown emitted a duplicate event.");

    recognizer.reset();
    static_cast<void>(recognizer.process(
        swipeFrame(10, 2'000'000, 0.30F, 0.30F), true, 0.9F));
    static_cast<void>(recognizer.process(
        swipeFrame(11, 2'100'000, 0.38F, 0.40F), true, 0.9F));
    require(!recognizer.process(
        swipeFrame(12, 2'220'000, 0.50F, 0.48F), true, 0.9F).has_value(),
        "A diagonal trajectory was incorrectly recognized as a swipe.");

    recognizer.reset();
    static_cast<void>(recognizer.process(
        swipeFrame(20, 3'000'000, 0.30F, 0.50F), true, 0.9F));
    require(!recognizer.process(
        swipeFrame(21, 3'180'000, 0.50F, 0.50F, 0.20F), true, 0.9F).has_value(),
        "Large depth/scale motion was incorrectly recognized as a swipe.");
}

void testOrderedHandEventRing()
{
    using namespace ryoiki::hand_input;
    publication::OrderedHandEventRing ring;
    for (std::uint64_t index = 0;
         index < publication::kHandEventRingCapacity + 3;
         ++index)
    {
        recognition::HandEvent event{};
        event.id = index % 2 == 0
            ? recognition::kSwipeLeftEventId
            : recognition::kSwipeRightEventId;
        ring.publish(event);
    }
    const auto first = ring.readAfter(0);
    require(first.droppedCount == 3,
        "Ordered Event ring did not report overwritten events.");
    require(first.count == publication::kMaxHandEventBatch,
        "Ordered Event ring did not respect the ABI batch bound.");
    for (std::size_t index = 1; index < first.count; ++index)
    {
        require(first.events[index].sequence
                == first.events[index - 1].sequence + 1,
            "Ordered Event ring returned non-monotonic events.");
    }
    const auto second = ring.readAfter(first.nextSequence);
    require(second.count > 0
            && second.events[0].sequence == first.nextSequence + 1,
        "Ordered Event cursor skipped or repeated an event.");
    ring.reset();
    require(ring.readAfter(0).count == 0,
        "Ordered Event reset retained published events.");
}

void testTimedStateStabilizerLifecycle()
{
    using namespace ryoiki::hand_input::recognition;
    TimedStateStabilizer stabilizer{TimedStateConfig{
        .enterDurationUs = 150'000,
        .exitDurationUs = 120'000,
        .missingGraceUs = 220'000}};

    auto state = stabilizer.update(
        kDomainExpansionStateId, true, true, 0.8F, 0.9F, 10, 1'000'000);
    require(state.phase == HandStatePhase::Candidate
            && state.transition == HandStateTransition::None,
        "State did not enter Candidate on the first matching sample.");

    state = stabilizer.update(
        kDomainExpansionStateId, true, true, 0.82F, 0.9F, 11, 1'149'999);
    require(state.phase == HandStatePhase::Candidate,
        "State became Active before the enter duration.");

    state = stabilizer.update(
        kDomainExpansionStateId, true, true, 0.84F, 0.9F, 12, 1'150'000);
    require(state.phase == HandStatePhase::Active
            && state.transition == HandStateTransition::Began
            && state.beganFrameId == 12,
        "State did not publish Began at the enter duration.");

    state = stabilizer.update(
        kDomainExpansionStateId, false, false, 0.0F, 0.0F, 13, 1'200'000);
    require(state.phase == HandStatePhase::Active
            && state.transition == HandStateTransition::None
            && state.flags == HandStateFlags::Stale,
        "A short invalid observation did not preserve an Active stale state.");

    state = stabilizer.update(
        kDomainExpansionStateId, false, false, 0.0F, 0.0F, 14, 1'420'000);
    require(state.phase == HandStatePhase::Inactive
            && state.transition == HandStateTransition::Cancelled,
        "Tracking loss beyond grace did not cancel the state.");

    static_cast<void>(stabilizer.update(
        kDomainExpansionStateId, true, true, 0.8F, 0.9F, 20, 2'000'000));
    state = stabilizer.update(
        kDomainExpansionStateId, true, true, 0.8F, 0.9F, 21, 2'150'000);
    require(state.transition == HandStateTransition::Began,
        "State did not reactivate after cancellation.");

    state = stabilizer.update(
        kDomainExpansionStateId, true, false, 0.2F, 0.9F, 22, 2'200'000);
    require(state.phase == HandStatePhase::Active
            && state.flags == HandStateFlags::ExitPending,
        "State ended before the exit duration.");

    state = stabilizer.update(
        kDomainExpansionStateId, true, false, 0.2F, 0.9F, 23, 2'320'000);
    require(state.phase == HandStatePhase::Inactive
            && state.transition == HandStateTransition::Ended,
        "A valid non-match did not publish Ended after the exit duration.");
}

void testLatestHandStateSlot()
{
    using namespace ryoiki::hand_input;
    publication::LatestHandStateSlot slot;
    publication::HandStateSnapshot snapshot{};
    snapshot.frameId = 42;
    snapshot.timestampUs = 9'000'000;
    snapshot.count = 1;
    snapshot.states[0].id = recognition::kDomainExpansionStateId;
    snapshot.states[0].phase = recognition::HandStatePhase::Active;
    slot.publish(snapshot);

    const auto latest = slot.latest();
    require(latest.frameId == 42 && latest.count == 1,
        "Latest State slot did not retain snapshot identity.");
    require(latest.states[0].phase == recognition::HandStatePhase::Active,
        "Latest State slot did not retain the published phase.");

    slot.reset();
    require(slot.latest().count == 0, "Latest State slot reset retained stale state.");
}

void testHandLandmarkModelContractUsesSemanticNamesAndShapes()
{
    using ryoiki::hand_perception::HandLandmarkModelContract;
    using ryoiki::hand_perception::ModelTensorDescriptor;

    const auto contract = HandLandmarkModelContract::openCvZoo2023();
    const std::vector<ModelTensorDescriptor> inputs{contract.input};
    const std::vector<ModelTensorDescriptor> reorderedOutputs{
        contract.worldLandmarks,
        contract.handedness,
        contract.imageLandmarks,
        contract.presence};
    std::string error{"stale error"};

    require(ryoiki::hand_perception::validateHandLandmarkModelContract(
        inputs, reorderedOutputs, contract, error),
        "Hand landmark contract depended on model output enumeration order.");
    require(error.empty(), "Successful hand landmark contract validation retained a stale error.");

    auto wrongNameOutputs = reorderedOutputs;
    wrongNameOutputs[0].name = "unexpected_world_landmarks";
    require(!ryoiki::hand_perception::validateHandLandmarkModelContract(
        inputs, wrongNameOutputs, contract, error),
        "Hand landmark contract accepted an output with the wrong semantic name.");
    require(!error.empty(), "Wrong hand output name did not produce a contract error.");

    auto wrongShapeOutputs = reorderedOutputs;
    wrongShapeOutputs[2].shape = {1, 62};
    require(!ryoiki::hand_perception::validateHandLandmarkModelContract(
        inputs, wrongShapeOutputs, contract, error),
        "Hand landmark contract accepted an output with the wrong shape.");
    require(!error.empty(), "Wrong hand output shape did not produce a contract error.");

    auto wrongInput = inputs;
    wrongInput[0].shape = {1, 3, 224, 224};
    require(!ryoiki::hand_perception::validateHandLandmarkModelContract(
        wrongInput, reorderedOutputs, contract, error),
        "Hand landmark contract accepted an input with the wrong layout.");
    require(!error.empty(), "Wrong hand input shape did not produce a contract error.");
}

void testOrtProviderDistribution()
{
    const auto providers = Ort::GetAvailableProviders();
    require(std::find(providers.begin(), providers.end(), "CPUExecutionProvider")
            != providers.end(),
        "ONNX Runtime distribution did not expose CPUExecutionProvider.");
#if defined(RYOIKI_ORT_QNN)
    require(std::find(providers.begin(), providers.end(), "QNNExecutionProvider") != providers.end(),
        "ONNX Runtime distribution did not expose QNNExecutionProvider.");
    const auto qnnConfiguration =
        ryoiki::hand_perception::OrtSessionConfiguration::qnnHtp();
    require(qnnConfiguration.executionProvider()
            == ryoiki::hand_perception::ExecutionProvider::QnnHtp,
        "QNN HTP configuration reported the wrong execution provider.");
    require(qnnConfiguration.providerName() == "QNNExecutionProvider(HTP)",
        "QNN HTP configuration reported the wrong provider name.");

    Ort::SessionOptions options;
    qnnConfiguration.apply(options);
#elif defined(RYOIKI_ORT_DIRECTML)
    require(std::find(providers.begin(), providers.end(), "DmlExecutionProvider") != providers.end(),
        "ONNX Runtime distribution did not expose DmlExecutionProvider.");
    const auto directMlConfiguration =
        ryoiki::hand_perception::OrtSessionConfiguration::directMl();
    require(directMlConfiguration.executionProvider()
            == ryoiki::hand_perception::ExecutionProvider::DirectMl,
        "DirectML configuration reported the wrong execution provider.");
    Ort::SessionOptions options;
    directMlConfiguration.apply(options);
#endif
}

void testDirectMlOrtRunnerSmoke()
{
#if defined(RYOIKI_ORT_DIRECTML) && defined(RYOIKI_PALM_MODEL_PATH) && defined(RYOIKI_HAND_MODEL_PATH)
    const auto configuration = ryoiki::hand_perception::OrtSessionConfiguration::directMl();
    std::string error;
    auto palmRunner = ryoiki::hand_perception::OrtPalmDetectionRunner::create(
        std::filesystem::path{RYOIKI_PALM_MODEL_PATH}, configuration, error);
    require(palmRunner != nullptr, error.c_str());
    ryoiki::buffers::FloatTensorBuffer palmInput{{1, 192, 192, 3}};
    PalmDetectionRawOutput palmOutput;
    ryoiki::hand_perception::ModelRunResult palmResult{};
    require(palmRunner->run(palmInput, palmOutput, palmResult, error), error.c_str());

    auto handRunner = ryoiki::hand_perception::OrtHandLandmarkRunner::create(
        std::filesystem::path{RYOIKI_HAND_MODEL_PATH},
        ryoiki::hand_perception::HandLandmarkModelContract::openCvZoo2023(),
        configuration,
        error);
    require(handRunner != nullptr, error.c_str());
    ryoiki::buffers::FloatTensorBuffer handInput{{1, 224, 224, 3}};
    ryoiki::hand_perception::HandLandmarkRawOutput handOutput{};
    ryoiki::hand_perception::ModelRunResult handResult{};
    require(handRunner->run(handInput, handOutput, handResult, error), error.c_str());
    require(palmResult.inferenceMs >= 0.0 && handResult.inferenceMs >= 0.0,
        "DirectML runner reported a negative inference duration.");
#endif
}

void testDirectMlCustomDeviceRunnerSmoke()
{
#if defined(RYOIKI_ORT_DIRECTML) && defined(RYOIKI_PALM_MODEL_PATH)
    std::string error;
    const auto d3d11 = ryoiki::runtime::D3d11Device::create(error);
    require(d3d11 != nullptr, error.c_str());
    const auto runtime = ryoiki::runtime::DirectMlRuntime::create(d3d11, error);
    require(runtime != nullptr, error.c_str());
    const auto configuration =
        ryoiki::hand_perception::OrtSessionConfiguration::directMl(runtime);
    auto runner = ryoiki::hand_perception::OrtPalmDetectionRunner::create(
        std::filesystem::path{RYOIKI_PALM_MODEL_PATH}, configuration, error);
    require(runner != nullptr, error.c_str());
    ryoiki::buffers::FloatTensorBuffer input{{1, 192, 192, 3}};
    PalmDetectionRawOutput output;
    ryoiki::hand_perception::ModelRunResult result{};
    require(runner->run(input, output, result, error), error.c_str());
#endif
}

void testDirectMlD3d12PreprocessRunnerSmoke()
{
#if defined(RYOIKI_ORT_DIRECTML) && defined(RYOIKI_PALM_MODEL_PATH)
    std::string error;
    const auto d3d11 = ryoiki::runtime::D3d11Device::createOn12(error);
    require(d3d11 != nullptr, error.c_str());
    const auto runtime = ryoiki::runtime::DirectMlRuntime::create(d3d11, error);
    require(runtime != nullptr, error.c_str());
    D3D11_TEXTURE2D_DESC description{};
    description.Width=320; description.Height=240; description.MipLevels=1;
    description.ArraySize=1; description.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count=1; description.Usage=D3D11_USAGE_DEFAULT;
    description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    require(SUCCEEDED(d3d11->device()->CreateTexture2D(&description,nullptr,&texture)),
        "Failed to create D3D12 preprocess input.");
    ryoiki::buffers::FrameBuffer frame;
    require(frame.prepareGpu(320,240,1,0,texture,0,{},
        ryoiki::runtime::FrameRotation::None),"Failed to prepare GPU frame.");
    auto geometry=ryoiki::geometry::D3d12HandGeometryProcessor::create(runtime,error);
    require(geometry!=nullptr,error.c_str());
    ryoiki::buffers::FloatTensorBuffer input{{1,192,192,3}};
    ryoiki::geometry::PalmPreprocessResult preprocess{};
    auto runner=ryoiki::hand_perception::OrtPalmDetectionRunner::create(
        std::filesystem::path{RYOIKI_PALM_MODEL_PATH},
        ryoiki::hand_perception::OrtSessionConfiguration::directMl(runtime),error);
    require(runner!=nullptr,error.c_str());
    for (std::uint64_t iteration = 1; iteration <= 30; ++iteration)
    {
        require(frame.prepareGpu(320,240,iteration,0,texture,0,{},
            ryoiki::runtime::FrameRotation::None),"Failed to update GPU frame.");
        require(geometry->preprocessPalm(frame,input,preprocess),
            "D3D12 palm preprocess failed.");
        require(input.gpuResource()!=nullptr,"D3D12 preprocess produced no GPU tensor.");
        PalmDetectionRawOutput output;
        ryoiki::hand_perception::ModelRunResult result{};
        require(runner->run(input,output,result,error),error.c_str());
    }
#endif
}


void testCpuOrtPalmRunnerSmoke()
{
#ifdef RYOIKI_PALM_MODEL_PATH
    const std::filesystem::path modelPath{RYOIKI_PALM_MODEL_PATH};
    if (!std::filesystem::is_regular_file(modelPath))
    {
        std::cout << "CPU ORT palm runner smoke skipped: model not found.\n";
        return;
    }

    std::string error;
    auto runner = ryoiki::hand_perception::OrtPalmDetectionRunner::create(modelPath, error);
    require(runner != nullptr, error.c_str());
    require(runner->executionProvider() == ryoiki::hand_perception::ExecutionProvider::Cpu,
        "CPU runner reported the wrong execution provider.");
    require(runner->providerName() == "CPUExecutionProvider", "CPU runner provider name is incorrect.");

    ryoiki::buffers::FloatTensorBuffer input{{1, 192, 192, 3}};
    std::fill(input.data(), input.data() + input.elementCount(), 0.0F);
    PalmDetectionRawOutput output;
    ryoiki::hand_perception::ModelRunResult result{};
    require(runner->run(input, output, result, error), error.c_str());
    require(result.inferenceMs >= 0.0, "CPU runner reported a negative inference duration.");
    require(std::all_of(output.regressions().begin(), output.regressions().end(),
        [](const float value) { return std::isfinite(value); }), "CPU runner emitted non-finite regressions.");
    require(std::all_of(output.scores().begin(), output.scores().end(),
        [](const float value) { return std::isfinite(value); }), "CPU runner emitted non-finite scores.");
#endif
}

void testCpuOrtHandLandmarkRunnerSmoke()
{
#ifdef RYOIKI_HAND_MODEL_PATH
    const std::filesystem::path modelPath{RYOIKI_HAND_MODEL_PATH};
    if (!std::filesystem::is_regular_file(modelPath))
    {
        std::cout << "CPU ORT hand runner smoke skipped: model not found.\n";
        return;
    }

    std::string error;
    auto runner = ryoiki::hand_perception::OrtHandLandmarkRunner::create(modelPath, error);
    require(runner != nullptr, error.c_str());
    require(runner->executionProvider() == ryoiki::hand_perception::ExecutionProvider::Cpu,
        "CPU hand runner reported the wrong execution provider.");
    require(runner->providerName() == "CPUExecutionProvider",
        "CPU hand runner provider name is incorrect.");

    ryoiki::buffers::FloatTensorBuffer wrongInput{{1, 223, 224, 3}};
    ryoiki::hand_perception::HandLandmarkRawOutput output;
    ryoiki::hand_perception::ModelRunResult result{};
    require(!runner->run(wrongInput, output, result, error),
        "CPU hand runner accepted an incorrect input shape.");
    require(!error.empty(), "CPU hand runner did not explain an incorrect input shape.");

    ryoiki::buffers::FloatTensorBuffer input{{1, 224, 224, 3}};
    std::fill(input.data(), input.data() + input.elementCount(), 0.0F);
    require(runner->run(input, output, result, error), error.c_str());
    require(error.empty(), "Successful hand inference did not clear the previous error.");
    require(result.inferenceMs >= 0.0, "CPU hand runner reported a negative inference duration.");
    require(std::all_of(output.imageLandmarks.begin(), output.imageLandmarks.end(),
        [](const float value) { return std::isfinite(value); }),
        "CPU hand runner emitted non-finite image landmarks.");
    require(std::all_of(output.worldLandmarks.begin(), output.worldLandmarks.end(),
        [](const float value) { return std::isfinite(value); }),
        "CPU hand runner emitted non-finite world landmarks.");
    require(std::isfinite(output.presence) && output.presence >= 0.0F && output.presence <= 1.0F,
        "CPU hand runner emitted an invalid presence score.");
    require(std::isfinite(output.handedness) && output.handedness >= 0.0F && output.handedness <= 1.0F,
        "CPU hand runner emitted an invalid handedness score.");

#endif
}
}

int main()
{
    try
    {
        testSigmoidThresholdAndAnchorDecode();
        testLetterboxInverseAndKeypointDecode();
        testNmsAndNonFiniteExclusion();
        testMaximumDetectionCount();
        testPalmDetectionToUprightRoi();
        testPalmDetectionToRotatedRoi();
        testHandRawCoordinatesMapToSourceRoi();
        testHandLandmarksToRoiUsesPartialLandmarks();
        testHandLandmarkGraphDecodeAndThreshold();
        testHandLandmarkGraphRejectsInvalidOutput();
        testPalmDetectionGraphSuccess();
        testPalmDetectionGraphFailure();
        testHandPerceptionTrackingAndPalmFallback();
        testCalibrationPalmProbeKeepsTrackedRoi();
        testHandPerceptionTracksTwoHandsWithSequentialInference();
        testHandPerceptionUsesReliableHandednessAcrossPositionConflict();
        testHandPerceptionReacquiresOneOfTwoTracksWithoutReplacingItsId();
        testHandPerceptionContinuesAfterSingleFrameRunnerFailures();
        testHandMeasurementExtractorCanonicalization();
        testHandTopologyHistorySummarizesAndBounds();
        testHandTopologyHistoryTolerateShortGapAndAccumulatePath();
        testHandTopologyHistoryPrunesToRollingTimeWindow();
        testMultiHandRelationOrderingIsDeterministic();
        testRyoikiHandTopologyAbiStructSizesAreBlittable();
        testMultiHandMeasurementStageKeepsPerTrackHistory();
        testWeightedPalmRotationTracker();
        testWeightedPalmRotationTrackerMixedRotationAndDegeneracy();
        testPalmRotationEskfSuppressesJitterAndTracksMotion();
        testPalmBasisRotationMatchesRigidLandmarkRotation();
        testPalmRotationOneEuroSuppressesJitterWithLowLag();
        testRotationObservationGateHoldsAndReacquiresContinuously();
        testRotationObservationGateAbsorbsTransientFlips();
        testRotationObservationGateReportsRebaseDrift();
        testNativeCadHandBindingRezeroesAfterTrackingGap();
        testNativeCadHandBindingSensitivityAndPresentation();
        testDomainExpansionGestureGeometry();
        testOpenPalmStateRecognition();
        testSwipeEventRecognition();
        testOrderedHandEventRing();
        testTimedStateStabilizerLifecycle();
        testLatestHandStateSlot();
        testHandLandmarkModelContractUsesSemanticNamesAndShapes();
        testOrtProviderDistribution();
        testDirectMlOrtRunnerSmoke();
        testDirectMlCustomDeviceRunnerSmoke();
        testDirectMlD3d12PreprocessRunnerSmoke();
        testCpuOrtPalmRunnerSmoke();
        testCpuOrtHandLandmarkRunnerSmoke();
        std::cout << "HandPerception tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
