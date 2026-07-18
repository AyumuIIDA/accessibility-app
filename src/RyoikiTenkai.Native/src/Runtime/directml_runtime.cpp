#include "Runtime/directml_runtime.h"

#include <dxgi1_6.h>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace ryoiki::runtime
{
namespace
{
std::string failed(const char* operation, const HRESULT result)
{
    std::ostringstream stream;
    stream << operation << " failed with HRESULT 0x" << std::hex
        << static_cast<unsigned long>(result) << '.';
    return stream.str();
}
}

std::shared_ptr<DirectMlRuntime> DirectMlRuntime::create(
    const std::shared_ptr<D3d11Device>& d3d11Device,
    std::string& error)
{
    if (d3d11Device == nullptr)
    {
        error = "A D3D11 device is required for DirectML interop.";
        return {};
    }
    auto runtime = std::shared_ptr<DirectMlRuntime>{new DirectMlRuntime{}};
    runtime->d3d11Device_ = d3d11Device;
    HRESULT result = S_OK;
    if (d3d11Device->isD3d11On12())
    {
        runtime->d3d12Device_ = d3d11Device->d3d12Device();
    }
    else
    {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        result = d3d11Device->device()->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(result)) result = dxgiDevice->GetAdapter(&adapter);
        if (SUCCEEDED(result)) result = D3D12CreateDevice(
            adapter.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&runtime->d3d12Device_));
    }
    if (FAILED(result))
    {
        error = failed("Create matching D3D12 device", result);
        return {};
    }
    if (SUCCEEDED(result))
    {
        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        result = runtime->d3d12Device_->CreateCommandQueue(
            &queueDescription, IID_PPV_ARGS(&runtime->commandQueue_));
    }
    if (SUCCEEDED(result)) result = DMLCreateDevice(
        runtime->d3d12Device_.Get(), DML_CREATE_DEVICE_FLAG_NONE,
        IID_PPV_ARGS(&runtime->dmlDevice_));
    Microsoft::WRL::ComPtr<ID3D11Device5> d3d11Device5;
    if (SUCCEEDED(result)) result = d3d11Device->device()->QueryInterface(IID_PPV_ARGS(&d3d11Device5));
    if (SUCCEEDED(result)) result = d3d11Device5->CreateFence(
        0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&runtime->d3d11Fence_));
    HANDLE fenceHandle = nullptr;
    if (SUCCEEDED(result)) result = runtime->d3d11Fence_->CreateSharedHandle(
        nullptr, GENERIC_ALL, nullptr, &fenceHandle);
    if (SUCCEEDED(result)) result = runtime->d3d12Device_->OpenSharedHandle(
        fenceHandle, IID_PPV_ARGS(&runtime->d3d12Fence_));
    if (fenceHandle != nullptr) CloseHandle(fenceHandle);
    if (FAILED(result))
    {
        error = failed("Create DirectML interop resources", result);
        return {};
    }
    error.clear();
    return runtime;
}

IDMLDevice* DirectMlRuntime::dmlDevice() const noexcept { return dmlDevice_.Get(); }
ID3D12Device* DirectMlRuntime::d3d12Device() const noexcept { return d3d12Device_.Get(); }
ID3D12CommandQueue* DirectMlRuntime::commandQueue() const noexcept { return commandQueue_.Get(); }
std::shared_ptr<D3d11Device> DirectMlRuntime::d3d11Device() const noexcept
{
    return d3d11Device_;
}
ID3D11Fence* DirectMlRuntime::d3d11Fence() const noexcept { return d3d11Fence_.Get(); }
ID3D12Fence* DirectMlRuntime::d3d12Fence() const noexcept { return d3d12Fence_.Get(); }
std::uint64_t DirectMlRuntime::nextFenceValue() noexcept { return ++fenceValue_; }
bool DirectMlRuntime::signal(const std::uint64_t fenceValue, std::string& error) const
{
    const HRESULT result = commandQueue_->Signal(d3d12Fence_.Get(), fenceValue);
    if (FAILED(result))
    {
        error = failed("Signal DirectML queue", result);
        return false;
    }
    error.clear();
    return true;
}
void DirectMlRuntime::waitForPreprocess(const std::uint64_t fenceValue) const
{
    if (fenceValue != 0) commandQueue_->Wait(d3d12Fence_.Get(), fenceValue);
}

Microsoft::WRL::ComPtr<ID3D12Resource> DirectMlRuntime::tryOpenD3d11Texture(
    ID3D11Texture2D* texture,
    std::string& diagnostic) const
{
    if (texture == nullptr)
    {
        diagnostic = "Camera texture is null.";
        return {};
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    std::ostringstream prefix;
    prefix << "Camera texture " << description.Width << 'x' << description.Height
        << ", format=" << static_cast<unsigned>(description.Format)
        << ", miscFlags=0x" << std::hex << description.MiscFlags << ": ";
    Microsoft::WRL::ComPtr<IDXGIResource1> dxgiResource;
    HRESULT result = texture->QueryInterface(IID_PPV_ARGS(&dxgiResource));
    HANDLE sharedHandle = nullptr;
    if (SUCCEEDED(result))
    {
        result = dxgiResource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &sharedHandle);
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (SUCCEEDED(result))
    {
        result = d3d12Device_->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&resource));
    }
    if (sharedHandle != nullptr) CloseHandle(sharedHandle);
    if (FAILED(result))
    {
        diagnostic = prefix.str() + failed("zero-copy D3D11/D3D12 open", result);
        return {};
    }
    diagnostic = prefix.str() + "zero-copy D3D12 open succeeded.";
    return resource;
}

