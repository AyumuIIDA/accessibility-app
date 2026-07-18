#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <wrl/client.h>
#include <d3d12.h>

namespace ryoiki::buffers
{
class FloatTensorBuffer final
{
public:
    explicit FloatTensorBuffer(std::array<std::int64_t, 4> shape);

    [[nodiscard]] const std::array<std::int64_t, 4>& shape() const noexcept;
    [[nodiscard]] std::size_t elementCount() const noexcept;
    [[nodiscard]] float* data() noexcept;
    [[nodiscard]] const float* data() const noexcept;
    void setGpuResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource, std::uint64_t readyFenceValue) noexcept;
    void clearGpuResource() noexcept;
    [[nodiscard]] ID3D12Resource* gpuResource() const noexcept;
    [[nodiscard]] std::uint64_t readyFenceValue() const noexcept;

private:
    std::array<std::int64_t, 4> shape_;
    std::vector<float> values_;
    Microsoft::WRL::ComPtr<ID3D12Resource> gpuResource_;
    std::uint64_t readyFenceValue_{0};
};
}
