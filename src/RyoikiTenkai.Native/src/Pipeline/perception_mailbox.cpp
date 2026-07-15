#include "Pipeline/perception_mailbox.h"

#include <utility>

namespace ryoiki::pipeline
{
void PerceptionMailbox::publish(std::shared_ptr<const buffers::FrameBuffer> frame)
{
    {
        std::lock_guard lock{mutex_};
        if (stopped_)
        {
            return;
        }
        if (pending_ != nullptr)
        {
            ++droppedFrames_;
        }
        pending_ = std::move(frame);
    }
    frameAvailable_.notify_one();
}

std::shared_ptr<const buffers::FrameBuffer> PerceptionMailbox::waitForLatest()
{
    std::unique_lock lock{mutex_};
    frameAvailable_.wait(lock, [this]
    {
        return stopped_ || pending_ != nullptr;
    });

    if (stopped_)
    {
        return {};
    }

    auto frame = std::move(pending_);
    pending_.reset();
    return frame;
}

void PerceptionMailbox::reset()
{
    std::lock_guard lock{mutex_};
    pending_.reset();
    droppedFrames_ = 0;
    stopped_ = false;
}

void PerceptionMailbox::stop()
{
    {
        std::lock_guard lock{mutex_};
        stopped_ = true;
        pending_.reset();
    }
    frameAvailable_.notify_all();
}

std::uint64_t PerceptionMailbox::droppedFrames() const
{
    std::lock_guard lock{mutex_};
    return droppedFrames_;
}
}
