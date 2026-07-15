#include "HandPerception/ModelRunners/hand_landmark_model_contract.h"

#include <algorithm>
#include <array>
#include <sstream>

namespace ryoiki::hand_perception
{
namespace
{
bool contains(
    const std::span<const ModelTensorDescriptor> descriptors,
    const ModelTensorDescriptor& expected)
{
    return std::ranges::any_of(descriptors, [&expected](const ModelTensorDescriptor& actual)
    {
        return actual.name == expected.name && actual.shape == expected.shape;
    });
}

std::string describe(const ModelTensorDescriptor& descriptor)
{
    std::ostringstream stream;
    stream << descriptor.name << " [";
    for (std::size_t index = 0; index < descriptor.shape.size(); ++index)
    {
        if (index > 0)
        {
            stream << ',';
        }
        stream << descriptor.shape[index];
    }
    stream << ']';
    return stream.str();
}

std::string describe(const std::span<const ModelTensorDescriptor> descriptors)
{
    std::ostringstream stream;
    for (std::size_t index = 0; index < descriptors.size(); ++index)
    {
        if (index > 0)
        {
            stream << "; ";
        }
        stream << describe(descriptors[index]);
    }
    return stream.str();
}
}

HandLandmarkModelContract HandLandmarkModelContract::openCvZoo2023()
{
    return {
        {"input_1", {1, 224, 224, 3}},
        {"Identity", {1, 63}},
        {"Identity_1", {1, 1}},
        {"Identity_2", {1, 1}},
        {"Identity_3", {1, 63}}
    };
}

bool validateHandLandmarkModelContract(
    const std::span<const ModelTensorDescriptor> inputs,
    const std::span<const ModelTensorDescriptor> outputs,
    const HandLandmarkModelContract& contract,
    std::string& error)
{
    if (inputs.size() != 1 || !contains(inputs, contract.input))
    {
        error = "Hand landmark model input does not match the selected model contract. Expected "
            + describe(contract.input) + "; actual " + describe(inputs) + '.';
        return false;
    }

    constexpr std::size_t kExpectedOutputCount = 4;
    if (outputs.size() != kExpectedOutputCount)
    {
        error = "Hand landmark model must expose exactly four contract outputs.";
        return false;
    }
    const std::array<const ModelTensorDescriptor*, kExpectedOutputCount> expectedOutputs{
        &contract.imageLandmarks,
        &contract.presence,
        &contract.handedness,
        &contract.worldLandmarks
    };
    for (const auto* expected : expectedOutputs)
    {
        if (!contains(outputs, *expected))
        {
            error = "Hand landmark model output does not match the selected model contract. Expected "
                + describe(*expected) + "; actual " + describe(outputs) + '.';
            return false;
        }
    }

    error.clear();
    return true;
}
}
