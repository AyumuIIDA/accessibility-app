#include "Geometry/d3d11_hand_geometry_processor.h"
#if defined(RYOIKI_ORT_DIRECTML)
#include "Runtime/directml_runtime.h"
#endif

#include <d3dcompiler.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <sstream>

namespace ryoiki::geometry
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr char kShaderSource[] = R"(
Texture2D<float4> sourceTexture : register(t0);
RWByteAddressBuffer outputTensor : register(u0);

cbuffer Parameters : register(b0)
{
    float4 transform0;
    float2 transform1;
    uint2 sourceSize;
    uint2 outputSize;
};

float4 loadOrZero(int2 coordinate)
{
    if (coordinate.x < 0 || coordinate.y < 0
        || coordinate.x >= int(sourceSize.x) || coordinate.y >= int(sourceSize.y))
    {
        return float4(0, 0, 0, 0);
    }
    return sourceTexture.Load(int3(coordinate, 0));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outputSize.x || id.y >= outputSize.y)
    {
        return;
    }
    float2 source = float2(
        transform0.x * id.x + transform0.y * id.y + transform0.z,
        transform0.w * id.x + transform1.x * id.y + transform1.y);
    int2 base = int2(floor(source));
    float2 fraction = source - float2(base);
    float4 top = lerp(loadOrZero(base), loadOrZero(base + int2(1, 0)), fraction.x);
    float4 bottom = lerp(loadOrZero(base + int2(0, 1)), loadOrZero(base + int2(1, 1)), fraction.x);
    float4 rgba = lerp(top, bottom, fraction.y);
    uint offset = (id.y * outputSize.x + id.x) * 3;
    outputTensor.Store(offset * 4, asuint(rgba.r));
    outputTensor.Store((offset + 1) * 4, asuint(rgba.g));
    outputTensor.Store((offset + 2) * 4, asuint(rgba.b));
}
)";

struct ShaderParameters
{
    float transform0[4]{};
    float transform1[2]{};
    std::uint32_t sourceSize[2]{};
    std::uint32_t outputSize[2]{};
    std::uint32_t padding[2]{};
};
static_assert(sizeof(ShaderParameters) == 48);

std::string hresultMessage(const char* operation, const HRESULT result)
{
    std::ostringstream stream;
    stream << operation << " failed with HRESULT 0x"
        << std::hex << static_cast<unsigned long>(result) << '.';
    return stream.str();
}

AffineTransform createPalmTensorToStorage(
    const buffers::FrameBuffer& frame,
    PalmPreprocessResult& result)
{
    constexpr float kSize = static_cast<float>(HandGeometryProcessor::kPalmInputSize);
    const float ratio = (std::min)(
        kSize / static_cast<float>(frame.uprightWidth()),
        kSize / static_cast<float>(frame.uprightHeight()));
    const int resizedWidth = (std::max)(1, static_cast<int>(frame.uprightWidth() * ratio));
    const int resizedHeight = (std::max)(1, static_cast<int>(frame.uprightHeight() * ratio));
    const int left = (HandGeometryProcessor::kPalmInputSize - resizedWidth) / 2;
    const int top = (HandGeometryProcessor::kPalmInputSize - resizedHeight) / 2;
    result.transform = {
        resizedWidth / static_cast<float>(frame.uprightWidth()),
        resizedHeight / static_cast<float>(frame.uprightHeight()),
        static_cast<float>(left),
        static_cast<float>(top)};
    const AffineTransform tensorToUpright{{
        1.0F / result.transform.scaleX, 0.0F,
        (0.5F - result.transform.padLeft) / result.transform.scaleX - 0.5F,
        0.0F, 1.0F / result.transform.scaleY,
        (0.5F - result.transform.padTop) / result.transform.scaleY - 0.5F}};
    AffineTransform uprightToStorage{};
    const bool inverted = invert(createStorageToUprightTransform(
        frame.width(), frame.height(), frame.orientation()), uprightToStorage);
    (void)inverted;
    return compose(tensorToUpright, toPixelCenterTransform(uprightToStorage));
}

