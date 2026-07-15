#include "Buffers/frame_pool.h"

namespace ryoiki::buffers
{
FramePool::FramePool(const std::size_t capacity)
{
    buffers_.reserve(capacity);
    for (std::size_t index = 0; index < capacity; ++index)
    {
        buffers_.push_back(std::make_shared<FrameBuffer>());
    }
}

std::shared_ptr<FrameBuffer> FramePool::tryAcquire()
{
    for (const auto& buffer : buffers_)
    {
        if (buffer.use_count() == 1)
        {
            return buffer;
        }
    }

    droppedAcquisitions_.fetch_add(1, std::memory_order_relaxed);
    return {};
}

void FramePool::resetStatistics() noexcept
{
    droppedAcquisitions_.store(0, std::memory_order_relaxed);
}

std::uint64_t FramePool::droppedAcquisitions() const noexcept
{
    return droppedAcquisitions_.load(std::memory_order_relaxed);
}

std::size_t FramePool::capacity() const noexcept
{
    return buffers_.size();
}
}
