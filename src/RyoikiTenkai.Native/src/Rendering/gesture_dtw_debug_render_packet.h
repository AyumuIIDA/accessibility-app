#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ryoiki::rendering
{
inline constexpr std::size_t kGestureDtwDebugFrameCapacity = 32;
inline constexpr std::size_t kGestureDtwDebugLandmarkCount = 21;
inline constexpr std::size_t kGestureDtwDebugPathCapacity = 64;
inline constexpr std::size_t kGestureDtwDebugScoreCount = 12;

struct GestureDtwDebugPoint
{
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

struct GestureDtwDebugSequenceFrame
{
    double timeOffsetMs{0.0};
    float palmCenterX{0.0F};
    float palmCenterY{0.0F};
    float palmOrientationRadians{0.0F};
    float palmVelocity{0.0F};
    std::array<GestureDtwDebugPoint, kGestureDtwDebugLandmarkCount> landmarks{};
};

struct GestureDtwDebugPathPoint
{
    std::uint8_t candidateIndex{0};
    std::uint8_t templateIndex{0};
};

// Rendering-owned, bounded copy of recognition diagnostics. It intentionally
// carries no pointers to registry/history storage and can cross to a render
// worker by value without extending recognition-object lifetimes.
struct GestureDtwDebugRenderPacket
{
    std::uint64_t frameId{0};
    std::uint64_t timestampUs{0};
    std::uint32_t trackId{0};
    std::uint32_t templateId{0};
    std::uint32_t candidateFrameCount{0};
    std::uint32_t templateFrameCount{0};
    std::uint32_t pathCount{0};
    bool eligible{false};
    float score{0.0F};
    float confidence{0.0F};
    bool liveHandValid{false};
    std::uint64_t liveHandFrameId{0};
    float livePalmCenterX{0.0F};
    float livePalmCenterY{0.0F};
    std::array<GestureDtwDebugPoint, kGestureDtwDebugLandmarkCount> liveLandmarks{};
    std::array<float, kGestureDtwDebugScoreCount> scoreBreakdown{};
    std::array<GestureDtwDebugSequenceFrame, kGestureDtwDebugFrameCapacity> candidateFrames{};
    std::array<GestureDtwDebugSequenceFrame, kGestureDtwDebugFrameCapacity> templateFrames{};
    std::array<GestureDtwDebugPathPoint, kGestureDtwDebugPathCapacity> path{};
};

[[nodiscard]] bool validateGestureDtwDebugRenderPacket(
    const GestureDtwDebugRenderPacket& packet) noexcept;
}
