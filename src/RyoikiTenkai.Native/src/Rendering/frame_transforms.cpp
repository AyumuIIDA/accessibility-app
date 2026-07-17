#include "Rendering/frame_transforms.h"

#include <algorithm>

namespace ryoiki::rendering
{
bool createFrameTransforms(
    const std::uint32_t storageWidth,
    const std::uint32_t storageHeight,
    const runtime::FrameRotation rotation,
    const std::uint32_t viewportWidth,
    const std::uint32_t viewportHeight,
    const bool mirrorHorizontally,
    FrameTransforms& result) noexcept
{
    if (storageWidth == 0 || storageHeight == 0
        || viewportWidth == 0 || viewportHeight == 0)
    {
        return false;
    }

    result.storageToUpright = geometry::createStorageToUprightTransform(
        storageWidth, storageHeight, rotation);

    const auto uprightSize = runtime::orientedSize(storageWidth, storageHeight, rotation);
    const float uprightWidth = static_cast<float>(uprightSize.width);
    const float uprightHeight = static_cast<float>(uprightSize.height);
    const float scale = (std::min)(
        static_cast<float>(viewportWidth) / uprightWidth,
        static_cast<float>(viewportHeight) / uprightHeight);
    result.contentWidth = uprightWidth * scale;
    result.contentHeight = uprightHeight * scale;
    result.contentLeft = (static_cast<float>(viewportWidth) - result.contentWidth) * 0.5F;
    result.contentTop = (static_cast<float>(viewportHeight) - result.contentHeight) * 0.5F;

    result.uprightToViewport = mirrorHorizontally
        ? geometry::AffineTransform{{-scale, 0.0F,
            result.contentLeft + result.contentWidth, 0.0F, scale, result.contentTop}}
        : geometry::AffineTransform{{scale, 0.0F,
            result.contentLeft, 0.0F, scale, result.contentTop}};
    result.storageToViewport = geometry::compose(
        result.storageToUpright,
        result.uprightToViewport);
    return geometry::invert(result.uprightToViewport, result.viewportToUpright)
        && geometry::invert(result.storageToUpright, result.uprightToStorage);
}
}
