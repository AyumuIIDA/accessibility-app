#include "Pipeline/latest_frame_slot.h"

#include <utility>

namespace ryoiki::pipeline
{
void LatestFrameSlot::publish(std::shared_ptr<const buffers::FrameBuffer> frame)
{
    std::lock_guard lock{mutex_};
    frame_ = std::move(frame);
}

std::shared_ptr<const buffers::FrameBuffer> LatestFrameSlot::snapshot() const
{
    std::lock_guard lock{mutex_};
    return frame_;
}

void LatestFrameSlot::clear()
{
    std::lock_guard lock{mutex_};
    frame_.reset();
}
}
