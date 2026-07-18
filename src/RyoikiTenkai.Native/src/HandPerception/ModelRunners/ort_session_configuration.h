#pragma once

#include "HandPerception/ModelRunners/hand_model_runner.h"

#include <onnxruntime_cxx_api.h>

#include <string>
#include <unordered_map>
#include <memory>

namespace ryoiki::runtime { class DirectMlRuntime; }

namespace ryoiki::hand_perception
{
class OrtSessionConfiguration final
{
public:
    static OrtSessionConfiguration cpu();
    static OrtSessionConfiguration directMl(
        std::shared_ptr<runtime::DirectMlRuntime> runtime = {});
    static OrtSessionConfiguration qnnHtp();

    [[nodiscard]] ExecutionProvider executionProvider() const noexcept;
    [[nodiscard]] const std::string& providerName() const noexcept;
    void apply(Ort::SessionOptions& sessionOptions) const;

private:
    OrtSessionConfiguration(
        ExecutionProvider executionProvider,
        std::string providerName,
        std::unordered_map<std::string, std::string> providerOptions,
        bool disableCpuFallback);

    ExecutionProvider executionProvider_{ExecutionProvider::Cpu};
    std::string providerName_;
    std::unordered_map<std::string, std::string> providerOptions_;
    bool disableCpuFallback_{false};
    std::shared_ptr<runtime::DirectMlRuntime> directMlRuntime_;

public:
    [[nodiscard]] const std::shared_ptr<runtime::DirectMlRuntime>& directMlRuntime() const noexcept;
};
}
