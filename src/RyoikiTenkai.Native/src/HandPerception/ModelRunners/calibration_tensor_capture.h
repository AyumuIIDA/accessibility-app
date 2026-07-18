#pragma once

#include "Buffers/tensor_buffer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace ryoiki::hand_perception
{
class CalibrationTensorCapture final
{
public:
    CalibrationTensorCapture(
        std::string_view modelName,
        std::array<std::int64_t, 4> shape) noexcept;

    void capture(const buffers::FloatTensorBuffer& tensor) noexcept;

private:
    std::filesystem::path outputDirectory_;
    std::array<std::int64_t, 4> shape_{};
    std::size_t limit_{0};
    std::size_t captured_{0};
    std::uint32_t processId_{0};
};
}
