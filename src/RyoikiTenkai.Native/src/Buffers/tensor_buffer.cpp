#include "Buffers/tensor_buffer.h"

#include <stdexcept>

namespace ryoiki::buffers
{
namespace
{
std::size_t calculateElementCount(const std::array<std::int64_t, 4>& shape)
{
    std::size_t count = 1;
    for (const auto dimension : shape)
    {
        if (dimension <= 0)
        {
            throw std::invalid_argument{"Tensor dimensions must be positive."};
        }
        count *= static_cast<std::size_t>(dimension);
    }
    return count;
}
}

FloatTensorBuffer::FloatTensorBuffer(const std::array<std::int64_t, 4> shape)
    : shape_{shape}, values_(calculateElementCount(shape))
{
}

const std::array<std::int64_t, 4>& FloatTensorBuffer::shape() const noexcept { return shape_; }
std::size_t FloatTensorBuffer::elementCount() const noexcept { return values_.size(); }
float* FloatTensorBuffer::data() noexcept { return values_.data(); }
const float* FloatTensorBuffer::data() const noexcept { return values_.data(); }
}
