#include "Buffers/frame_buffer.h"

#include <limits>

namespace ryoiki::buffers
{
bool FrameBuffer::prepare(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint64_t frameId,
    const std::uint64_t captureTimestampUs,
    const runtime::FrameRotation orientation)
{
    constexpr std::uint64_t kBytesPerPixel = 4;
    const auto stride = static_cast<std::uint64_t>(width) * kBytesPerPixel;
    const auto byteCount = stride * height;
    if (width == 0 || height == 0
        || stride > std::numeric_limits<std::uint32_t>::max()
        || byteCount > std::numeric_limits<std::size_t>::max())
    {
        return false;
    }

    clearGpuResource();
    pixels_.resize(static_cast<std::size_t>(byteCount));
    width_ = width;
    height_ = height;
    stride_ = static_cast<std::uint32_t>(stride);
    frameId_ = frameId;
    captureTimestampUs_ = captureTimestampUs;
    orientation_ = orientation;
    return true;
}

bool FrameBuffer::prepareGpu(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint64_t frameId,
    const std::uint64_t captureTimestampUs,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    const std::uint32_t textureSubresource,
    Microsoft::WRL::ComPtr<IUnknown> sampleOwner,
    const runtime::FrameRotation orientation)
{
    if (width == 0 || height == 0 || texture == nullptr)
    {
        return false;
    }
    pixels_.clear();
    gpuTexture_ = std::move(texture);
    gpuSampleOwner_ = std::move(sampleOwner);
    memoryLocation_ = MemoryLocation::Gpu;
    gpuTextureSubresource_ = textureSubresource;
    width_ = width;
    height_ = height;
    stride_ = width * 4U;
    frameId_ = frameId;
    captureTimestampUs_ = captureTimestampUs;
    orientation_ = orientation;
    return true;
}

std::uint32_t FrameBuffer::width() const noexcept { return width_; }
std::uint32_t FrameBuffer::height() const noexcept { return height_; }
std::uint32_t FrameBuffer::uprightWidth() const noexcept
{
    return runtime::orientedSize(width_, height_, orientation_).width;
}
std::uint32_t FrameBuffer::uprightHeight() const noexcept
{
    return runtime::orientedSize(width_, height_, orientation_).height;
}
std::uint32_t FrameBuffer::stride() const noexcept { return stride_; }
std::uint64_t FrameBuffer::frameId() const noexcept { return frameId_; }
std::uint64_t FrameBuffer::captureTimestampUs() const noexcept { return captureTimestampUs_; }
PixelFormat FrameBuffer::pixelFormat() const noexcept { return PixelFormat::Bgra32; }
MemoryLocation FrameBuffer::memoryLocation() const noexcept { return memoryLocation_; }
runtime::FrameRotation FrameBuffer::orientation() const noexcept { return orientation_; }
const std::vector<std::uint8_t>& FrameBuffer::pixels() const noexcept { return pixels_; }
std::vector<std::uint8_t>& FrameBuffer::writablePixels() noexcept { return pixels_; }
ID3D11Texture2D* FrameBuffer::gpuTexture() const noexcept { return gpuTexture_.Get(); }
std::uint32_t FrameBuffer::gpuTextureSubresource() const noexcept { return gpuTextureSubresource_; }
std::mutex& FrameBuffer::gpuAccessMutex() const noexcept { return *gpuAccessMutex_; }
bool FrameBuffer::attachGpuResource(
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    const std::uint32_t textureSubresource,
    Microsoft::WRL::ComPtr<IUnknown> sampleOwner)
{
    if (texture == nullptr)
    {
        return false;
    }
    gpuTexture_ = std::move(texture);
    gpuSampleOwner_ = std::move(sampleOwner);
    gpuTextureSubresource_ = textureSubresource;
    memoryLocation_ = pixels_.empty() ? MemoryLocation::Gpu : MemoryLocation::Shared;
    return true;
}
void FrameBuffer::clearGpuResource() noexcept
{
    gpuSampleOwner_.Reset();
    gpuTexture_.Reset();
    gpuTextureSubresource_ = 0;
    memoryLocation_ = MemoryLocation::Cpu;
}
}
