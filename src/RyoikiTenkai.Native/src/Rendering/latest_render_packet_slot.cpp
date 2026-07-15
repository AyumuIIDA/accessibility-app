#include "Rendering/latest_render_packet_slot.h"

#include <utility>

namespace ryoiki::rendering
{
void LatestRenderPacketSlot::publish(RenderPacket packet)
{
    std::lock_guard lock{mutex_};
    packet_ = std::move(packet);
}

std::optional<RenderPacket> LatestRenderPacketSlot::snapshot() const
{
    std::lock_guard lock{mutex_};
    return packet_;
}

void LatestRenderPacketSlot::clear()
{
    std::lock_guard lock{mutex_};
    packet_.reset();
}
}
