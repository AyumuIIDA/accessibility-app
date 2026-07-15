#pragma once

#include "Buffers/frame_buffer.h"

#include <memory>
#include <mutex>

namespace ryoiki::pipeline
{
class LatestFrameSlot final
{
public:
    void publish(std::shared_ptr<const buffers::FrameBuffer> frame);
    [[nodiscard]] std::shared_ptr<const buffers::FrameBuffer> snapshot() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const buffers::FrameBuffer> frame_;
};
}
