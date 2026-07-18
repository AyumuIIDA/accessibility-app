#include "Runtime/d3d11_device.h"

#include <d3d10.h>
#include <wrl/client.h>
#include <array>
#include <iomanip>
#include <sstream>

namespace ryoiki::runtime
{
namespace
{
std::string hresultMessage(const char* operation, const HRESULT result)
{
    std::ostringstream stream;
    stream << operation << " failed with HRESULT 0x"
        << std::hex << static_cast<unsigned long>(result) << '.';
    return stream.str();
}
}

std::shared_ptr<D3d11Device> D3d11Device::create(std::string& error)
{
    constexpr std::array<D3D_FEATURE_LEVEL, 2> kFeatureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selectedFeatureLevel{};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    const HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        kFeatureLevels.data(),
        static_cast<UINT>(kFeatureLevels.size()),
        D3D11_SDK_VERSION,
        &device,
        &selectedFeatureLevel,
        &context);
    if (FAILED(result))
    {
        error = hresultMessage("D3D11CreateDevice", result);
        return {};
    }
    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (FAILED(context.As(&multithread)))
    {
        error = "The D3D11 immediate context does not expose ID3D10Multithread.";
        return {};
    }
    multithread->SetMultithreadProtected(TRUE);
    return std::shared_ptr<D3d11Device>{new D3d11Device{
        std::move(device), std::move(context)}};
}

std::shared_ptr<D3d11Device> D3d11Device::createOn12(std::string& error)
{
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device;
    HRESULT result = D3D12CreateDevice(
        nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device));
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    if (SUCCEEDED(result))
    {
        result = d3d12Device->CreateCommandQueue(
            &queueDescription, IID_PPV_ARGS(&queue));
    }
    constexpr std::array<D3D_FEATURE_LEVEL, 2> kFeatureLevels{
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    IUnknown* queues[]{queue.Get()};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL selected{};
    if (SUCCEEDED(result))
    {
        result = D3D11On12CreateDevice(
            d3d12Device.Get(),
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            kFeatureLevels.data(), static_cast<UINT>(kFeatureLevels.size()),
            queues, 1, 0, &device, &context, &selected);
    }
    Microsoft::WRL::ComPtr<ID3D11On12Device2> on12Device;
    if (SUCCEEDED(result)) result = device.As(&on12Device);
    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(result)) result = context.As(&multithread);
    if (FAILED(result))
    {
        error = hresultMessage("Create D3D11On12 device", result);
        return {};
    }
    multithread->SetMultithreadProtected(TRUE);
    error.clear();
    return std::shared_ptr<D3d11Device>{new D3d11Device{
        std::move(device), std::move(context), std::move(d3d12Device),
        std::move(queue), std::move(on12Device)}};
}

D3d11Device::D3d11Device(
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context,
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device,
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12Queue,
    Microsoft::WRL::ComPtr<ID3D11On12Device2> on12Device)
    : device_{std::move(device)}, context_{std::move(context)},
      d3d12Device_{std::move(d3d12Device)}, d3d12Queue_{std::move(d3d12Queue)},
      on12Device_{std::move(on12Device)}
{
}

ID3D11Device* D3d11Device::device() const noexcept { return device_.Get(); }
ID3D11DeviceContext* D3d11Device::immediateContext() const noexcept { return context_.Get(); }
std::mutex& D3d11Device::immediateContextMutex() noexcept { return contextMutex_; }
bool D3d11Device::isD3d11On12() const noexcept { return on12Device_ != nullptr; }
ID3D12Device* D3d11Device::d3d12Device() const noexcept { return d3d12Device_.Get(); }
ID3D12CommandQueue* D3d11Device::d3d12CommandQueue() const noexcept
{
    return d3d12Queue_.Get();
}
Microsoft::WRL::ComPtr<ID3D12Resource> D3d11Device::unwrapTexture(
    ID3D11Texture2D* texture,
    std::string& error,
    ID3D12CommandQueue* commandQueue) const
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (on12Device_ == nullptr || texture == nullptr)
    {
        error = "D3D11On12 texture unwrap is unavailable.";
        return {};
    }
    const HRESULT result = on12Device_->UnwrapUnderlyingResource(
        texture, commandQueue == nullptr ? d3d12Queue_.Get() : commandQueue,
        IID_PPV_ARGS(&resource));
    if (FAILED(result))
    {
        error = hresultMessage("Unwrap camera texture", result);
        return {};
    }
    error.clear();
    return resource;
}
bool D3d11Device::returnTexture(
    ID3D11Texture2D* texture,
    ID3D12Fence* fence,
    const std::uint64_t fenceValue,
    std::string& error) const
{
    if (on12Device_ == nullptr || texture == nullptr) return false;
    ID3D12Fence* fences[]{fence};
    const UINT fenceCount = fence == nullptr ? 0U : 1U;
    UINT64 values[]{fenceValue};
    const HRESULT result = on12Device_->ReturnUnderlyingResource(
        texture, fenceCount, fenceCount == 0 ? nullptr : values,
        fenceCount == 0 ? nullptr : fences);
    if (FAILED(result))
    {
        error = hresultMessage("Return camera texture", result);
        return false;
    }
    error.clear();
    return true;
}

