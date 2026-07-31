#pragma once

namespace ryoiki::rendering
{
struct GestureDtwDebugRect
{
    float left{0.0F};
    float top{0.0F};
    float right{0.0F};
    float bottom{0.0F};

    [[nodiscard]] float width() const noexcept { return right - left; }
    [[nodiscard]] float height() const noexcept { return bottom - top; }
};

struct GestureDtwDebugLayout
{
    GestureDtwDebugRect candidate;
    GestureDtwDebugRect templateView;
    GestureDtwDebugRect diagnostics;
    bool valid{false};
};

// Two equally sized skeleton panes above one full-width trajectory/score pane.
// Values are device-independent pixels and therefore usable by D2D and tests.
[[nodiscard]] GestureDtwDebugLayout createGestureDtwDebugLayout(
    float width,
    float height) noexcept;
}
