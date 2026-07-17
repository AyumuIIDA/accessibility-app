#include "Runtime/frame_orientation.h"

#include <cstring>
#include <limits>

namespace ryoiki::runtime
{
namespace
{
constexpr std::size_t kBytesPerPixel = 4;

bool checkedImageBytes(
    const std::uint32_t width,
    const std::uint32_t height,
    std::size_t& byteCount)
{
    if (width == 0 || height == 0)
    {
        return false;
    }

    const std::uint64_t bytes = static_cast<std::uint64_t>(width)
        * static_cast<std::uint64_t>(height) * kBytesPerPixel;
    if (bytes > std::numeric_limits<std::size_t>::max())
    {
        return false;
    }

    byteCount = static_cast<std::size_t>(bytes);
    return true;
}
}

bool copyStorageBgra32(
    const std::uint8_t* const sourceScanline0,
    const std::ptrdiff_t sourceStride,
    const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight,
    std::vector<std::uint8_t>& destination)
{
    if (sourceScanline0 == nullptr || sourceStride == 0
        || sourceStride == (std::numeric_limits<std::ptrdiff_t>::min)())
    {
        return false;
    }
    const auto strideMagnitude = static_cast<std::size_t>(
        sourceStride < 0 ? -sourceStride : sourceStride);
    const auto rowBytes64 = static_cast<std::uint64_t>(sourceWidth) * kBytesPerPixel;
    std::size_t byteCount = 0;
    if (rowBytes64 > strideMagnitude
        || !checkedImageBytes(sourceWidth, sourceHeight, byteCount))
    {
        return false;
    }

    destination.resize(byteCount);
    const auto rowBytes = static_cast<std::size_t>(rowBytes64);
    for (std::uint32_t y = 0; y < sourceHeight; ++y)
    {
        std::memcpy(
            destination.data() + static_cast<std::size_t>(y) * rowBytes,
            sourceScanline0 + static_cast<std::ptrdiff_t>(y) * sourceStride,
            rowBytes);
    }
    return true;
}

}