bool D3d11Device::validateUnwrappedTexturePixels(
    ID3D12Resource* resource, std::string& diagnostic) const
{
    if (resource == nullptr || d3d12Device_ == nullptr || d3d12Queue_ == nullptr)
    {
        diagnostic = "D3D12 camera readback is unavailable.";
        return false;
    }
    const auto description = resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
    d3d12Device_->GetCopyableFootprints(
        &description, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = totalBytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    HRESULT result = d3d12Device_->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&readback));
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (SUCCEEDED(result)) result = d3d12Device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(result)) result = d3d12Device_->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        IID_PPV_ARGS(&list));
    if (FAILED(result))
    {
        diagnostic = hresultMessage("Create D3D12 camera readback", result);
        return false;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    result = list->Close();
    if (SUCCEEDED(result))
    {
        ID3D12CommandList* lists[]{list.Get()};
        d3d12Queue_->ExecuteCommandLists(1, lists);
    }
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (SUCCEEDED(result)) result = d3d12Device_->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (SUCCEEDED(result)) result = d3d12Queue_->Signal(fence.Get(), 1);
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(result) && eventHandle != nullptr)
    {
        result = fence->SetEventOnCompletion(1, eventHandle);
        if (SUCCEEDED(result) && WaitForSingleObject(eventHandle, 5000) != WAIT_OBJECT_0)
        {
            result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    }
    if (eventHandle != nullptr) CloseHandle(eventHandle);
    void* mappedPixels = nullptr;
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(totalBytes)};
    if (SUCCEEDED(result)) result = readback->Map(0, &readRange, &mappedPixels);
    if (FAILED(result))
    {
        diagnostic = hresultMessage("Read D3D12 camera pixels", result);
        return false;
    }
    const auto* pixels = static_cast<const std::uint8_t*>(mappedPixels);
    std::uint64_t hash = 1469598103934665603ULL;
    std::uint32_t nonZeroSamples = 0;
    constexpr std::uint32_t kGrid = 8;
    for (std::uint32_t gridY = 0; gridY < kGrid; ++gridY)
    {
        const auto y = (description.Height - 1) * gridY / (kGrid - 1);
        for (std::uint32_t gridX = 0; gridX < kGrid; ++gridX)
        {
            const auto x = (description.Width - 1) * gridX / (kGrid - 1);
            const auto* pixel = pixels + footprint.Offset
                + y * footprint.Footprint.RowPitch + x * 4;
            const std::uint32_t packed = static_cast<std::uint32_t>(pixel[0])
                | (static_cast<std::uint32_t>(pixel[1]) << 8)
                | (static_cast<std::uint32_t>(pixel[2]) << 16)
                | (static_cast<std::uint32_t>(pixel[3]) << 24);
            if (packed != 0) ++nonZeroSamples;
            hash = (hash ^ packed) * 1099511628211ULL;
        }
    }
    D3D12_RANGE writtenRange{0, 0};
    readback->Unmap(0, &writtenRange);
    std::ostringstream stream;
    stream << "pixel readback nonzero=" << nonZeroSamples << "/64, hash=0x"
        << std::hex << hash << '.';
    diagnostic = stream.str();
    return nonZeroSamples > 0;
}
}
