#include "HandInput/Publication/latest_hand_state_slot.h"

namespace ryoiki::hand_input::publication
{
void LatestHandStateSlot::publish(const HandStateSnapshot& snapshot) noexcept
{
    std::lock_guard lock{mutex_};
    snapshot_ = snapshot;
}

HandStateSnapshot LatestHandStateSlot::latest() const noexcept
{
    std::lock_guard lock{mutex_};
    return snapshot_;
}

void LatestHandStateSlot::reset() noexcept
{
    std::lock_guard lock{mutex_};
    snapshot_ = {};
}
}