AffineTransform createHandTensorToStorage(
    const buffers::FrameBuffer& frame,
    const RotatedRegion& region,
    HandPreprocessResult& result)
{
    constexpr float kSize = static_cast<float>(HandGeometryProcessor::kHandInputSize);
    const float cosine = std::cos(region.rotationRadiansClockwise);
    const float sine = std::sin(region.rotationRadiansClockwise);
    result.tensorToSource = {{
        cosine * region.width / kSize,
        -sine * region.height / kSize,
        region.center.x - cosine * region.width * 0.5F + sine * region.height * 0.5F,
        sine * region.width / kSize,
        cosine * region.height / kSize,
        region.center.y - sine * region.width * 0.5F - cosine * region.height * 0.5F}};
    const bool sourceTransformInverted = invert(result.tensorToSource, result.sourceToTensor);
    (void)sourceTransformInverted;
    AffineTransform uprightToStorage{};
    const bool orientationInverted = invert(createStorageToUprightTransform(
        frame.width(), frame.height(), frame.orientation()), uprightToStorage);
    (void)orientationInverted;
    return compose(result.tensorToSource, toPixelCenterTransform(uprightToStorage));
}
}

class D3d11HandGeometryProcessor::Impl final
{
public:
    Impl(std::shared_ptr<runtime::D3d11Device> device,
         std::shared_ptr<runtime::DirectMlRuntime> directMlRuntime)
        : device_{std::move(device)}, directMlRuntime_{std::move(directMlRuntime)} {}

    bool initialize(std::string& error)
    {
        ComPtr<ID3DBlob> bytecode;
        ComPtr<ID3DBlob> diagnostics;
        HRESULT result = D3DCompile(
            kShaderSource, sizeof(kShaderSource) - 1, "gpu_hand_preprocess", nullptr, nullptr,
            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &diagnostics);
        if (FAILED(result))
        {
            error = diagnostics == nullptr
                ? hresultMessage("D3DCompile", result)
                : std::string{static_cast<const char*>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize()};
            return false;
        }
        result = device_->device()->CreateComputeShader(
            bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader_);
        if (FAILED(result))
        {
            error = hresultMessage("Create GPU preprocess shader", result);
            return false;
        }
        D3D11_BUFFER_DESC constantDescription{};
        constantDescription.ByteWidth = sizeof(ShaderParameters);
        constantDescription.Usage = D3D11_USAGE_DYNAMIC;
        constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        result = device_->device()->CreateBuffer(&constantDescription, nullptr, &constantBuffer_);
        if (FAILED(result))
        {
            error = hresultMessage("Create GPU preprocess constants", result);
            return false;
        }
        return true;
    }

