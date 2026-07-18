#pragma once

#include "Runtime/d3d11_device.h"

#include <DirectML.h>
#include <d3d12.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ryoiki::runtime
{
class DirectMlRuntime final
{
public:
    static std::shared_ptr<DirectMlRuntime> create(
        const std::shared_ptr<D3d11Device>& d3d11Device,
        std::string& error);

    [[nodiscard]] IDMLDevice* dmlDevice() const noexcept;
    [[nodiscard]] ID3D12Device* d3d12Device() const noexcept;
    [[nodiscard]] ID3D12CommandQueue* commandQueue() const noexcept;
    [[nodiscard]] std::shared_ptr<D3d11Device> d3d11Device() const noexcept;
    [[nodiscard]] ID3D11Fence* d3d11Fence() const noexcept;
    [[nodiscard]] ID3D12Fence* d3d12Fence() const noexcept;
    [[nodiscard]] std::uint64_t nextFenceValue() noexcept;
    bool signal(std::uint64_t fenceValue, std::string& error) const;
    void waitForPreprocess(std::uint64_t fenceValue) const;
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Resource> tryOpenD3d11Texture(
        ID3D11Texture2D* texture,
        std::string& diagnostic) const;
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Resource> copyCameraToSharedTexture(
        ID3D11Texture2D* texture,
        std::uint64_t frameId,
        double& completedCopyMs,
        std::string& diagnostic);
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Resource> copySharedTensorTexture(
        ID3D12Resource* texture,
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t readyFenceValue,
        std::string& error);

private:
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<IDMLDevice> dmlDevice_;
    Microsoft::WRL::ComPtr<ID3D11Fence> d3d11Fence_;
    Microsoft::WRL::ComPtr<ID3D12Fence> d3d12Fence_;
    std::uint64_t fenceValue_{0};
    std::shared_ptr<D3d11Device> d3d11Device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedCameraTexture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> sharedCameraResource_;
    std::uint32_t sharedCameraWidth_{0};
    std::uint32_t sharedCameraHeight_{0};
    std::uint64_t sharedCameraFrameId_{0};
};
}
