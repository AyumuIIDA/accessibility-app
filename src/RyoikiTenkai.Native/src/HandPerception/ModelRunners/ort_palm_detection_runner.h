#pragma once

#include "HandPerception/ModelRunners/ort_session_configuration.h"
#include "HandPerception/ModelRunners/palm_detection_runner.h"

#include <filesystem>
#include <memory>
#include <string>

namespace ryoiki::hand_perception
{
class OrtPalmDetectionRunner final : public IPalmDetectionRunner
{
public:
    static std::unique_ptr<OrtPalmDetectionRunner> create(
        const std::filesystem::path& modelPath,
        std::string& error);
    static std::unique_ptr<OrtPalmDetectionRunner> create(
        const std::filesystem::path& modelPath,
        const OrtSessionConfiguration& configuration,
        std::string& error);

    ~OrtPalmDetectionRunner() override;

    [[nodiscard]] ExecutionProvider executionProvider() const noexcept override;
    [[nodiscard]] std::string_view providerName() const noexcept override;
    bool run(
        const buffers::FloatTensorBuffer& input,
        PalmDetectionRawOutput& output,
        ModelRunResult& result,
        std::string& error) override;

private:
    struct Impl;
    explicit OrtPalmDetectionRunner(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};
}
