#include "Buffers/frame_buffer.h"
#include "Buffers/tensor_buffer.h"
#include "Geometry/hand_geometry_processor.h"
#include "HandPerception/MediaPipeGraph/hand_landmark_graph.h"
#include "HandPerception/MediaPipeGraph/hand_landmarks_to_roi.h"
#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"
#include "HandPerception/MediaPipeGraph/palm_detection_graph.h"
#include "HandPerception/MediaPipeGraph/palm_detection_postprocessor.h"
#include "HandPerception/MediaPipeGraph/palm_detection_to_roi.h"
#include "HandPerception/ModelRunners/cpu_hand_landmark_runner.h"
#include "HandPerception/ModelRunners/cpu_palm_detection_runner.h"

#include <algorithm>
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
        if (!succeeds)
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
        if (!succeeds_)
        {
            error = "fake runner failure";
            return false;
        }

        clearRawOutput(output);
        setDetection(output, 0, 5.0F, 0.0F, 0.0F, 2.0F, 2.0F);
        result.inferenceMs = 12.5;
        error.clear();
        return true;
    }

    bool wasCalled{false};
    bool receivedExpectedShape{false};
    std::size_t runCallCount{0};

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
}

void testCpuRunnerSmoke()
{
#ifdef RYOIKI_PALM_MODEL_PATH
    const std::filesystem::path modelPath{RYOIKI_PALM_MODEL_PATH};
    if (!std::filesystem::is_regular_file(modelPath))
    {
        std::cout << "CpuPalmDetectionRunner smoke skipped: model not found.\n";
        return;
    }

    std::string error;
    auto runner = ryoiki::hand_perception::CpuPalmDetectionRunner::create(modelPath, error);
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

void testCpuHandLandmarkRunnerSmoke()
{
#ifdef RYOIKI_HAND_MODEL_PATH
    const std::filesystem::path modelPath{RYOIKI_HAND_MODEL_PATH};
    if (!std::filesystem::is_regular_file(modelPath))
    {
        std::cout << "CpuHandLandmarkRunner smoke skipped: model not found.\n";
        return;
    }

    std::string error;
    auto runner = ryoiki::hand_perception::CpuHandLandmarkRunner::create(modelPath, error);
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
        testCpuRunnerSmoke();
        testCpuHandLandmarkRunnerSmoke();
        std::cout << "HandPerception tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
