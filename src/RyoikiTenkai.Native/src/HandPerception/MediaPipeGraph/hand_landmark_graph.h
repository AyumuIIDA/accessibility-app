#pragma once

#include "Buffers/frame_buffer.h"
#include "Buffers/tensor_buffer.h"
#include "Geometry/geometry_types.h"
#include "Geometry/hand_geometry_processor.h"
#include "HandPerception/MediaPipeGraph/palm_detection_postprocessor.h"
#include "HandPerception/ModelRunners/hand_landmark_runner.h"

#include <array>
#include <memory>
#include <string>

namespace ryoiki::hand_perception
{
struct Landmark3f
{
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

struct HandLandmarkResult
{
    std::array<Landmark3f, 21> landmarks{};
    std::array<Landmark3f, 21> worldLandmarks{};
    BoundingBox box;
    float confidence{0.0F};
    float handedness{0.0F};
    bool detected{false};
};

struct HandLandmarkGraphMetrics
{
    double roiCropWarpMs{0.0};
    double inferenceMs{0.0};
    double postprocessMs{0.0};
};

class HandLandmarkGraph final
{
public:
    explicit HandLandmarkGraph(std::unique_ptr<IHandLandmarkRunner> runner);
    HandLandmarkGraph(
        std::unique_ptr<IHandLandmarkRunner> runner,
        std::unique_ptr<geometry::IGeometryProcessor> geometryProcessor);

    bool process(
        const buffers::FrameBuffer& frame,
        const geometry::RotatedRegion& region,
        float confidenceThreshold,
        HandLandmarkResult& result,
        HandLandmarkGraphMetrics& metrics,
        std::string& error);

    [[nodiscard]] std::string_view providerName() const noexcept;

private:
    std::unique_ptr<IHandLandmarkRunner> runner_;
    std::unique_ptr<geometry::IGeometryProcessor> geometryProcessor_;
    buffers::FloatTensorBuffer inputTensor_{{1, 224, 224, 3}};
    HandLandmarkRawOutput rawOutput_;
};
}
