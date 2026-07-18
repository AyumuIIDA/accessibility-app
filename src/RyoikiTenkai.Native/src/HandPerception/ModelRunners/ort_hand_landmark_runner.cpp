#include "HandPerception/ModelRunners/ort_hand_landmark_runner.h"
#include "HandPerception/ModelRunners/calibration_tensor_capture.h"

#include <onnxruntime_cxx_api.h>
#if defined(RYOIKI_ORT_DIRECTML)
#include <dml_provider_factory.h>
#include "Runtime/directml_runtime.h"
#endif

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

struct OrtHandLandmarkRunner::Impl
{
    Impl(
        const std::filesystem::path& modelPath,
        const HandLandmarkModelContract& contract,
        const OrtSessionConfiguration& configuration)
        : environment{ORT_LOGGING_LEVEL_WARNING, "RyoikiTenkai"},
          executionProvider{configuration.executionProvider()},
          providerName{configuration.providerName()},
          directMlRuntime{configuration.directMlRuntime()}
    {
        if (!std::filesystem::is_regular_file(modelPath))
        {
            throw std::runtime_error{"Hand landmark model was not found: " + modelPath.string()};
        }

        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        configuration.apply(sessionOptions);
        session = std::make_unique<Ort::Session>(environment, modelPath.c_str(), sessionOptions);

        const auto requiredInputShape = std::vector<std::int64_t>{
            kInputShape.begin(), kInputShape.end()};
        const auto requiredLandmarkShape = std::vector<std::int64_t>{
            kLandmarkShape.begin(), kLandmarkShape.end()};
        const auto requiredScalarShape = std::vector<std::int64_t>{
            kScalarShape.begin(), kScalarShape.end()};
        if (contract.input.shape != requiredInputShape
            || contract.imageLandmarks.shape != requiredLandmarkShape
            || contract.presence.shape != requiredScalarShape
            || contract.handedness.shape != requiredScalarShape
            || contract.worldLandmarks.shape != requiredLandmarkShape)
        {
            throw std::runtime_error{"Selected hand landmark contract uses unsupported tensor shapes."};
        }

        Ort::AllocatorWithDefaultOptions allocator;
        std::vector<ModelTensorDescriptor> inputDescriptors;
        std::vector<ModelTensorDescriptor> outputDescriptors;
        inputDescriptors.reserve(session->GetInputCount());
        outputDescriptors.reserve(session->GetOutputCount());
        for (std::size_t index = 0; index < session->GetInputCount(); ++index)
        {
            const auto typeInfo = session->GetInputTypeInfo(index);
            const auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
            if (tensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            {
                throw std::runtime_error{"Hand landmark model inputs must use float32."};
            }
            auto name = session->GetInputNameAllocated(index, allocator);
            inputDescriptors.push_back({name.get(), tensorInfo.GetShape()});
        }
        for (std::size_t index = 0; index < session->GetOutputCount(); ++index)
        {
            const auto typeInfo = session->GetOutputTypeInfo(index);
            const auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
            if (tensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            {
                throw std::runtime_error{"Hand landmark model outputs must use float32."};
            }
            auto name = session->GetOutputNameAllocated(index, allocator);
            outputDescriptors.push_back({name.get(), tensorInfo.GetShape()});
        }

        std::string contractError;
        if (!validateHandLandmarkModelContract(
                inputDescriptors,
                outputDescriptors,
                contract,
                contractError))
        {
            throw std::runtime_error{contractError};
        }

        inputNameStorage = contract.input.name;
        outputNames = {
            contract.imageLandmarks.name,
            contract.presence.name,
            contract.handedness.name,
            contract.worldLandmarks.name
        };
    }

    Ort::Env environment;
    Ort::SessionOptions sessionOptions;
    std::unique_ptr<Ort::Session> session;
    std::string inputNameStorage;
    std::array<std::string, 4> outputNames{};
    ExecutionProvider executionProvider{ExecutionProvider::Cpu};
    std::string providerName;
    CalibrationTensorCapture calibrationCapture{"hand", kInputShape};
    std::shared_ptr<runtime::DirectMlRuntime> directMlRuntime;
};

std::unique_ptr<OrtHandLandmarkRunner> OrtHandLandmarkRunner::create(
    const std::filesystem::path& modelPath,
    std::string& error)
{
    return create(modelPath, HandLandmarkModelContract::openCvZoo2023(), error);
}

std::unique_ptr<OrtHandLandmarkRunner> OrtHandLandmarkRunner::create(
    const std::filesystem::path& modelPath,
    const HandLandmarkModelContract& contract,
    std::string& error)
{
    return create(modelPath, contract, OrtSessionConfiguration::cpu(), error);
}

std::unique_ptr<OrtHandLandmarkRunner> OrtHandLandmarkRunner::create(
    const std::filesystem::path& modelPath,
    const HandLandmarkModelContract& contract,
    const OrtSessionConfiguration& configuration,
    std::string& error)
{
    try
    {
        error.clear();
        return std::unique_ptr<OrtHandLandmarkRunner>{
            new OrtHandLandmarkRunner{
                std::make_unique<Impl>(modelPath, contract, configuration)}};
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

OrtHandLandmarkRunner::OrtHandLandmarkRunner(std::unique_ptr<Impl> impl)
    : impl_{std::move(impl)}
{
}

OrtHandLandmarkRunner::~OrtHandLandmarkRunner() = default;

ExecutionProvider OrtHandLandmarkRunner::executionProvider() const noexcept
{
    return impl_->executionProvider;
}

std::string_view OrtHandLandmarkRunner::providerName() const noexcept
{
    return impl_->providerName;
}

bool OrtHandLandmarkRunner::run(
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
        if (input.gpuResource() == nullptr) impl_->calibrationCapture.capture(input);
        const auto started = clock::now();
        auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        void* dmlAllocation = nullptr;
        Ort::Value inputValue{nullptr};
        if (input.gpuResource() != nullptr && impl_->directMlRuntime != nullptr)
        {
#if defined(RYOIKI_ORT_DIRECTML)
            impl_->directMlRuntime->waitForPreprocess(input.readyFenceValue());
            const OrtDmlApi* dmlApi = nullptr;
            Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi(
                "DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dmlApi)));
            Ort::ThrowOnError(dmlApi->CreateGPUAllocationFromD3DResource(
                input.gpuResource(), &dmlAllocation));
            Ort::MemoryInfo dmlMemory{"DML", OrtDeviceAllocator, 0, OrtMemTypeDefault};
            inputValue = Ort::Value::CreateTensor(
                dmlMemory, dmlAllocation, input.elementCount() * sizeof(float),
                kInputShape.data(), kInputShape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
#endif
        }
        else
        {
            inputValue = Ort::Value::CreateTensor<float>(
                memoryInfo, const_cast<float*>(input.data()), input.elementCount(),
                kInputShape.data(), kInputShape.size());
        }

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
#if defined(RYOIKI_ORT_DIRECTML)
        if (dmlAllocation != nullptr)
        {
            const OrtDmlApi* dmlApi = nullptr;
            Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi(
                "DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dmlApi)));
            dmlApi->FreeGPUAllocation(dmlAllocation);
        }
#endif
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
