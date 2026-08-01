#include "Rendering/d3d11_d2d_renderer.h"

#include "Rendering/frame_transforms.h"
#include "Rendering/hand_3d_plot.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <optional>
#include <sstream>

namespace ryoiki::rendering
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr std::array<std::array<int, 2>, 23> kHandConnections{{
    {0, 1}, {1, 2}, {2, 3}, {3, 4},
    {0, 5}, {5, 6}, {6, 7}, {7, 8},
    {0, 9}, {9, 10}, {10, 11}, {11, 12},
    {0, 13}, {13, 14}, {14, 15}, {15, 16},
    {0, 17}, {17, 18}, {18, 19}, {19, 20},
    {5, 9}, {9, 13}, {13, 17}}};

std::string hresultMessage(const char* operation, const HRESULT result)
{
    std::ostringstream stream;
    stream << operation << " failed with HRESULT 0x"
        << std::hex << static_cast<unsigned long>(result) << '.';
    return stream.str();
}

D2D1_MATRIX_3X2_F toD2dMatrix(const geometry::AffineTransform& value) noexcept
{
    const auto& v = value.values;
    return D2D1::Matrix3x2F(
        v[0], v[3], v[1], v[4], v[2], v[5]);
}

D2D1_POINT_2F toD2dPoint(const geometry::Point2f point) noexcept
{
    return D2D1::Point2F(point.x, point.y);
}

bool isDeviceLost(const HRESULT result) noexcept
{
    return result == DXGI_ERROR_DEVICE_REMOVED
        || result == DXGI_ERROR_DEVICE_RESET
        || result == D2DERR_RECREATE_TARGET;
}
}

class D3d11D2dRenderer::Impl final
{
public:
    bool initialize(
        std::shared_ptr<runtime::D3d11Device> sharedDevice,
        const HWND hwnd,
        const std::uint32_t width,
        const std::uint32_t height,
        std::string& error)
    {
        sharedDevice_ = std::move(sharedDevice);
        if (sharedDevice_ == nullptr)
        {
            error = "A shared D3D11 device is required.";
            return false;
        }
        hwnd_ = hwnd;
        width_ = width;
        height_ = height;
        return createDeviceResources(error);
    }

    bool resize(
        const std::uint32_t width,
        const std::uint32_t height,
        std::string& error)
    {
        width_ = width;
        height_ = height;
        if (width == 0 || height == 0)
        {
            return true;
        }
        if (swapChain_ == nullptr)
        {
            return createDeviceResources(error);
        }

        d2dContext_->SetTarget(nullptr);
        targetBitmap_.Reset();
        const HRESULT result = swapChain_->ResizeBuffers(
            0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(result))
        {
            if (isDeviceLost(result))
            {
                return createDeviceResources(error);
            }
            error = hresultMessage("IDXGISwapChain::ResizeBuffers", result);
            return false;
        }
        return createTargetBitmap(error);
    }

    bool render(
        const RenderPacket& packet,
        RenderPresentation& presentation,
        std::string& error)
    {
        HRESULT result = renderOnce(packet, presentation, error);
        if (!isDeviceLost(result))
        {
            return SUCCEEDED(result);
        }

        error.clear();
        if (!createDeviceResources(error))
        {
            return false;
        }
        result = renderOnce(packet, presentation, error);
        return SUCCEEDED(result);
    }

    void setHand3dView(const Hand3dView view) noexcept
    {
        hand3dView_ = view;
    }

    ~Impl()
    {
        releaseSwapChain();
    }

