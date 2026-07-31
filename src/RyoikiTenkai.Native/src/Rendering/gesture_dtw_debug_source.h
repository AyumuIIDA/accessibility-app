#pragma once
#include "Rendering/gesture_dtw_debug_render_packet.h"
#include <memory>
#include <mutex>

namespace ryoiki::rendering
{
class GestureDtwDebugSource final
{
public:
    void publish(GestureDtwDebugRenderPacket packet) noexcept;
    void publishLiveHand(std::uint64_t frameId,
        const std::array<GestureDtwDebugPoint, kGestureDtwDebugLandmarkCount>& landmarks) noexcept;
    [[nodiscard]] bool tryGet(GestureDtwDebugRenderPacket& packet,
        std::uint64_t& revision) const noexcept;
    void clear() noexcept;
private:
    mutable std::mutex mutex_;
    GestureDtwDebugRenderPacket latest_{};
    bool hasValue_{false};
    std::array<GestureDtwDebugPoint, kGestureDtwDebugLandmarkCount> liveLandmarks_{};
    std::uint64_t liveFrameId_{0};
    std::uint64_t revision_{0};
};
}

struct RyoikiHandle;
[[nodiscard]] std::shared_ptr<ryoiki::rendering::GestureDtwDebugSource>
ryoikiGestureDtwDebugSource(RyoikiHandle* handle) noexcept;
