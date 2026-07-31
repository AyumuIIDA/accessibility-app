#include "Rendering/gesture_dtw_debug_render_packet.h"

#include <cmath>

namespace ryoiki::rendering
{
namespace
{
bool finite(const GestureDtwDebugPoint& point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

bool finite(const GestureDtwDebugSequenceFrame& frame) noexcept
{
    if (!std::isfinite(frame.timeOffsetMs)
        || !std::isfinite(frame.palmCenterX)
        || !std::isfinite(frame.palmCenterY)
        || !std::isfinite(frame.palmOrientationRadians)
        || !std::isfinite(frame.palmVelocity))
    {
        return false;
    }
    for (const auto& landmark : frame.landmarks)
    {
        if (!finite(landmark)) return false;
    }
    return true;
}

template <typename Frames>
bool validateFrames(const Frames& frames, const std::size_t count) noexcept
{
    double previousTime = -1.0;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (!finite(frames[index]) || frames[index].timeOffsetMs < 0.0
            || (index > 0 && frames[index].timeOffsetMs < previousTime))
        {
            return false;
        }
        previousTime = frames[index].timeOffsetMs;
    }
    return true;
}
}

bool validateGestureDtwDebugRenderPacket(
    const GestureDtwDebugRenderPacket& packet) noexcept
{
    if (packet.candidateFrameCount > kGestureDtwDebugFrameCapacity
        || packet.templateFrameCount > kGestureDtwDebugFrameCapacity
        || packet.pathCount > kGestureDtwDebugPathCapacity
        || !std::isfinite(packet.score)
        || !std::isfinite(packet.confidence)
        || packet.confidence < 0.0F || packet.confidence > 1.0F)
    {
        return false;
    }
    for (const auto score : packet.scoreBreakdown)
    {
        if (!std::isfinite(score)) return false;
    }
    if (packet.liveHandValid)
    {
        if (!std::isfinite(packet.livePalmCenterX) || !std::isfinite(packet.livePalmCenterY)) return false;
        for (const auto& point : packet.liveLandmarks) if (!finite(point)) return false;
    }
    if (!validateFrames(packet.candidateFrames, packet.candidateFrameCount)
        || !validateFrames(packet.templateFrames, packet.templateFrameCount))
    {
        return false;
    }
    if (packet.pathCount > 0
        && (packet.candidateFrameCount == 0 || packet.templateFrameCount == 0))
    {
        return false;
    }
    for (std::size_t index = 0; index < packet.pathCount; ++index)
    {
        const auto point = packet.path[index];
        if (point.candidateIndex >= packet.candidateFrameCount
            || point.templateIndex >= packet.templateFrameCount)
        {
            return false;
        }
        if (index > 0)
        {
            const auto previous = packet.path[index - 1];
            const auto candidateStep = static_cast<int>(point.candidateIndex)
                - static_cast<int>(previous.candidateIndex);
            const auto templateStep = static_cast<int>(point.templateIndex)
                - static_cast<int>(previous.templateIndex);
            if (candidateStep < 0 || templateStep < 0
                || candidateStep > 1 || templateStep > 1
                || (candidateStep == 0 && templateStep == 0))
            {
                return false;
            }
        }
    }
    return true;
}
}