    // DXGI keeps an HWND associated with its swap chain until the swap chain is
    // actually destroyed, and destruction is deferred while the immediate
    // context still references the back buffers. Releasing the ComPtr alone is
    // not enough: a Stop followed by a quick Start then failed with
    // CreateSwapChainForHwnd -> E_ACCESSDENIED (0x80070005), while a restart
    // after a long enough pause succeeded once the deferred release had run.
    // ClearState + Flush forces that release now, so restarting is immediate.
    void releaseSwapChain() noexcept
    {
        if (d2dContext_ != nullptr)
        {
            d2dContext_->SetTarget(nullptr);
        }
        targetBitmap_.Reset();
        cameraBitmap_.Reset();
        gpuCopyTexture_.Reset();
        if (d3dContext_ != nullptr)
        {
            if (sharedDevice_ != nullptr)
            {
                std::lock_guard lock{sharedDevice_->immediateContextMutex()};
                d3dContext_->ClearState();
                d3dContext_->Flush();
            }
            else
            {
                d3dContext_->ClearState();
                d3dContext_->Flush();
            }
        }
        swapChain_.Reset();
    }

private:
    bool createDeviceResources(std::string& error)
    {
        palmBrush_.Reset();
        handBrush_.Reset();
        candidateBrush_.Reset();
        activeBrush_.Reset();
        labelBackgroundBrush_.Reset();
        labelTextBrush_.Reset();
        textFormat_.Reset();
        dwriteFactory_.Reset();
        plotGridBrush_.Reset();
        plotXAxisBrush_.Reset();
        plotYAxisBrush_.Reset();
        plotZAxisBrush_.Reset();
        palmDirectionBrush_.Reset();
        // Releases target/camera bitmaps and the swap chain in the order DXGI
        // requires before this HWND can be given a new swap chain.
        releaseSwapChain();
        d2dContext_.Reset();
        d2dDevice_.Reset();
        d2dFactory_.Reset();
        cameraWidth_ = 0;
        cameraHeight_ = 0;
        cameraBitmapIsGpuSurface_ = false;
        uploadedFrameId_.reset();

        d3dDevice_ = sharedDevice_->device();
        d3dContext_ = sharedDevice_->immediateContext();
        HRESULT result = S_OK;

        ComPtr<IDXGIDevice> dxgiDevice;
        result = d3dDevice_.As(&dxgiDevice);
        if (FAILED(result))
        {
            error = hresultMessage("ID3D11Device::QueryInterface(IDXGIDevice)", result);
            return false;
        }
        ComPtr<IDXGIDevice1> latencyDevice;
        if (SUCCEEDED(dxgiDevice.As(&latencyDevice)))
        {
            latencyDevice->SetMaximumFrameLatency(1);
        }

        ComPtr<IDXGIAdapter> adapter;
        result = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(result))
        {
            error = hresultMessage("IDXGIDevice::GetAdapter", result);
            return false;
        }
        ComPtr<IDXGIFactory2> dxgiFactory;
        result = adapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
        if (FAILED(result))
        {
            error = hresultMessage("IDXGIAdapter::GetParent", result);
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};
        swapChainDescription.Width = width_;
        swapChainDescription.Height = height_;
        swapChainDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapChainDescription.SampleDesc.Count = 1;
        swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDescription.BufferCount = 2;
        swapChainDescription.Scaling = DXGI_SCALING_STRETCH;
        swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDescription.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        result = dxgiFactory->CreateSwapChainForHwnd(
            d3dDevice_.Get(),
            hwnd_,
            &swapChainDescription,
            nullptr,
            nullptr,
            &swapChain_);
        if (FAILED(result))
        {
            error = hresultMessage("IDXGIFactory2::CreateSwapChainForHwnd", result);
            return false;
        }

