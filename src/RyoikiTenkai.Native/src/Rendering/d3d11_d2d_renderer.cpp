#include "Rendering/d3d11_d2d_renderer.h"

#include "Rendering/frame_transforms.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <optional>
#include <sstream>

namespace ryoiki::rendering
{
namespace
{
using Microsoft::WRL::ComPtr;

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
        const HWND hwnd,
        const std::uint32_t width,
        const std::uint32_t height,
        std::string& error)
    {
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

private:
    bool createDeviceResources(std::string& error)
    {
        cameraBitmap_.Reset();
        palmBrush_.Reset();
        handBrush_.Reset();
        targetBitmap_.Reset();
        d2dContext_.Reset();
        d2dDevice_.Reset();
        d2dFactory_.Reset();
        swapChain_.Reset();
        d3dContext_.Reset();
        d3dDevice_.Reset();
        cameraWidth_ = 0;
        cameraHeight_ = 0;
        uploadedFrameId_.reset();

        constexpr std::array<D3D_FEATURE_LEVEL, 2> kFeatureLevels{
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selectedFeatureLevel{};
        HRESULT result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            kFeatureLevels.data(),
            static_cast<UINT>(kFeatureLevels.size()),
            D3D11_SDK_VERSION,
            &d3dDevice_,
            &selectedFeatureLevel,
            &d3dContext_);
        if (FAILED(result))
        {
            error = hresultMessage("D3D11CreateDevice", result);
            return false;
        }

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

        result = d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.0F, 1.0F, 0.70F), &palmBrush_);
        if (FAILED(result))
        {
            error = hresultMessage("Create palm brush", result);
            return false;
        }
        result = d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(1.0F, 0.82F, 0.27F), &handBrush_);
        if (FAILED(result))
        {
            error = hresultMessage("Create hand brush", result);
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
        if (cameraBitmap_ != nullptr && cameraWidth_ == width && cameraHeight_ == height)
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
        return true;
    }

    HRESULT renderOnce(
        const RenderPacket& packet,
        RenderPresentation& presentation,
        std::string& error)
    {
        using clock = std::chrono::steady_clock;
        const auto started = clock::now();
        if (packet.frame == nullptr || packet.frame->pixels().empty()
            || width_ == 0 || height_ == 0)
        {
            error = "The render packet does not contain a drawable frame.";
            return E_INVALIDARG;
        }
        const auto& frame = *packet.frame;
        if (!ensureCameraBitmap(frame.width(), frame.height(), error))
        {
            return E_FAIL;
        }
        HRESULT result = S_OK;
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

        FrameTransforms transforms{};
        if (!createFrameTransforms(
                frame.width(),
                frame.height(),
                frame.orientation(),
                width_,
                height_,
                true,
                transforms))
        {
            error = "Failed to calculate frame-to-viewport transforms.";
            return E_INVALIDARG;
        }

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

        d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());
        drawPalmOverlay(packet, transforms);
        drawHandOverlay(packet, transforms);
        result = d2dContext_->EndDraw();
        if (FAILED(result))
        {
            error = hresultMessage("ID2D1DeviceContext::EndDraw", result);
            return result;
        }
        result = swapChain_->Present(1, 0);
        if (FAILED(result))
        {
            error = hresultMessage("IDXGISwapChain::Present", result);
            return result;
        }

        const auto completed = clock::now();
        presentation.frameId = frame.frameId();
        presentation.captureTimestampUs = frame.captureTimestampUs();
        presentation.renderMs = std::chrono::duration<double, std::milli>(
            completed - started).count();
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
        const auto& hand = packet.perception.hand;
        if (!hand.detected)
        {
            return;
        }
        constexpr std::array<std::array<int, 2>, 23> kConnections{{
            {0, 1}, {1, 2}, {2, 3}, {3, 4},
            {0, 5}, {5, 6}, {6, 7}, {7, 8},
            {0, 9}, {9, 10}, {10, 11}, {11, 12},
            {0, 13}, {13, 14}, {14, 15}, {15, 16},
            {0, 17}, {17, 18}, {18, 19}, {19, 20},
            {5, 9}, {9, 13}, {13, 17}}};

        const auto mapLandmark = [&hand, &transforms](const int index)
        {
            return transforms.uprightToViewport.transform(
                {hand.landmarks[index].x, hand.landmarks[index].y});
        };
        for (const auto& connection : kConnections)
        {
            d2dContext_->DrawLine(
                toD2dPoint(mapLandmark(connection[0])),
                toD2dPoint(mapLandmark(connection[1])),
                handBrush_.Get(),
                2.0F);
        }
        for (int index = 0; index < 21; ++index)
        {
            d2dContext_->FillEllipse(
                D2D1::Ellipse(toD2dPoint(mapLandmark(index)), 3.5F, 3.5F),
                handBrush_.Get());
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
            handBrush_.Get(),
            1.5F);
    }

    HWND hwnd_{nullptr};
    std::uint32_t width_{0};
    std::uint32_t height_{0};
    std::uint32_t cameraWidth_{0};
    std::uint32_t cameraHeight_{0};
    std::optional<std::uint64_t> uploadedFrameId_;
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    ComPtr<ID2D1Bitmap1> targetBitmap_;
    ComPtr<ID2D1Bitmap1> cameraBitmap_;
    ComPtr<ID2D1SolidColorBrush> palmBrush_;
    ComPtr<ID2D1SolidColorBrush> handBrush_;
};

D3d11D2dRenderer::D3d11D2dRenderer() : impl_{std::make_unique<Impl>()} {}
D3d11D2dRenderer::~D3d11D2dRenderer() = default;

bool D3d11D2dRenderer::initialize(
    const HWND hwnd,
    const std::uint32_t width,
    const std::uint32_t height,
    std::string& error)
{
    return impl_->initialize(hwnd, width, height, error);
}

bool D3d11D2dRenderer::resize(
    const std::uint32_t width,
    const std::uint32_t height,
    std::string& error)
{
    return impl_->resize(width, height, error);
}

bool D3d11D2dRenderer::render(
    const RenderPacket& packet,
    RenderPresentation& presentation,
    std::string& error)
{
    return impl_->render(packet, presentation, error);
}
}
