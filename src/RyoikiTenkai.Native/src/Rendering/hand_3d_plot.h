#pragma once

#include "Geometry/geometry_types.h"
#include "HandPerception/MediaPipeGraph/hand_landmark_graph.h"

#include <array>
#include <cstdint>

namespace ryoiki::rendering
{
struct PlotViewport
{
    float left{0.0F};
    float top{0.0F};
    float right{0.0F};
    float bottom{0.0F};

    [[nodiscard]] float width() const noexcept { return right - left; }
    [[nodiscard]] float height() const noexcept { return bottom - top; }
};

struct Hand3dView
{
    float yawRadians{0.0F};
    float pitchRadians{0.0F};
    float halfExtent{0.12F};
    bool mirrorHorizontally{true};
};

struct ProjectedHandPoint
{
    geometry::Point2f position;
    float depth{0.0F};
    bool visible{false};
};

struct Hand3dProjection
{
    std::array<ProjectedHandPoint, 21> points{};
    hand_perception::Landmark3f origin{};
    ProjectedHandPoint palmCenter{};
    ProjectedHandPoint palmDirectionTip{};
    bool hasPalmDirection{false};
    bool valid{false};
};

[[nodiscard]] ProjectedHandPoint projectHand3dPoint(
    const hand_perception::Landmark3f& pointRelativeToWrist,
    const PlotViewport& viewport,
    const Hand3dView& view) noexcept;

[[nodiscard]] Hand3dProjection projectHand3d(
    const hand_perception::HandLandmarkResult& hand,
    const PlotViewport& viewport,
    const Hand3dView& view) noexcept;
}
