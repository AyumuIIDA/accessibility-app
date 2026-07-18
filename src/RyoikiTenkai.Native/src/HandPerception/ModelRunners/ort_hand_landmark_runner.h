#pragma once

#include "HandPerception/ModelRunners/hand_landmark_runner.h"
#include "HandPerception/ModelRunners/hand_landmark_model_contract.h"
#include "HandPerception/ModelRunners/ort_session_configuration.h"

#include <filesystem>
#include <memory>
#include <string>

namespace ryoiki::hand_perception
{
class OrtHandLandmarkRunner final : public IHandLandmarkRunner
{
public:
    static std::unique_ptr<OrtHandLandmarkRunner> create(
        const std::filesystem::path& modelPath,
        std::string& error);
    static std::unique_ptr<OrtHandLandmarkRunner> create(
        const std::filesystem::path& modelPath,
        const HandLandmarkModelContract& contract,
        std::string& error);
    static std::unique_ptr<OrtHandLandmarkRunner> create(
        const std::filesystem::path& modelPath,
        const HandLandmarkModelContract& contract,
        const OrtSessionConfiguration& configuration,
        std::string& error);

    ~OrtHandLandmarkRunner() override;

    [[nodiscard]] ExecutionProvider executionProvider() const noexcept override;
    [[nodiscard]] std::string_view providerName() const noexcept override;
    bool run(
        const buffers::FloatTensorBuffer& input,
        HandLandmarkRawOutput& output,
        ModelRunResult& result,
        std::string& error) override;

private:
    struct Impl;
    explicit OrtHandLandmarkRunner(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};
}
