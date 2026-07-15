#include "HandPerception/ModelRunners/cpu_palm_detection_runner.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
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
constexpr std::array<std::int64_t, 4> kInputShape{1, 192, 192, 3};
constexpr std::array<std::int64_t, 3> kRegressionShape{1, 2016, 18};
constexpr std::array<std::int64_t, 3> kScoreShape{1, 2016, 1};

bool hasShape(const std::vector<std::int64_t>& actual, const std::span<const std::int64_t> expected)
{
    return actual.size() == expected.size()
        && std::equal(actual.begin(), actual.end(), expected.begin());
}
}

struct CpuPalmDetectionRunner::Impl
{
    explicit Impl(const std::filesystem::path& modelPath)
        : environment{ORT_LOGGING_LEVEL_WARNING, "RyoikiTenkai"}
    {
        if (!std::filesystem::is_regular_file(modelPath))
        {
            throw std::runtime_error{"Palm detection model was not found: " + modelPath.string()};
        }

        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        session = std::make_unique<Ort::Session>(
            environment,
            modelPath.c_str(),
            sessionOptions);

        Ort::AllocatorWithDefaultOptions allocator;
        if (session->GetInputCount() != 1)
        {
            throw std::runtime_error{"Palm model must expose exactly one input."};
        }

        auto inputName = session->GetInputNameAllocated(0, allocator);
        inputNameStorage = inputName.get();
        const auto inputShape = session->GetInputTypeInfo(0)
            .GetTensorTypeAndShapeInfo().GetShape();
        if (!hasShape(inputShape, kInputShape))
        {
            throw std::runtime_error{"Palm model input shape must be [1,192,192,3]."};
        }

        for (std::size_t outputIndex = 0; outputIndex < session->GetOutputCount(); ++outputIndex)
        {
            const auto shape = session->GetOutputTypeInfo(outputIndex)
                .GetTensorTypeAndShapeInfo().GetShape();
            if (hasShape(shape, kRegressionShape))
            {
                regressionOutputIndex = outputIndex;
            }
            else if (hasShape(shape, kScoreShape))
            {
                scoreOutputIndex = outputIndex;
            }
        }

        if (regressionOutputIndex == kMissingIndex || scoreOutputIndex == kMissingIndex)
        {
            throw std::runtime_error{"Palm model outputs must include [1,2016,18] and [1,2016,1]."};
        }

        auto regressionName = session->GetOutputNameAllocated(regressionOutputIndex, allocator);
        auto scoreName = session->GetOutputNameAllocated(scoreOutputIndex, allocator);
        regressionOutputNameStorage = regressionName.get();
        scoreOutputNameStorage = scoreName.get();
    }

    static constexpr std::size_t kMissingIndex = static_cast<std::size_t>(-1);

    Ort::Env environment;
    Ort::SessionOptions sessionOptions;
    std::unique_ptr<Ort::Session> session;
    std::string inputNameStorage;
    std::string regressionOutputNameStorage;
    std::string scoreOutputNameStorage;
    std::size_t regressionOutputIndex{kMissingIndex};
    std::size_t scoreOutputIndex{kMissingIndex};
};

std::unique_ptr<CpuPalmDetectionRunner> CpuPalmDetectionRunner::create(
    const std::filesystem::path& modelPath,
    std::string& error)
{
    try
    {
        error.clear();
        return std::unique_ptr<CpuPalmDetectionRunner>{
            new CpuPalmDetectionRunner{std::make_unique<Impl>(modelPath)}};
    }
    catch (const Ort::Exception& exception)
    {
        error = "ONNX Runtime palm session creation failed: " + std::string{exception.what()};
        return {};
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return {};
    }
}

CpuPalmDetectionRunner::CpuPalmDetectionRunner(std::unique_ptr<Impl> impl)
    : impl_{std::move(impl)}
{
}

CpuPalmDetectionRunner::~CpuPalmDetectionRunner() = default;

ExecutionProvider CpuPalmDetectionRunner::executionProvider() const noexcept
{
    return ExecutionProvider::Cpu;
}

std::string_view CpuPalmDetectionRunner::providerName() const noexcept
{
    return "CPUExecutionProvider";
}

bool CpuPalmDetectionRunner::run(
    const buffers::FloatTensorBuffer& input,
    PalmDetectionRawOutput& output,
    ModelRunResult& result,
    std::string& error)
{
    using clock = std::chrono::steady_clock;
    if (input.shape() != kInputShape)
    {
        error = "Palm input tensor shape must be [1,192,192,3].";
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
        auto regressions = output.regressions();
        auto scores = output.scores();
        std::array<Ort::Value, 2> outputValues{
            Ort::Value::CreateTensor<float>(
                memoryInfo,
                regressions.data(),
                regressions.size(),
                kRegressionShape.data(),
                kRegressionShape.size()),
            Ort::Value::CreateTensor<float>(
                memoryInfo,
                scores.data(),
                scores.size(),
                kScoreShape.data(),
                kScoreShape.size())};
        const char* inputNames[]{impl_->inputNameStorage.c_str()};
        const char* outputNames[]{
            impl_->regressionOutputNameStorage.c_str(),
            impl_->scoreOutputNameStorage.c_str()};
        impl_->session->Run(
            Ort::RunOptions{nullptr},
            inputNames,
            &inputValue,
            1,
            outputNames,
            outputValues.data(),
            outputValues.size());
        result.inferenceMs = std::chrono::duration<double, std::milli>(
            clock::now() - started).count();
        error.clear();
        return true;
    }
    catch (const Ort::Exception& exception)
    {
        error = "ONNX Runtime palm inference failed: " + std::string{exception.what()};
        return false;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
}
