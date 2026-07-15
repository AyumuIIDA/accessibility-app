#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ryoiki::hand_perception
{
struct ModelTensorDescriptor
{
    std::string name;
    std::vector<std::int64_t> shape;
};

struct HandLandmarkModelContract
{
    ModelTensorDescriptor input;
    ModelTensorDescriptor imageLandmarks;
    ModelTensorDescriptor presence;
    ModelTensorDescriptor handedness;
    ModelTensorDescriptor worldLandmarks;

    [[nodiscard]] static HandLandmarkModelContract openCvZoo2023();
};

bool validateHandLandmarkModelContract(
    std::span<const ModelTensorDescriptor> inputs,
    std::span<const ModelTensorDescriptor> outputs,
    const HandLandmarkModelContract& contract,
    std::string& error);
}
