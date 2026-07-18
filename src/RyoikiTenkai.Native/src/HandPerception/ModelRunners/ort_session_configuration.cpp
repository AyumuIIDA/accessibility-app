#include "HandPerception/ModelRunners/ort_session_configuration.h"
#if defined(RYOIKI_ORT_DIRECTML)
#include "Runtime/directml_runtime.h"
#endif

#if defined(RYOIKI_ORT_DIRECTML)
#include <dml_provider_factory.h>
#endif

#include <stdexcept>
#include <utility>

namespace ryoiki::hand_perception
{
OrtSessionConfiguration OrtSessionConfiguration::cpu()
{
    return OrtSessionConfiguration{
        ExecutionProvider::Cpu, "CPUExecutionProvider", {}, false};
}

OrtSessionConfiguration OrtSessionConfiguration::qnnHtp()
{
    return OrtSessionConfiguration{
        ExecutionProvider::QnnHtp,
        "QNNExecutionProvider(HTP)",
        {
            {"backend_type", "htp"},
            {"htp_performance_mode", "balanced"},
            {"offload_graph_io_quantization", "0"}
        },
        true};
}

OrtSessionConfiguration OrtSessionConfiguration::directMl(
    std::shared_ptr<runtime::DirectMlRuntime> runtime)
{
    auto configuration = OrtSessionConfiguration{
        ExecutionProvider::DirectMl, "DmlExecutionProvider(GPU 0)", {}, true};
    configuration.directMlRuntime_ = std::move(runtime);
    return configuration;
}

OrtSessionConfiguration::OrtSessionConfiguration(
    const ExecutionProvider executionProvider,
    std::string providerName,
    std::unordered_map<std::string, std::string> providerOptions,
    const bool disableCpuFallback)
    : executionProvider_{executionProvider},
      providerName_{std::move(providerName)},
      providerOptions_{std::move(providerOptions)},
      disableCpuFallback_{disableCpuFallback}
{
}

ExecutionProvider OrtSessionConfiguration::executionProvider() const noexcept
{
    return executionProvider_;
}

const std::string& OrtSessionConfiguration::providerName() const noexcept
{
    return providerName_;
}

void OrtSessionConfiguration::apply(Ort::SessionOptions& sessionOptions) const
{
    switch (executionProvider_)
    {
    case ExecutionProvider::Cpu:
        return;
    case ExecutionProvider::QnnHtp:
#if defined(RYOIKI_ORT_QNN)
        if (disableCpuFallback_)
        {
            sessionOptions.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
        }
        sessionOptions.AppendExecutionProvider("QNN", providerOptions_);
        return;
#else
        throw std::invalid_argument{
            "QNN HTP requires a native build with RYOIKI_ORT_BACKEND=qnn."};
#endif
    case ExecutionProvider::DirectMl:
#if defined(RYOIKI_ORT_DIRECTML)
    {
        sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        sessionOptions.DisableMemPattern();
        if (disableCpuFallback_)
        {
            sessionOptions.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
        }
        const OrtDmlApi* dmlApi = nullptr;
        Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi(
            "DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dmlApi)));
        if (dmlApi == nullptr)
        {
            throw std::runtime_error{"The DirectML execution provider API is unavailable."};
        }
        if (directMlRuntime_ != nullptr)
        {
            Ort::ThrowOnError(dmlApi->SessionOptionsAppendExecutionProvider_DML1(
                sessionOptions,
                directMlRuntime_->dmlDevice(),
                directMlRuntime_->commandQueue()));
        }
        else
        {
            Ort::ThrowOnError(dmlApi->SessionOptionsAppendExecutionProvider_DML(
                sessionOptions, 0));
        }
        return;
    }
#else
        throw std::invalid_argument{
            "DirectML requires a native build with RYOIKI_ORT_BACKEND=directml."};
#endif
    case ExecutionProvider::QnnGpu:
        throw std::invalid_argument{"The selected execution provider is not implemented."};
    }

    throw std::invalid_argument{"Unknown execution provider."};
}

const std::shared_ptr<runtime::DirectMlRuntime>&
OrtSessionConfiguration::directMlRuntime() const noexcept
{
    return directMlRuntime_;
}
}
