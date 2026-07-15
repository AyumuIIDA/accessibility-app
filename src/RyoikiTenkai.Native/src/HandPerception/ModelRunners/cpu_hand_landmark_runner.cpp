#include "HandPerception/ModelRunners/cpu_hand_landmark_runner.h"

#include <onnxruntime_cxx_api.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ryoiki::hand_perception
{
namespace
{
constexpr std::array<std::int64_t, 4> kInputShape{1, 224, 224, 3};
constexpr std::array<std::int64_t, 2> kLandmarkShape{1, 63};
constexpr std::array<std::int64_t, 2> kScalarShape{1, 1};
}

struct CpuHandLandmarkRunner::Impl
{
    explicit Impl(const std::filesystem::path& modelPath)
        : environment{ORT_LOGGING_LEVEL_WARNING, "RyoikiTenkai"}
    {
        if (!std::filesystem::is_regular_file(modelPath))
        {
            throw std::runtime_error{"Hand landmark model was not found: " + modelPath.string()};
        }

        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        session = std::make_unique<Ort::Session>(environment, modelPath.c_str(), sessionOptions);

        Ort::AllocatorWithDefaultOptions allocator;
        if (session->GetInputCount() != 1)
        {
            throw std::runtime_error{"Hand landmark model must expose exactly one input."};
        }
        auto inputName = session->GetInputNameAllocated(0, allocator);
        inputNameStorage = inputName.get();
        const auto inputShape = session->GetInputTypeInfo(0)
            .GetTensorTypeAndShapeInfo().GetShape();
        if (inputShape != std::vector<std::int64_t>{kInputShape.begin(), kInputShape.end()})
        {
            throw std::runtime_error{"Hand landmark model input shape must be [1,224,224,3]."};
        }

        constexpr std::array<const char*, 4> kExpectedOutputs{
            "Identity", "Identity_1", "Identity_2", "Identity_3"};
        if (session->GetOutputCount() != kExpectedOutputs.size())
        {
            throw std::runtime_error{"Hand landmark model must expose four outputs."};
        }
        for (std::size_t index = 0; index < kExpectedOutputs.size(); ++index)
        {
            auto outputName = session->GetOutputNameAllocated(index, allocator);
            outputNames[index] = outputName.get();
            if (outputNames[index] != kExpectedOutputs[index])
            {
                throw std::runtime_error{"Hand landmark model output names do not match the expected contract."};
            }
        }
    }

    Ort::Env environment;
    Ort::SessionOptions sessionOptions;
    std::unique_ptr<Ort::Session> session;
    std::string inputNameStorage;
    std::array<std::string, 4> outputNames{};
};

std::unique_ptr<CpuHandLandmarkRunner> CpuHandLandmarkRunner::create(
    const std::filesystem::path& modelPath,
    std::string& error)
{
    try
    {
        error.clear();
        return std::unique_ptr<CpuHandLandmarkRunner>{
            new CpuHandLandmarkRunner{std::make_unique<Impl>(modelPath)}};
    }
    catch (const Ort::Exception& exception)
    {
        error = "ONNX Runtime hand session creation failed: " + std::string{exception.what()};
        return {};
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return {};
    }
}

CpuHandLandmarkRunner::CpuHandLandmarkRunner(std::unique_ptr<Impl> impl)
    : impl_{std::move(impl)}
{
}

CpuHandLandmarkRunner::~CpuHandLandmarkRunner() = default;

ExecutionProvider CpuHandLandmarkRunner::executionProvider() const noexcept
{
    return ExecutionProvider::Cpu;
}

std::string_view CpuHandLandmarkRunner::providerName() const noexcept
{
    return "CPUExecutionProvider";
}

bool CpuHandLandmarkRunner::run(
    const buffers::FloatTensorBuffer& input,
    HandLandmarkRawOutput& output,
    ModelRunResult& result,
    std::string& error)
{
    using clock = std::chrono::steady_clock;
    if (input.shape() != kInputShape)
    {
        error = "Hand landmark input tensor shape must be [1,224,224,3].";
        return false;
    }

    try
    {
        const auto started = clock::now();
        auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto inputValue = Ort::Value::CreateTensor<float>(
            memoryInfo,
            const_cast<float*>(input.data()),
            input.elementCount(),
            kInputShape.data(),
            kInputShape.size());

        float presence = 0.0F;
        float handedness = 0.0F;
        std::array<Ort::Value, 4> outputValues{
            Ort::Value::CreateTensor<float>(memoryInfo, output.imageLandmarks.data(),
                output.imageLandmarks.size(), kLandmarkShape.data(), kLandmarkShape.size()),
            Ort::Value::CreateTensor<float>(memoryInfo, &presence, 1, kScalarShape.data(), kScalarShape.size()),
            Ort::Value::CreateTensor<float>(memoryInfo, &handedness, 1, kScalarShape.data(), kScalarShape.size()),
            Ort::Value::CreateTensor<float>(memoryInfo, output.worldLandmarks.data(),
                output.worldLandmarks.size(), kLandmarkShape.data(), kLandmarkShape.size())};
        const char* inputNames[]{impl_->inputNameStorage.c_str()};
        std::array<const char*, 4> outputNames{};
        for (std::size_t index = 0; index < outputNames.size(); ++index)
        {
            outputNames[index] = impl_->outputNames[index].c_str();
        }
        impl_->session->Run(
            Ort::RunOptions{nullptr}, inputNames, &inputValue, 1,
            outputNames.data(), outputValues.data(), outputValues.size());
        output.presence = presence;
        output.handedness = handedness;
        result.inferenceMs = std::chrono::duration<double, std::milli>(
            clock::now() - started).count();
        error.clear();
        return true;
    }
    catch (const Ort::Exception& exception)
    {
        error = "ONNX Runtime hand inference failed: " + std::string{exception.what()};
        return false;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
}
