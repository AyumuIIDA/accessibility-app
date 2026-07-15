#pragma once

#include <cstdint>
#include <vector>

namespace ryoiki::buffers
{
enum class MemoryLocation
{
    Cpu
};

enum class PixelFormat
{
    Bgra32
};

class FrameBuffer final
{
public:
    bool prepare(
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t frameId,
        std::uint64_t captureTimestampUs);

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::uint32_t stride() const noexcept;
    [[nodiscard]] std::uint64_t frameId() const noexcept;
    [[nodiscard]] std::uint64_t captureTimestampUs() const noexcept;
    [[nodiscard]] PixelFormat pixelFormat() const noexcept;
    [[nodiscard]] MemoryLocation memoryLocation() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t>& writablePixels() noexcept;

private:
    std::vector<std::uint8_t> pixels_;
    std::uint32_t width_{0};
    std::uint32_t height_{0};
    std::uint32_t stride_{0};
    std::uint64_t frameId_{0};
    std::uint64_t captureTimestampUs_{0};
};
}
