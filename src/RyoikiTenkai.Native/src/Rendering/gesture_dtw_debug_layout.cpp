#include "Rendering/gesture_dtw_debug_layout.h"

#include <algorithm>
#include <cmath>

namespace ryoiki::rendering
{
GestureDtwDebugLayout createGestureDtwDebugLayout(
    const float width,
    const float height) noexcept
{
    GestureDtwDebugLayout result{};
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0F || height <= 0.0F)
    {
        return result;
    }

    constexpr float kPadding = 16.0F;
    constexpr float kGap = 12.0F;
    if (width <= (kPadding * 2.0F) + kGap || height <= (kPadding * 2.0F) + kGap)
    {
        return result;
    }

    const float contentLeft = kPadding;
    const float contentTop = kPadding;
    const float contentRight = width - kPadding;
    const float contentBottom = height - kPadding;
    const float contentHeight = contentBottom - contentTop;
    const float diagnosticsHeight = std::clamp(contentHeight * 0.32F, 96.0F, 240.0F);
    const float upperBottom = contentBottom - diagnosticsHeight - kGap;
    if (upperBottom <= contentTop) return result;

    const float middle = (contentLeft + contentRight) * 0.5F;
    result.candidate = {contentLeft, contentTop, middle - (kGap * 0.5F), upperBottom};
    result.templateView = {middle + (kGap * 0.5F), contentTop, contentRight, upperBottom};
    result.diagnostics = {contentLeft, upperBottom + kGap, contentRight, contentBottom};
    result.valid = result.candidate.width() > 0.0F && result.candidate.height() > 0.0F
        && result.templateView.width() > 0.0F && result.templateView.height() > 0.0F
        && result.diagnostics.width() > 0.0F && result.diagnostics.height() > 0.0F;
    return result;
}
}