        D2D1_FACTORY_OPTIONS factoryOptions{};
        result = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1),
            &factoryOptions,
            &d2dFactory_);
        if (FAILED(result))
        {
            error = hresultMessage("D2D1CreateFactory", result);
            return false;
        }
        result = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
        if (FAILED(result))
        {
            error = hresultMessage("ID2D1Factory1::CreateDevice", result);
            return false;
        }
        result = d2dDevice_->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            &d2dContext_);
        if (FAILED(result))
        {
            error = hresultMessage("ID2D1Device::CreateDeviceContext", result);
            return false;
        }
        d2dContext_->SetDpi(96.0F, 96.0F);
        d2dContext_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (!createTargetBitmap(error))
        {
            return false;
        }

        result = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
        if (FAILED(result))
        {
            error = hresultMessage("DWriteCreateFactory", result);
            return false;
        }
        result = dwriteFactory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            16.0F,
            L"en-us",
            &textFormat_);
        if (FAILED(result))
        {
            error = hresultMessage("Create Domain Sign text format", result);
            return false;
        }
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        result = d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.0F, 1.0F, 0.70F), &palmBrush_);
        if (FAILED(result))
        {
            error = hresultMessage("Create palm brush", result);
            return false;
        }
        result = d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(1.0F, 0.58F, 0.08F), &candidateBrush_);
        if (FAILED(result))
        {
            error = hresultMessage("Create candidate State brush", result);
            return false;
        }
        result = d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.20F, 0.92F, 1.0F), &activeBrush_);
        if (FAILED(result))
        {
            error = hresultMessage("Create active State brush", result);
            return false;
        }
        result = d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.03F, 0.05F, 0.09F, 0.88F), &labelBackgroundBrush_);
        if (FAILED(result))
        {
            error = hresultMessage("Create State label background brush", result);
            return false;
        }
        result = d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.97F, 0.99F, 1.0F), &labelTextBrush_);
        if (FAILED(result))
        {
            error = hresultMessage("Create State label text brush", result);
            return false;
        }
        result = d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(1.0F, 0.82F, 0.27F), &handBrush_);
        if (FAILED(result))
        {
            error = hresultMessage("Create hand brush", result);
            return false;
        }
        result = d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.30F, 0.34F, 0.42F), &plotGridBrush_);
        if (FAILED(result))
        {
            error = hresultMessage("Create 3D plot grid brush", result);
            return false;
        }
        result = d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.95F, 0.30F, 0.30F), &plotXAxisBrush_);
        if (FAILED(result))
        {
            error = hresultMessage("Create 3D plot X axis brush", result);
            return false;
        }
        result = d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.30F, 0.90F, 0.45F), &plotYAxisBrush_);
        if (FAILED(result))
        {
            error = hresultMessage("Create 3D plot Y axis brush", result);
            return false;
        }
        result = d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.30F, 0.55F, 1.0F), &plotZAxisBrush_);
        if (FAILED(result))
        {
            error = hresultMessage("Create 3D plot Z axis brush", result);
            return false;
        }
        result = d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.10F, 0.95F, 1.0F), &palmDirectionBrush_);
        if (FAILED(result))
        {
            error = hresultMessage("Create palm direction brush", result);
            return false;
        }
        return true;
    }

    bool createTargetBitmap(std::string& error)
    {
        ComPtr<IDXGISurface> backBuffer;
        const HRESULT getBufferResult = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(getBufferResult))
        {
            error = hresultMessage("IDXGISwapChain::GetBuffer", getBufferResult);
            return false;
        }

        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_IGNORE),
            96.0F,
            96.0F);
        const HRESULT result = d2dContext_->CreateBitmapFromDxgiSurface(
            backBuffer.Get(), &properties, &targetBitmap_);
        if (FAILED(result))
        {
            error = hresultMessage("CreateBitmapFromDxgiSurface", result);
            return false;
        }
        d2dContext_->SetTarget(targetBitmap_.Get());
        return true;
    }

    bool ensureCameraBitmap(
        const std::uint32_t width,
        const std::uint32_t height,
        std::string& error)
    {
        if (cameraBitmap_ != nullptr && !cameraBitmapIsGpuSurface_
            && cameraWidth_ == width && cameraHeight_ == height)
        {
            return true;
        }
        cameraBitmap_.Reset();
        uploadedFrameId_.reset();
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_IGNORE),
            96.0F,
            96.0F);
        const HRESULT result = d2dContext_->CreateBitmap(
            D2D1::SizeU(width, height),
            nullptr,
            0,
            &properties,
            &cameraBitmap_);
        if (FAILED(result))
        {
            error = hresultMessage("Create camera bitmap", result);
            return false;
        }
        cameraWidth_ = width;
        cameraHeight_ = height;
        cameraBitmapIsGpuSurface_ = false;
        return true;
    }

    bool tryUseGpuCameraBitmap(
        const buffers::FrameBuffer& frame,
        bool& usedGpuSurface,
        std::string& error)
    {
        usedGpuSurface = false;
        if (frame.gpuTexture() == nullptr)
        {
            return true;
        }
        if (uploadedFrameId_.has_value() && *uploadedFrameId_ == frame.frameId()
            && cameraBitmap_ != nullptr)
        {
            usedGpuSurface = true;
            return true;
        }

        D3D11_TEXTURE2D_DESC description{};
        frame.gpuTexture()->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            return true;
        }
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_IGNORE),
            96.0F,
            96.0F);
        if (frame.gpuTextureSubresource() == 0)
        {
            ComPtr<IDXGISurface> cameraSurface;
            if (SUCCEEDED(frame.gpuTexture()->QueryInterface(IID_PPV_ARGS(&cameraSurface))))
            {
                ComPtr<ID2D1Bitmap1> directBitmap;
                const HRESULT directResult = d2dContext_->CreateBitmapFromDxgiSurface(
                    cameraSurface.Get(), &properties, &directBitmap);
                if (SUCCEEDED(directResult))
                {
                    cameraBitmap_ = std::move(directBitmap);
                    cameraWidth_ = frame.width();
                    cameraHeight_ = frame.height();
                    cameraBitmapIsGpuSurface_ = true;
                    uploadedFrameId_ = frame.frameId();
                    usedGpuSurface = true;
                    return true;
                }
            }
        }

        HRESULT result = S_OK;
        const bool recreateCopyTexture = gpuCopyTexture_ == nullptr
            || cameraWidth_ != frame.width() || cameraHeight_ != frame.height()
            || !cameraBitmapIsGpuSurface_;
        if (recreateCopyTexture)
        {
            D3D11_TEXTURE2D_DESC copyDescription{};
            copyDescription.Width = description.Width;
            copyDescription.Height = description.Height;
            copyDescription.MipLevels = 1;
            copyDescription.ArraySize = 1;
            copyDescription.Format = description.Format;
            copyDescription.SampleDesc.Count = 1;
            copyDescription.Usage = D3D11_USAGE_DEFAULT;
            copyDescription.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            gpuCopyTexture_.Reset();
            result = d3dDevice_->CreateTexture2D(
                &copyDescription, nullptr, &gpuCopyTexture_);
            if (FAILED(result))
            {
                error = hresultMessage("Create drawable camera texture", result);
                return false;
            }
            ComPtr<IDXGISurface> surface;
            result = gpuCopyTexture_.As(&surface);
            if (FAILED(result))
            {
                error = hresultMessage("Query drawable camera DXGI surface", result);
                return false;
            }
            ComPtr<ID2D1Bitmap1> gpuBitmap;
            result = d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &gpuBitmap);
            if (FAILED(result))
            {
                error = hresultMessage("Wrap drawable camera DXGI surface", result);
                return false;
            }
            cameraBitmap_ = std::move(gpuBitmap);
        }
        {
            std::lock_guard lock{sharedDevice_->immediateContextMutex()};
            d3dContext_->CopySubresourceRegion(
                gpuCopyTexture_.Get(), 0, 0, 0, 0,
                frame.gpuTexture(), frame.gpuTextureSubresource(), nullptr);
        }
        cameraWidth_ = frame.width();
        cameraHeight_ = frame.height();
        cameraBitmapIsGpuSurface_ = true;
        uploadedFrameId_ = frame.frameId();
        usedGpuSurface = true;
        return true;
    }

    HRESULT renderOnce(
        const RenderPacket& packet,
        RenderPresentation& presentation,
        std::string& error)
    {
        using clock = std::chrono::steady_clock;
        const auto started = clock::now();
        if (packet.frame == nullptr
            || (packet.frame->pixels().empty() && packet.frame->gpuTexture() == nullptr)
            || width_ == 0 || height_ == 0)
        {
            error = "The render packet does not contain a drawable frame.";
            return E_INVALIDARG;
        }
        const auto& frame = *packet.frame;
        HRESULT result = S_OK;
        const auto uploadStarted = clock::now();
        bool usedGpuSurface = false;
        if (!tryUseGpuCameraBitmap(frame, usedGpuSurface, error))
        {
            return E_FAIL;
        }
        if (!usedGpuSurface)
        {
            if (frame.pixels().empty())
            {
                error = "The GPU camera surface cannot be drawn and has no CPU fallback.";
                return E_INVALIDARG;
            }
            if (!ensureCameraBitmap(frame.width(), frame.height(), error))
            {
                return E_FAIL;
            }
            if (!uploadedFrameId_.has_value() || *uploadedFrameId_ != frame.frameId())
            {
                result = cameraBitmap_->CopyFromMemory(
                    nullptr,
                    frame.pixels().data(),
                    frame.stride());
                if (FAILED(result))
                {
                    error = hresultMessage("ID2D1Bitmap1::CopyFromMemory", result);
                    return result;
                }
                uploadedFrameId_ = frame.frameId();
            }
        }
        const auto uploadCompleted = clock::now();

        const auto cameraViewport = createCameraViewport();
        const auto plotViewport = createPlotViewport(cameraViewport);
        FrameTransforms transforms{};
        if (!createFrameTransforms(
                frame.width(),
                frame.height(),
                frame.orientation(),
                cameraViewport,
                true,
                transforms))
        {
            error = "Failed to calculate frame-to-viewport transforms.";
            return E_INVALIDARG;
        }

        const auto cameraDrawStarted = clock::now();
        d2dContext_->BeginDraw();
        d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());
        d2dContext_->Clear(D2D1::ColorF(0.02F, 0.024F, 0.031F));
        d2dContext_->SetTransform(toD2dMatrix(transforms.storageToViewport));
        d2dContext_->DrawBitmap(
            cameraBitmap_.Get(),
            D2D1::RectF(
                0.0F,
                0.0F,
                static_cast<float>(frame.width()),
                static_cast<float>(frame.height())),
            1.0F,
            D2D1_INTERPOLATION_MODE_LINEAR,
            nullptr);
        const auto cameraDrawCompleted = clock::now();

        const auto overlayDrawStarted = clock::now();
        d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());
        drawPalmOverlay(packet, transforms);
        drawHandOverlay(packet, transforms);
        const auto overlayDrawCompleted = clock::now();
        const auto hand3dDrawStarted = clock::now();
        drawHand3dPlot(packet, plotViewport);
        const auto hand3dDrawCompleted = clock::now();
        const auto endDrawStarted = clock::now();
        result = d2dContext_->EndDraw();
        const auto endDrawCompleted = clock::now();
        if (FAILED(result))
        {
            error = hresultMessage("ID2D1DeviceContext::EndDraw", result);
            return result;
        }
        const auto presentStarted = clock::now();
        result = swapChain_->Present(1, 0);
        const auto presentCompleted = clock::now();
        if (FAILED(result))
        {
            error = hresultMessage("IDXGISwapChain::Present", result);
            return result;
        }

        presentation.frameId = frame.frameId();
        presentation.captureTimestampUs = frame.captureTimestampUs();
        presentation.cameraUploadMs = std::chrono::duration<double, std::milli>(
            uploadCompleted - uploadStarted).count();
        presentation.cameraDrawMs = std::chrono::duration<double, std::milli>(
            cameraDrawCompleted - cameraDrawStarted).count();
        presentation.overlayDrawMs = std::chrono::duration<double, std::milli>(
            overlayDrawCompleted - overlayDrawStarted).count();
        presentation.hand3dDrawMs = std::chrono::duration<double, std::milli>(
            hand3dDrawCompleted - hand3dDrawStarted).count();
        presentation.endDrawMs = std::chrono::duration<double, std::milli>(
            endDrawCompleted - endDrawStarted).count();
        presentation.presentWaitMs = std::chrono::duration<double, std::milli>(
            presentCompleted - presentStarted).count();
        presentation.renderMs = std::chrono::duration<double, std::milli>(
            presentCompleted - started).count();
        presentation.usedGpuCameraSurface = usedGpuSurface;
        return S_OK;
    }

    void drawPalmOverlay(const RenderPacket& packet, const FrameTransforms& transforms)
    {
        for (std::size_t palmIndex = 0;
            palmIndex < packet.perception.palms.size();
            ++palmIndex)
        {
            const auto& palm = packet.perception.palms[palmIndex];
            const auto topLeft = transforms.uprightToViewport.transform(
                {palm.box.left, palm.box.top});
            const auto bottomRight = transforms.uprightToViewport.transform(
                {palm.box.right, palm.box.bottom});
            const auto left = (std::min)(topLeft.x, bottomRight.x);
            const auto right = (std::max)(topLeft.x, bottomRight.x);
            const auto top = (std::min)(topLeft.y, bottomRight.y);
            const auto bottom = (std::max)(topLeft.y, bottomRight.y);
            d2dContext_->DrawRectangle(
                D2D1::RectF(left, top, right, bottom), palmBrush_.Get(), 3.0F);
            for (const auto& keypoint : palm.keypoints)
            {
                const auto point = transforms.uprightToViewport.transform(
                    {keypoint.x, keypoint.y});
                d2dContext_->FillEllipse(
                    D2D1::Ellipse(toD2dPoint(point), 3.5F, 3.5F),
                    palmBrush_.Get());
            }
        }
    }

    void drawHandOverlay(const RenderPacket& packet, const FrameTransforms& transforms)
    {
        const std::size_t handCount = packet.perception.handCount > 0
            ? packet.perception.handCount
            : packet.perception.hand.detected ? 1 : 0;
        if (handCount == 0)
        {
            return;
        }
        using hand_input::recognition::HandStatePhase;
        // Only Domain Sign remains a published State. With Open Palm removed the
        // skeleton stays in its neutral colour unless that State is running, so
        // the overlay no longer recolours itself for an unregistered pose.
        const bool domainVisible =
            packet.domainSignState.phase == HandStatePhase::Candidate
            || packet.domainSignState.phase == HandStatePhase::Active;
        const auto& displayState = packet.domainSignState;
        const wchar_t* stateName = L"DOMAIN SIGN";
        ID2D1SolidColorBrush* overlayBrush = handBrush_.Get();
        if (domainVisible)
        {
            overlayBrush = displayState.phase == HandStatePhase::Candidate
                ? candidateBrush_.Get()
                : activeBrush_.Get();
        }

        for (std::size_t handIndex = 0;
            handIndex < handCount;
            ++handIndex)
        {
            const auto& hand = packet.perception.handCount > 0
                ? packet.perception.hands[handIndex]
                : packet.perception.hand;
            const auto mapLandmark = [&hand, &transforms](const int index)
            {
                return transforms.uprightToViewport.transform(
                    {hand.landmarks[index].x, hand.landmarks[index].y});
            };
            for (const auto& connection : kHandConnections)
            {
                d2dContext_->DrawLine(
                    toD2dPoint(mapLandmark(connection[0])),
                    toD2dPoint(mapLandmark(connection[1])),
                    overlayBrush,
                    2.0F);
            }
            for (int index = 0; index < 21; ++index)
            {
                d2dContext_->FillEllipse(
                    D2D1::Ellipse(toD2dPoint(mapLandmark(index)), 3.5F, 3.5F),
                    overlayBrush);
            }

            const auto topLeft = transforms.uprightToViewport.transform(
                {hand.box.left, hand.box.top});
            const auto bottomRight = transforms.uprightToViewport.transform(
                {hand.box.right, hand.box.bottom});
            d2dContext_->DrawRectangle(
                D2D1::RectF(
                    (std::min)(topLeft.x, bottomRight.x),
                    (std::min)(topLeft.y, bottomRight.y),
                    (std::max)(topLeft.x, bottomRight.x),
                    (std::max)(topLeft.y, bottomRight.y)),
                overlayBrush,
                1.5F);
            if (handIndex == 0 && domainVisible)
            {
                drawStateLabel(
                    displayState,
                    stateName,
                    topLeft,
                    bottomRight,
                    overlayBrush);
            }
        }
    }

    void drawStateLabel(
        const hand_input::recognition::HandStateResult& state,
        const wchar_t* stateName,
        const geometry::Point2f topLeft,
        const geometry::Point2f bottomRight,
        ID2D1SolidColorBrush* stateBrush)
    {
        using hand_input::recognition::HandStatePhase;
        const auto phase = state.phase;
        if (phase != HandStatePhase::Candidate && phase != HandStatePhase::Active)
        {
            return;
        }

        const float roiLeft = (std::min)(topLeft.x, bottomRight.x);
        const float roiRight = (std::max)(topLeft.x, bottomRight.x);
        constexpr float kLabelWidth = 250.0F;
        constexpr float kLabelHeight = 30.0F;
        const auto cameraViewport = createCameraViewport();
        const float cameraRight = static_cast<float>(
            cameraViewport.left + cameraViewport.width);
        const float center = 0.5F * (roiLeft + roiRight);
        const float left = std::clamp(
            center - 0.5F * kLabelWidth,
            6.0F,
            (std::max)(6.0F, cameraRight - kLabelWidth - 6.0F));
        const float roiTop = (std::min)(topLeft.y, bottomRight.y);
        const float desiredTop = roiTop >= kLabelHeight + 10.0F
            ? roiTop - kLabelHeight - 6.0F
            : (std::max)(6.0F, (std::max)(topLeft.y, bottomRight.y) + 6.0F);
        const float top = std::clamp(
            desiredTop,
            6.0F,
            (std::max)(6.0F, static_cast<float>(height_) - kLabelHeight - 6.0F));
        const auto rect = D2D1::RoundedRect(
            D2D1::RectF(left, top, left + kLabelWidth, top + kLabelHeight),
            5.0F,
            5.0F);
        d2dContext_->FillRoundedRectangle(rect, labelBackgroundBrush_.Get());
        d2dContext_->DrawRoundedRectangle(rect, stateBrush, 2.0F);

        wchar_t label[64]{};
        const wchar_t* phaseText = phase == HandStatePhase::Active
            ? L"ACTIVE"
            : L"CHECKING";
        _snwprintf_s(
            label,
            _TRUNCATE,
            L"%s  %s  %.0f%%",
            stateName,
            phaseText,
            static_cast<double>(state.confidence * 100.0F));
        d2dContext_->DrawText(
            label,
            static_cast<UINT32>(std::wcslen(label)),
            textFormat_.Get(),
            rect.rect,
            labelTextBrush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    [[nodiscard]] ViewportRect createCameraViewport() const noexcept
    {
        constexpr std::uint32_t kMinimumSplitWidth = 480;
        if (width_ < kMinimumSplitWidth)
        {
            return {0, 0, width_, height_};
        }
        const auto cameraWidth = static_cast<std::uint32_t>(
            static_cast<double>(width_) * 0.70);
        return {0, 0, (std::max)(cameraWidth, 1U), height_};
    }

    [[nodiscard]] PlotViewport createPlotViewport(
        const ViewportRect cameraViewport) const noexcept
    {
        if (cameraViewport.width >= width_)
        {
            return {};
        }
        constexpr float kPadding = 12.0F;
        return {
            static_cast<float>(cameraViewport.width) + kPadding,
            kPadding,
            static_cast<float>(width_) - kPadding,
            static_cast<float>(height_) - kPadding};
    }

    void drawHand3dPlot(const RenderPacket& packet, const PlotViewport& viewport)
    {
        if (viewport.width() <= 0.0F || viewport.height() <= 0.0F)
        {
            return;
        }

        d2dContext_->DrawRectangle(
            D2D1::RectF(viewport.left, viewport.top, viewport.right, viewport.bottom),
            plotGridBrush_.Get(),
            1.0F);

        constexpr int kGridHalfSteps = 3;
        const float gridStep = hand3dView_.halfExtent / static_cast<float>(kGridHalfSteps);
        for (int step = -kGridHalfSteps; step <= kGridHalfSteps; ++step)
        {
            const float offset = static_cast<float>(step) * gridStep;
            const auto horizontalStart = projectHand3dPoint(
                {-hand3dView_.halfExtent, offset, 0.0F}, viewport, hand3dView_);
            const auto horizontalEnd = projectHand3dPoint(
                {hand3dView_.halfExtent, offset, 0.0F}, viewport, hand3dView_);
            const auto verticalStart = projectHand3dPoint(
                {offset, -hand3dView_.halfExtent, 0.0F}, viewport, hand3dView_);
            const auto verticalEnd = projectHand3dPoint(
                {offset, hand3dView_.halfExtent, 0.0F}, viewport, hand3dView_);
            d2dContext_->DrawLine(
                toD2dPoint(horizontalStart.position),
                toD2dPoint(horizontalEnd.position),
                plotGridBrush_.Get(),
                0.7F);
            d2dContext_->DrawLine(
                toD2dPoint(verticalStart.position),
                toD2dPoint(verticalEnd.position),
                plotGridBrush_.Get(),
                0.7F);
        }

        const auto axisOrigin = projectHand3dPoint({}, viewport, hand3dView_);
        const auto axisX = projectHand3dPoint({0.07F, 0.0F, 0.0F}, viewport, hand3dView_);
        const auto axisY = projectHand3dPoint({0.0F, 0.07F, 0.0F}, viewport, hand3dView_);
        const auto axisZ = projectHand3dPoint({0.0F, 0.0F, 0.07F}, viewport, hand3dView_);
        d2dContext_->DrawLine(toD2dPoint(axisOrigin.position), toD2dPoint(axisX.position),
            plotXAxisBrush_.Get(), 2.0F);
        d2dContext_->DrawLine(toD2dPoint(axisOrigin.position), toD2dPoint(axisY.position),
            plotYAxisBrush_.Get(), 2.0F);
        d2dContext_->DrawLine(toD2dPoint(axisOrigin.position), toD2dPoint(axisZ.position),
            plotZAxisBrush_.Get(), 2.0F);

        const auto projection = projectHand3d(packet.perception.hand, viewport, hand3dView_);
        if (!projection.valid)
        {
            return;
        }

        struct ProjectedBone
        {
            int start{0};
            int end{0};
            float depth{0.0F};
        };
        std::array<ProjectedBone, kHandConnections.size()> bones{};
        for (std::size_t index = 0; index < kHandConnections.size(); ++index)
        {
            const auto connection = kHandConnections[index];
            bones[index] = {connection[0], connection[1],
                (projection.points[connection[0]].depth
                    + projection.points[connection[1]].depth) * 0.5F};
        }
        std::sort(bones.begin(), bones.end(), [](const auto& left, const auto& right)
        {
            return left.depth < right.depth;
        });
        for (const auto& bone : bones)
        {
            d2dContext_->DrawLine(
                toD2dPoint(projection.points[bone.start].position),
                toD2dPoint(projection.points[bone.end].position),
                handBrush_.Get(),
                2.0F);
        }

        if (projection.hasPalmDirection)
        {
            d2dContext_->DrawLine(
                toD2dPoint(projection.palmCenter.position),
                toD2dPoint(projection.palmDirectionTip.position),
                palmDirectionBrush_.Get(),
                3.0F);
            d2dContext_->DrawEllipse(
                D2D1::Ellipse(toD2dPoint(projection.palmCenter.position), 5.0F, 5.0F),
                palmDirectionBrush_.Get(),
                2.0F);
            d2dContext_->FillEllipse(
                D2D1::Ellipse(
                    toD2dPoint(projection.palmDirectionTip.position), 5.0F, 5.0F),
                palmDirectionBrush_.Get());
        }

        std::array<int, 21> pointOrder{};
        for (int index = 0; index < static_cast<int>(pointOrder.size()); ++index)
        {
            pointOrder[static_cast<std::size_t>(index)] = index;
        }
        std::sort(pointOrder.begin(), pointOrder.end(), [&projection](const int left, const int right)
        {
            return projection.points[left].depth < projection.points[right].depth;
        });
        for (const int index : pointOrder)
        {
            const auto& point = projection.points[index];
            const float radius = std::clamp(4.2F - point.depth * 10.0F, 3.0F, 5.5F);
            d2dContext_->FillEllipse(
                D2D1::Ellipse(toD2dPoint(point.position), radius, radius),
                handBrush_.Get());
        }
    }

    HWND hwnd_{nullptr};
    std::uint32_t width_{0};
    std::uint32_t height_{0};
    std::uint32_t cameraWidth_{0};
    std::uint32_t cameraHeight_{0};
    bool cameraBitmapIsGpuSurface_{false};
    std::optional<std::uint64_t> uploadedFrameId_;
    Hand3dView hand3dView_{};
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    std::shared_ptr<runtime::D3d11Device> sharedDevice_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    ComPtr<ID2D1Bitmap1> targetBitmap_;
    ComPtr<ID2D1Bitmap1> cameraBitmap_;
    ComPtr<ID3D11Texture2D> gpuCopyTexture_;
    ComPtr<ID2D1SolidColorBrush> palmBrush_;
    ComPtr<ID2D1SolidColorBrush> handBrush_;
    ComPtr<ID2D1SolidColorBrush> candidateBrush_;
    ComPtr<ID2D1SolidColorBrush> activeBrush_;
    ComPtr<ID2D1SolidColorBrush> labelBackgroundBrush_;
    ComPtr<ID2D1SolidColorBrush> labelTextBrush_;
    ComPtr<ID2D1SolidColorBrush> plotGridBrush_;
    ComPtr<ID2D1SolidColorBrush> plotXAxisBrush_;
    ComPtr<ID2D1SolidColorBrush> plotYAxisBrush_;
    ComPtr<ID2D1SolidColorBrush> plotZAxisBrush_;
    ComPtr<ID2D1SolidColorBrush> palmDirectionBrush_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IDWriteTextFormat> textFormat_;
};

D3d11D2dRenderer::D3d11D2dRenderer() : impl_{std::make_unique<Impl>()} {}
D3d11D2dRenderer::~D3d11D2dRenderer() = default;

bool D3d11D2dRenderer::initialize(
    std::shared_ptr<runtime::D3d11Device> d3dDevice,
    const HWND hwnd,
    const std::uint32_t width,
    const std::uint32_t height,
    std::string& error)
{
    return impl_->initialize(std::move(d3dDevice), hwnd, width, height, error);
}

bool D3d11D2dRenderer::resize(
    const std::uint32_t width,
    const std::uint32_t height,
    std::string& error)
{
    return impl_->resize(width, height, error);
}

void D3d11D2dRenderer::setHand3dView(const Hand3dView view) noexcept
{
    impl_->setHand3dView(view);
}

bool D3d11D2dRenderer::render(
    const RenderPacket& packet,
    RenderPresentation& presentation,
    std::string& error)
{
    if (packet.frame == nullptr)
    {
        error = "Render packet has no frame.";
        return false;
    }
    std::lock_guard gpuAccessLock{packet.frame->gpuAccessMutex()};
    return impl_->render(packet, presentation, error);
}
}
