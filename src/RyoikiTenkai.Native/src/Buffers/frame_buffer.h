#pragma once

#include "Runtime/frame_orientation.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ryoiki::buffers
{
enum class MemoryLocation
{
    Cpu,
    Gpu,
    Npu,
    Shared
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
        std::uint64_t captureTimestampUs,
        runtime::FrameRotation orientation = runtime::FrameRotation::None);
    bool prepareGpu(
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t frameId,
        std::uint64_t captureTimestampUs,
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
        std::uint32_t textureSubresource,
        Microsoft::WRL::ComPtr<IUnknown> sampleOwner,
        runtime::FrameRotation orientation = runtime::FrameRotation::None);

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::uint32_t uprightWidth() const noexcept;
    [[nodiscard]] std::uint32_t uprightHeight() const noexcept;
    [[nodiscard]] std::uint32_t stride() const noexcept;
    [[nodiscard]] std::uint64_t frameId() const noexcept;
    [[nodiscard]] std::uint64_t captureTimestampUs() const noexcept;
    [[nodiscard]] PixelFormat pixelFormat() const noexcept;
    [[nodiscard]] MemoryLocation memoryLocation() const noexcept;
    [[nodiscard]] runtime::FrameRotation orientation() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t>& writablePixels() noexcept;
    [[nodiscard]] ID3D11Texture2D* gpuTexture() const noexcept;
    [[nodiscard]] std::uint32_t gpuTextureSubresource() const noexcept;
    [[nodiscard]] std::mutex& gpuAccessMutex() const noexcept;
    bool attachGpuResource(
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
        std::uint32_t textureSubresource,
        Microsoft::WRL::ComPtr<IUnknown> sampleOwner);
    void clearGpuResource() noexcept;

private:
    std::vector<std::uint8_t> pixels_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> gpuTexture_;
    Microsoft::WRL::ComPtr<IUnknown> gpuSampleOwner_;
    MemoryLocation memoryLocation_{MemoryLocation::Cpu};
    std::uint32_t gpuTextureSubresource_{0};
    std::uint32_t width_{0};
    std::uint32_t height_{0};
    std::uint32_t stride_{0};
    std::uint64_t frameId_{0};
    std::uint64_t captureTimestampUs_{0};
    runtime::FrameRotation orientation_{runtime::FrameRotation::None};
    std::shared_ptr<std::mutex> gpuAccessMutex_{std::make_shared<std::mutex>()};
};
}