Microsoft::WRL::ComPtr<ID3D12Resource> DirectMlRuntime::copyCameraToSharedTexture(
    ID3D11Texture2D* texture,
    const std::uint64_t frameId,
    double& completedCopyMs,
    std::string& diagnostic)
{
    using clock = std::chrono::steady_clock;
    completedCopyMs = 0.0;
    if (texture == nullptr || d3d11Device_ == nullptr)
    {
        diagnostic = "A camera texture and D3D11 device are required.";
        return {};
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_DEFAULT;
    description.CPUAccessFlags = 0;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE
        | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    HRESULT result = S_OK;
    if (sharedCameraTexture_ == nullptr || sharedCameraWidth_ != description.Width
        || sharedCameraHeight_ != description.Height)
    {
        sharedCameraTexture_.Reset();
        sharedCameraResource_.Reset();
        result = d3d11Device_->device()->CreateTexture2D(
            &description, nullptr, &sharedCameraTexture_);
        if (FAILED(result))
        {
            diagnostic = failed("Create shareable D3D11 camera texture", result);
            return {};
        }
        Microsoft::WRL::ComPtr<IDXGIResource1> dxgiResource;
        HANDLE sharedHandle = nullptr;
        result = sharedCameraTexture_.As(&dxgiResource);
        if (SUCCEEDED(result)) result = dxgiResource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &sharedHandle);
        if (SUCCEEDED(result)) result = d3d12Device_->OpenSharedHandle(
            sharedHandle, IID_PPV_ARGS(&sharedCameraResource_));
        if (sharedHandle != nullptr) CloseHandle(sharedHandle);
        if (FAILED(result))
        {
            diagnostic = failed("Open shared camera texture in D3D12", result);
            return {};
        }
        sharedCameraWidth_ = description.Width;
        sharedCameraHeight_ = description.Height;
    }
    const auto started = clock::now();
    if (frameId != sharedCameraFrameId_)
    {
        std::lock_guard lock{d3d11Device_->immediateContextMutex()};
        d3d11Device_->immediateContext()->CopyResource(sharedCameraTexture_.Get(), texture);
        const auto value = nextFenceValue();
        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
        result = d3d11Device_->immediateContext()->QueryInterface(IID_PPV_ARGS(&context4));
        if (SUCCEEDED(result)) result = context4->Signal(d3d11Fence_.Get(), value);
        HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (SUCCEEDED(result) && eventHandle != nullptr)
        {
            result = d3d11Fence_->SetEventOnCompletion(value, eventHandle);
            if (SUCCEEDED(result)) WaitForSingleObject(eventHandle, 5000);
        }
        if (eventHandle != nullptr) CloseHandle(eventHandle);
        if (SUCCEEDED(result)) sharedCameraFrameId_ = frameId;
    }
    completedCopyMs = std::chrono::duration<double, std::milli>(
        clock::now() - started).count();
    std::ostringstream message;
    message << "Full-frame GPU shared copy " << description.Width << 'x'
        << description.Height << " completed in " << std::fixed
        << std::setprecision(3) << completedCopyMs << " ms: ";
    diagnostic = message.str() + (SUCCEEDED(result)
        ? "D3D12 open succeeded." : failed("D3D12 open", result));
    return sharedCameraResource_;
}

Microsoft::WRL::ComPtr<ID3D12Resource> DirectMlRuntime::copySharedTensorTexture(
    ID3D12Resource* texture,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint64_t readyFenceValue,
    std::string& error)
{
    const auto byteWidth = static_cast<UINT64>(width) * height * sizeof(float);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = byteWidth;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    HRESULT result = d3d12Device_->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&output));
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (SUCCEEDED(result)) result = d3d12Device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(result)) result = d3d12Device_->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.Get(), nullptr,
        IID_PPV_ARGS(&list));
    if (FAILED(result))
    {
        error = failed("Create GPU tensor copy", result);
        return {};
    }
    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = texture;
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = output.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    destination.PlacedFootprint.Footprint.Width = width;
    destination.PlacedFootprint.Footprint.Height = height;
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = width * sizeof(float);
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(toCopy.Transition.StateBefore, toCopy.Transition.StateAfter);
    list->ResourceBarrier(1, &toCopy);
    D3D12_RESOURCE_BARRIER outputBarrier{};
    outputBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    outputBarrier.Transition.pResource = output.Get();
    outputBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    outputBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    outputBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &outputBarrier);
    result = list->Close();
    if (FAILED(result))
    {
        error = failed("Close GPU tensor copy", result);
        return {};
    }
    waitForPreprocess(readyFenceValue);
    ID3D12CommandList* lists[]{list.Get()};
    commandQueue_->ExecuteCommandLists(1, lists);
    error.clear();
    return output;
}
}
