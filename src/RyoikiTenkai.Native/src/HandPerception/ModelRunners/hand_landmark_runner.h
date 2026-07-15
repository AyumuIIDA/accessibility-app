#pragma once

#include "Buffers/tensor_buffer.h"
#include "HandPerception/ModelRunners/hand_model_runner.h"
#include "HandPerception/ModelRunners/palm_detection_runner.h"

#include <array>
#include <string>

namespace ryoiki::hand_perception
{
struct HandLandmarkRawOutput
{
    std::array<float, 21 * 3> imageLandmarks{};
    std::array<float, 21 * 3> worldLandmarks{};
    float presence{0.0F};
    float handedness{0.0F};
};

class IHandLandmarkRunner : public IHandModelRunner
{
public:
    ~IHandLandmarkRunner() override = default;

    virtual bool run(
        const buffers::FloatTensorBuffer& input,
        HandLandmarkRawOutput& output,
        ModelRunResult& result,
        std::string& error) = 0;
};
}
