#pragma once

#include "Buffers/frame_buffer.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ryoiki::pipeline
{
class PerceptionMailbox final
{
public:
    void publish(std::shared_ptr<const buffers::FrameBuffer> frame);
    [[nodiscard]] std::shared_ptr<const buffers::FrameBuffer> waitForLatest();
    void reset();
    void stop();
    [[nodiscard]] std::uint64_t droppedFrames() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable frameAvailable_;
    std::shared_ptr<const buffers::FrameBuffer> pending_;
    std::uint64_t droppedFrames_{0};
    bool stopped_{true};
};
}
