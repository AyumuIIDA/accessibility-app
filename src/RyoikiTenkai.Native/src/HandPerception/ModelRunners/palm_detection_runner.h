#pragma once

#include "Buffers/tensor_buffer.h"
#include "HandPerception/ModelRunners/hand_model_runner.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace ryoiki::hand_perception
{
class PalmDetectionRawOutput final
{
public:
    static constexpr std::size_t kAnchorCount = 2016;
    static constexpr std::size_t kRegressionCount = 18;

    [[nodiscard]] std::span<float> regressions() noexcept;
    [[nodiscard]] std::span<const float> regressions() const noexcept;
    [[nodiscard]] std::span<float> scores() noexcept;
    [[nodiscard]] std::span<const float> scores() const noexcept;

private:
    std::array<float, kAnchorCount * kRegressionCount> regressions_{};
    std::array<float, kAnchorCount> scores_{};
};

struct ModelRunResult
{
    double inferenceMs{0.0};
};

class IPalmDetectionRunner : public IHandModelRunner
{
public:
    ~IPalmDetectionRunner() override = default;

    virtual bool run(
        const buffers::FloatTensorBuffer& input,
        PalmDetectionRawOutput& output,
        ModelRunResult& result,
        std::string& error) = 0;
};
}
