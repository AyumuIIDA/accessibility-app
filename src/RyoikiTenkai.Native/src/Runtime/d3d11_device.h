#pragma once

#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <memory>
#include <mutex>
#include <string>

namespace ryoiki::runtime
{
class D3d11Device final
{
public:
    [[nodiscard]] static std::shared_ptr<D3d11Device> create(std::string& error);
    [[nodiscard]] static std::shared_ptr<D3d11Device> createOn12(std::string& error);

    [[nodiscard]] ID3D11Device* device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* immediateContext() const noexcept;
    [[nodiscard]] std::mutex& immediateContextMutex() noexcept;
    [[nodiscard]] bool isD3d11On12() const noexcept;
    [[nodiscard]] ID3D12Device* d3d12Device() const noexcept;
    [[nodiscard]] ID3D12CommandQueue* d3d12CommandQueue() const noexcept;
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Resource> unwrapTexture(
        ID3D11Texture2D* texture,
        std::string& error,
        ID3D12CommandQueue* commandQueue = nullptr) const;
    bool returnTexture(
        ID3D11Texture2D* texture,
        ID3D12Fence* fence,
        std::uint64_t fenceValue,
        std::string& error) const;
    bool validateUnwrappedTexturePixels(
        ID3D12Resource* resource, std::string& diagnostic) const;

private:
    D3d11Device(
        Microsoft::WRL::ComPtr<ID3D11Device> device,
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context,
        Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device = {},
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12Queue = {},
        Microsoft::WRL::ComPtr<ID3D11On12Device2> on12Device = {});

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12Queue_;
    Microsoft::WRL::ComPtr<ID3D11On12Device2> on12Device_;
    std::mutex contextMutex_;
};
}
