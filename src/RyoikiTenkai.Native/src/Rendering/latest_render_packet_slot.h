#pragma once

#include "Rendering/render_packet.h"

#include <mutex>
#include <optional>

namespace ryoiki::rendering
{
class LatestRenderPacketSlot final
{
public:
    void publish(RenderPacket packet);
    [[nodiscard]] std::optional<RenderPacket> snapshot() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::optional<RenderPacket> packet_;
};
}
