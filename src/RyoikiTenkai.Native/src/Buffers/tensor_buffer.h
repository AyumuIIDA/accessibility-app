#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ryoiki::buffers
{
class FloatTensorBuffer final
{
public:
    explicit FloatTensorBuffer(std::array<std::int64_t, 4> shape);

    [[nodiscard]] const std::array<std::int64_t, 4>& shape() const noexcept;
    [[nodiscard]] std::size_t elementCount() const noexcept;
    [[nodiscard]] float* data() noexcept;
    [[nodiscard]] const float* data() const noexcept;

private:
    std::array<std::int64_t, 4> shape_;
    std::vector<float> values_;
};
}
