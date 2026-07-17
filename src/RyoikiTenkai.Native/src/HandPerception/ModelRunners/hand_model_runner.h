#pragma once

#include <string_view>

namespace ryoiki::hand_perception
{
enum class ExecutionProvider
{
    Cpu,
    DirectMl,
    QnnGpu,
    QnnHtp
};

struct ModelRunnerExecutionSettings
{
    bool preferQnnHtp{false};
    bool requireQnnHtp{false};
};

class IHandModelRunner
{
public:
    virtual ~IHandModelRunner() = default;

    [[nodiscard]] virtual ExecutionProvider executionProvider() const noexcept = 0;
    [[nodiscard]] virtual std::string_view providerName() const noexcept = 0;
    [[nodiscard]] virtual std::string_view fallbackReason() const noexcept
    {
        return {};
    }
};
}
