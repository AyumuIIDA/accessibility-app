#pragma once

#include "Buffers/frame_buffer.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace ryoiki::buffers
{
class FramePool final
{
public:
    explicit FramePool(std::size_t capacity);

    [[nodiscard]] std::shared_ptr<FrameBuffer> tryAcquire();
    void resetStatistics() noexcept;
    [[nodiscard]] std::uint64_t droppedAcquisitions() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    std::vector<std::shared_ptr<FrameBuffer>> buffers_;
    std::atomic<std::uint64_t> droppedAcquisitions_{0};
};
}
