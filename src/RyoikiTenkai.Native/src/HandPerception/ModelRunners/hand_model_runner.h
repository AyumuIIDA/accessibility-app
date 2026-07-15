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

class IHandModelRunner
{
public:
    virtual ~IHandModelRunner() = default;

    [[nodiscard]] virtual ExecutionProvider executionProvider() const noexcept = 0;
    [[nodiscard]] virtual std::string_view providerName() const noexcept = 0;
};
}
