#include "Rendering/gesture_dtw_debug_source.h"
#include <cmath>
#include <array>
#include <utility>
namespace ryoiki::rendering
{
void GestureDtwDebugSource::publish(GestureDtwDebugRenderPacket packet) noexcept
{ if(!validateGestureDtwDebugRenderPacket(packet))return;std::lock_guard lock{mutex_};latest_=std::move(packet);hasValue_=true;++revision_; }
void GestureDtwDebugSource::publishLiveHand(const std::uint64_t frameId,
    const std::array<GestureDtwDebugPoint, kGestureDtwDebugLandmarkCount>& landmarks) noexcept
{ for(const auto& point:landmarks)if(!std::isfinite(point.x)||!std::isfinite(point.y)||!std::isfinite(point.z))return;std::lock_guard lock{mutex_};liveFrameId_=frameId;liveLandmarks_=landmarks;++revision_; }
bool GestureDtwDebugSource::tryGet(GestureDtwDebugRenderPacket& packet,std::uint64_t& revision) const noexcept
{ std::lock_guard lock{mutex_};if(!hasValue_)return false;packet=latest_;if(liveFrameId_>0){packet.liveHandValid=true;packet.liveHandFrameId=liveFrameId_;packet.liveLandmarks=liveLandmarks_;constexpr std::array<std::size_t,5> palm{0,5,9,13,17};for(const auto index:palm){packet.livePalmCenterX+=liveLandmarks_[index].x;packet.livePalmCenterY+=liveLandmarks_[index].y;}packet.livePalmCenterX/=palm.size();packet.livePalmCenterY/=palm.size();}revision=revision_;return true; }
void GestureDtwDebugSource::clear() noexcept { std::lock_guard lock{mutex_};latest_={};hasValue_=false;liveFrameId_=0;revision_=0; }
}