    bool process(
        const buffers::FrameBuffer& frame,
        const AffineTransform& transform,
        const std::uint32_t outputSize,
        buffers::FloatTensorBuffer& tensor,
        std::string& error)
    {
        if (frame.gpuTexture() == nullptr || !ensureResources(frame, outputSize, error))
        {
            return false;
        }
        const auto& v = transform.values;
        ShaderParameters parameters{
            {v[0], v[1], v[2], v[3]}, {v[4], v[5]},
            {frame.width(), frame.height()}, {outputSize, outputSize}};
        auto* context = device_->immediateContext();
        std::lock_guard lock{device_->immediateContextMutex()};
        context->CopySubresourceRegion(
            sourceTexture_.Get(), 0, 0, 0, 0,
            frame.gpuTexture(), frame.gpuTextureSubresource(), nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT result = context->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(result))
        {
            error = hresultMessage("Map GPU preprocess constants", result);
            return false;
        }
        std::memcpy(mapped.pData, &parameters, sizeof(parameters));
        context->Unmap(constantBuffer_.Get(), 0);
        ID3D11ShaderResourceView* srv = sourceView_.Get();
        ID3D11UnorderedAccessView* uav = outputView_.Get();
        ID3D11Buffer* constants = constantBuffer_.Get();
        context->CSSetShader(shader_.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, &srv);
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->CSSetConstantBuffers(0, 1, &constants);
        context->Dispatch((outputSize + 7U) / 8U, (outputSize + 7U) / 8U, 1);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ID3D11UnorderedAccessView* nullUav = nullptr;
        context->CSSetShaderResources(0, 1, &nullSrv);
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
#if defined(RYOIKI_ORT_DIRECTML)
        if (directMlRuntime_ != nullptr)
        {
            Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
            result = context->QueryInterface(IID_PPV_ARGS(&context4));
            if (FAILED(result))
            {
                error = hresultMessage("Query D3D11 fence context", result);
                return false;
            }
            const auto fenceValue = directMlRuntime_->nextFenceValue();
            result = context4->Signal(directMlRuntime_->d3d11Fence(), fenceValue);
            if (FAILED(result))
            {
                error = hresultMessage("Signal GPU preprocess completion", result);
                return false;
            }
            tensor.setGpuResource(sharedOutputBuffer_, fenceValue);
            return true;
        }
#endif
        context->CopyResource(stagingBuffer_.Get(), outputBuffer_.Get());
        result = context->Map(stagingBuffer_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(result))
        {
            error = hresultMessage("Read GPU preprocess tensor", result);
            return false;
        }
        std::memcpy(tensor.data(), mapped.pData, tensor.elementCount() * sizeof(float));
        context->Unmap(stagingBuffer_.Get(), 0);
        return true;
    }

private:
    bool ensureResources(
        const buffers::FrameBuffer& frame,
        const std::uint32_t outputSize,
        std::string& error)
    {
        if (sourceTexture_ != nullptr && sourceWidth_ == frame.width()
            && sourceHeight_ == frame.height() && outputSize_ == outputSize)
        {
            return true;
        }
        sourceTexture_.Reset();
        sourceView_.Reset();
        outputBuffer_.Reset();
        outputView_.Reset();
        sharedOutputBuffer_.Reset();
        stagingBuffer_.Reset();
        D3D11_TEXTURE2D_DESC sourceDescription{};
        sourceDescription.Width = frame.width();
        sourceDescription.Height = frame.height();
        sourceDescription.MipLevels = 1;
        sourceDescription.ArraySize = 1;
        sourceDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sourceDescription.SampleDesc.Count = 1;
        sourceDescription.Usage = D3D11_USAGE_DEFAULT;
        sourceDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT result = device_->device()->CreateTexture2D(&sourceDescription, nullptr, &sourceTexture_);
        if (FAILED(result)
            || FAILED(device_->device()->CreateShaderResourceView(sourceTexture_.Get(), nullptr, &sourceView_)))
        {
            error = "Failed to create the GPU preprocess source texture.";
            return false;
        }
        const UINT byteWidth = outputSize * outputSize * 3U * sizeof(float);
#if defined(RYOIKI_ORT_DIRECTML)
        if (directMlRuntime_ != nullptr)
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            description.Width = byteWidth;
            description.Height = 1;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            result = directMlRuntime_->d3d12Device()->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_SHARED, &description,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&sharedOutputBuffer_));
            HANDLE sharedHandle = nullptr;
            if (SUCCEEDED(result)) result = directMlRuntime_->d3d12Device()->CreateSharedHandle(
                sharedOutputBuffer_.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle);
            ComPtr<ID3D11Device1> device1;
            if (SUCCEEDED(result)) result = device_->device()->QueryInterface(IID_PPV_ARGS(&device1));
            if (SUCCEEDED(result)) result = device1->OpenSharedResource1(
                sharedHandle, IID_PPV_ARGS(&outputBuffer_));
            if (sharedHandle != nullptr) CloseHandle(sharedHandle);
            if (FAILED(result))
            {
                error = hresultMessage("Open DirectML tensor buffer in D3D11", result);
                return false;
            }
        }
        else
