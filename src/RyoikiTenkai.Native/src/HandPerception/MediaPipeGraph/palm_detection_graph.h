#pragma once

#include "Buffers/frame_buffer.h"
#include "Buffers/tensor_buffer.h"
#include "Geometry/hand_geometry_processor.h"
#include "HandPerception/MediaPipeGraph/palm_detection_postprocessor.h"
#include "HandPerception/ModelRunners/palm_detection_runner.h"

#include <memory>
#include <string>

namespace ryoiki::hand_perception
{
struct PalmDetectionGraphMetrics
{
    double preprocessMs{0.0};
    double inferenceMs{0.0};
    double postprocessMs{0.0};
};

class PalmDetectionGraph final
{
public:
    explicit PalmDetectionGraph(std::unique_ptr<IPalmDetectionRunner> runner);

    bool process(
        const buffers::FrameBuffer& frame,
        PalmDetectionResult& result,
        PalmDetectionGraphMetrics& metrics,
        std::string& error);

    [[nodiscard]] std::string_view providerName() const noexcept;

private:
    std::unique_ptr<IPalmDetectionRunner> runner_;
    geometry::HandGeometryProcessor geometryProcessor_;
    buffers::FloatTensorBuffer inputTensor_{{1, 192, 192, 3}};
    PalmDetectionRawOutput rawOutput_;
    PalmDetectionPostprocessor postprocessor_;
};
}
