#pragma once

#include "HandPerception/ModelRunners/hand_landmark_runner.h"

#include <filesystem>
#include <memory>
#include <string>

namespace ryoiki::hand_perception
{
class CpuHandLandmarkRunner final : public IHandLandmarkRunner
{
public:
    static std::unique_ptr<CpuHandLandmarkRunner> create(
        const std::filesystem::path& modelPath,
        std::string& error);

    ~CpuHandLandmarkRunner() override;

    [[nodiscard]] ExecutionProvider executionProvider() const noexcept override;
    [[nodiscard]] std::string_view providerName() const noexcept override;
    bool run(
        const buffers::FloatTensorBuffer& input,
        HandLandmarkRawOutput& output,
        ModelRunResult& result,
        std::string& error) override;

private:
    struct Impl;
    explicit CpuHandLandmarkRunner(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};
}