#endif
        {
        D3D11_BUFFER_DESC outputDescription{};
        outputDescription.ByteWidth = byteWidth;
        outputDescription.Usage = D3D11_USAGE_DEFAULT;
        outputDescription.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        outputDescription.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        result = device_->device()->CreateBuffer(&outputDescription, nullptr, &outputBuffer_);
        D3D11_BUFFER_DESC stagingDescription = outputDescription;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDescription.MiscFlags = 0;
        if (SUCCEEDED(result)) result = device_->device()->CreateBuffer(
            &stagingDescription, nullptr, &stagingBuffer_);
        if (FAILED(result))
        {
            error = hresultMessage("Create GPU tensor staging buffer", result);
            return false;
        }
        D3D11_UNORDERED_ACCESS_VIEW_DESC outputViewDescription{};
        outputViewDescription.Format = DXGI_FORMAT_R32_TYPELESS;
        outputViewDescription.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        outputViewDescription.Buffer.NumElements = byteWidth / sizeof(float);
        outputViewDescription.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        result = device_->device()->CreateUnorderedAccessView(
            outputBuffer_.Get(), &outputViewDescription, &outputView_);
        if (FAILED(result))
        {
            error = hresultMessage("Create GPU tensor UAV", result);
            return false;
        }
        }
        sourceWidth_ = frame.width();
        sourceHeight_ = frame.height();
        outputSize_ = outputSize;
        return true;
    }

    std::shared_ptr<runtime::D3d11Device> device_;
    std::shared_ptr<runtime::DirectMlRuntime> directMlRuntime_;
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> constantBuffer_;
    ComPtr<ID3D11Texture2D> sourceTexture_;
    ComPtr<ID3D11ShaderResourceView> sourceView_;
    ComPtr<ID3D11Buffer> outputBuffer_;
    ComPtr<ID3D11UnorderedAccessView> outputView_;
    ComPtr<ID3D11Buffer> stagingBuffer_;
    ComPtr<ID3D12Resource> sharedOutputBuffer_;
    std::uint32_t sourceWidth_{0};
    std::uint32_t sourceHeight_{0};
    std::uint32_t outputSize_{0};
};

std::unique_ptr<D3d11HandGeometryProcessor> D3d11HandGeometryProcessor::create(
    std::shared_ptr<runtime::D3d11Device> device,
    std::string& error)
{
    return create(std::move(device), {}, error);
}

std::unique_ptr<D3d11HandGeometryProcessor> D3d11HandGeometryProcessor::create(
    std::shared_ptr<runtime::D3d11Device> device,
    std::shared_ptr<runtime::DirectMlRuntime> directMlRuntime,
    std::string& error)
{
    if (device == nullptr)
    {
        error = "A D3D11 device is required for GPU preprocessing.";
        return {};
    }
    auto impl = std::make_unique<Impl>(std::move(device), std::move(directMlRuntime));
    if (!impl->initialize(error))
    {
        return {};
    }
    return std::unique_ptr<D3d11HandGeometryProcessor>{
        new D3d11HandGeometryProcessor{std::move(impl)}};
}

D3d11HandGeometryProcessor::D3d11HandGeometryProcessor(std::unique_ptr<Impl> impl)
    : impl_{std::move(impl)} {}
D3d11HandGeometryProcessor::~D3d11HandGeometryProcessor() = default;
buffers::MemoryLocation D3d11HandGeometryProcessor::inputMemoryLocation() const noexcept
{
    return buffers::MemoryLocation::Gpu;
}
buffers::MemoryLocation D3d11HandGeometryProcessor::outputMemoryLocation() const noexcept
{
    return impl_ == nullptr ? buffers::MemoryLocation::Cpu : buffers::MemoryLocation::Gpu;
}

bool D3d11HandGeometryProcessor::preprocessPalm(
    const buffers::FrameBuffer& frame,
    buffers::FloatTensorBuffer& tensor,
    PalmPreprocessResult& result)
{
    constexpr std::array<std::int64_t, 4> kShape{1, 192, 192, 3};
    if (tensor.shape() != kShape || frame.gpuTexture() == nullptr)
    {
        return false;
    }
    return impl_->process(frame, createPalmTensorToStorage(frame, result), 192, tensor, lastError_);
}

bool D3d11HandGeometryProcessor::preprocessHand(
    const buffers::FrameBuffer& frame,
    const RotatedRegion& region,
    buffers::FloatTensorBuffer& tensor,
    HandPreprocessResult& result)
{
    constexpr std::array<std::int64_t, 4> kShape{1, 224, 224, 3};
    if (tensor.shape() != kShape || frame.gpuTexture() == nullptr
        || region.width <= 0.0F || region.height <= 0.0F)
    {
        return false;
    }
    return impl_->process(frame, createHandTensorToStorage(frame, region, result), 224, tensor, lastError_);
}
const std::string& D3d11HandGeometryProcessor::lastError() const noexcept { return lastError_; }
}
