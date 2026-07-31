#include "Rendering/gesture_dtw_debug_renderer.h"

#include "Rendering/gesture_dtw_debug_layout.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace ryoiki::rendering
{
using Microsoft::WRL::ComPtr;
namespace
{
constexpr std::array<std::pair<std::size_t, std::size_t>, 21> kConnections{{
    {0,1},{1,2},{2,3},{3,4},{0,5},{5,6},{6,7},{7,8},{5,9},{9,10},{10,11},
    {11,12},{9,13},{13,14},{14,15},{15,16},{13,17},{17,18},{18,19},{19,20},{0,17}}};

bool deviceLost(const HRESULT value) noexcept
{
    return value == DXGI_ERROR_DEVICE_REMOVED || value == DXGI_ERROR_DEVICE_RESET
        || value == D2DERR_RECREATE_TARGET;
}

std::string failure(const char* operation, const HRESULT value)
{
    return std::string{operation} + " failed (HRESULT " + std::to_string(value) + ").";
}
}

class GestureDtwDebugRenderer::Impl final
{
public:
    bool initialize(std::shared_ptr<runtime::D3d11Device> device, const HWND hwnd,
        const std::uint32_t width, const std::uint32_t height, std::string& error)
    {
        device_ = std::move(device); hwnd_ = hwnd; width_ = width; height_ = height;
        if (device_ == nullptr || hwnd_ == nullptr) { error = "DTW renderer requires a device and HWND."; return false; }
        return createResources(error);
    }

    bool resize(const std::uint32_t width, const std::uint32_t height, std::string& error)
    {
        width_ = width; height_ = height;
        if (width == 0 || height == 0) return true;
        if (swapChain_ == nullptr) return createResources(error);
        context_->SetTarget(nullptr); target_.Reset();
        const auto result = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(result))
        {
            if (deviceLost(result)) return createResources(error);
            error = failure("ResizeBuffers", result); return false;
        }
        return createTarget(error);
    }

    bool render(const GestureDtwDebugRenderPacket& packet, std::string& error)
    {
        if (!validateGestureDtwDebugRenderPacket(packet)) { error = "Invalid DTW render packet."; return false; }
        auto result = renderOnce(packet, error);
        if (!deviceLost(result)) return SUCCEEDED(result);
        error.clear();
        if (!createResources(error)) return false;
        result = renderOnce(packet, error);
        return SUCCEEDED(result);
    }

private:
    bool createResources(std::string& error)
    {
        candidateBrush_.Reset(); templateBrush_.Reset(); gridBrush_.Reset(); textBrush_.Reset();
        target_.Reset(); context_.Reset(); d2dDevice_.Reset(); factory_.Reset(); swapChain_.Reset();
        ComPtr<IDXGIDevice> dxgiDevice;
        auto result = device_->device()->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (FAILED(result)) { error = failure("Query IDXGIDevice", result); return false; }
        ComPtr<IDXGIAdapter> adapter; result = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(result)) { error = failure("GetAdapter", result); return false; }
        ComPtr<IDXGIFactory2> dxgiFactory; result = adapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
        if (FAILED(result)) { error = failure("Get DXGI factory", result); return false; }
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width_; description.Height = height_; description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1; description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2; description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        result = dxgiFactory->CreateSwapChainForHwnd(device_->device(), hwnd_, &description, nullptr, nullptr, &swapChain_);
        if (FAILED(result)) { error = failure("CreateSwapChainForHwnd", result); return false; }
        result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
        if (FAILED(result)) { error = failure("D2D1CreateFactory", result); return false; }
        result = factory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
        if (FAILED(result)) { error = failure("Create D2D device", result); return false; }
        result = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_);
        if (FAILED(result)) { error = failure("Create D2D context", result); return false; }
        context_->SetDpi(96, 96);
        if (!createTarget(error)) return false;
        result = context_->CreateSolidColorBrush(D2D1::ColorF(1.0F, 0.72F, 0.10F), &candidateBrush_);
        if (SUCCEEDED(result)) result = context_->CreateSolidColorBrush(D2D1::ColorF(0.10F, 0.70F, 1.0F), &templateBrush_);
        if (SUCCEEDED(result)) result = context_->CreateSolidColorBrush(D2D1::ColorF(0.22F, 0.25F, 0.31F), &gridBrush_);
        if (SUCCEEDED(result)) result = context_->CreateSolidColorBrush(D2D1::ColorF(0.85F, 0.89F, 0.96F), &textBrush_);
        if (FAILED(result)) { error = failure("Create DTW brushes", result); return false; }
        return true;
    }

    bool createTarget(std::string& error)
    {
        ComPtr<IDXGISurface> surface; auto result = swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface));
        if (FAILED(result)) { error = failure("Get swapchain buffer", result); return false; }
        const D2D1_BITMAP_PROPERTIES1 properties{D2D1_PIXEL_FORMAT{DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_IGNORE}, 96, 96, D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, nullptr};
        result = context_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &target_);
        if (FAILED(result)) { error = failure("Create target bitmap", result); return false; }
        context_->SetTarget(target_.Get()); return true;
    }

    static D2D1_POINT_2F mapPoint(const GestureDtwDebugPoint& point,
        const GestureDtwDebugRect& pane, const GestureDtwDebugSequenceFrame* frames, const std::size_t count)
    {
        float minX = point.x, maxX = point.x, minY = point.y, maxY = point.y;
        for (std::size_t i = 0; i < count; ++i) for (const auto& value : frames[i].landmarks)
        { minX = (std::min)(minX,value.x); maxX=(std::max)(maxX,value.x); minY=(std::min)(minY,value.y); maxY=(std::max)(maxY,value.y); }
        const float sx = (pane.width()-24) / (std::max)(0.001F,maxX-minX);
        const float sy = (pane.height()-24) / (std::max)(0.001F,maxY-minY);
        const float scale = (std::min)(sx,sy);
        return {pane.left+12+(point.x-minX)*scale, pane.top+12+(point.y-minY)*scale};
    }

    void drawSequence(const GestureDtwDebugSequenceFrame* frames, const std::size_t count,
        const std::size_t selectedIndex, const GestureDtwDebugRect& pane, ID2D1SolidColorBrush* brush)
    {
        if (count == 0) return;
        const auto& frame=frames[(std::min)(selectedIndex,count-1)];
        for (const auto [a,b] : kConnections)
            context_->DrawLine(mapPoint(frame.landmarks[a],pane,&frame,1), mapPoint(frame.landmarks[b],pane,&frame,1), brush, 2.8F);
        brush->SetOpacity(1.0F);
    }

    void drawPalmTrail(const GestureDtwDebugSequenceFrame* frames,const std::size_t count,
        const GestureDtwDebugRect& pane,ID2D1SolidColorBrush* brush)
    {
        if(count<2)return;float minX=frames[0].palmCenterX,maxX=minX,minY=frames[0].palmCenterY,maxY=minY;
        for(std::size_t i=1;i<count;++i){minX=(std::min)(minX,frames[i].palmCenterX);maxX=(std::max)(maxX,frames[i].palmCenterX);minY=(std::min)(minY,frames[i].palmCenterY);maxY=(std::max)(maxY,frames[i].palmCenterY);}
        const auto map=[&](const auto& f){return D2D1::Point2F(pane.left+10+(f.palmCenterX-minX)/(std::max)(.001F,maxX-minX)*(pane.width()*.22F),pane.bottom-10-(f.palmCenterY-minY)/(std::max)(.001F,maxY-minY)*(pane.height()*.20F));};
        brush->SetOpacity(.28F);for(std::size_t i=1;i<count;++i)context_->DrawLine(map(frames[i-1]),map(frames[i]),brush,1.2F);brush->SetOpacity(1);
    }

    void drawDtwPlot(const GestureDtwDebugRenderPacket& packet,const GestureDtwDebugRect& pane)
    {
        if(packet.candidateFrameCount==0||packet.templateFrameCount==0)return;
        const float pad=16, plotSize=(std::min)(pane.height()-2*pad,pane.width()*.52F),left=pane.left+pad,top=pane.top+pad;
        const float cell=plotSize/32.0F;
        gridBrush_->SetOpacity(.55F);
        for(std::size_t index=0;index<=8;++index){const float offset=plotSize*static_cast<float>(index)/8.0F;context_->DrawLine(D2D1::Point2F(left+offset,top),D2D1::Point2F(left+offset,top+plotSize),gridBrush_.Get(),.8F);context_->DrawLine(D2D1::Point2F(left,top+offset),D2D1::Point2F(left+plotSize,top+offset),gridBrush_.Get(),.8F);}
        gridBrush_->SetOpacity(1.0F);
        context_->DrawLine(D2D1::Point2F(left,top+plotSize),D2D1::Point2F(left+plotSize,top),gridBrush_.Get(),1.8F);
        D2D1_POINT_2F previous{};bool hasPrevious=false;
        for(std::size_t i=0;i<packet.pathCount;++i){const auto point=packet.path[i];const auto current=D2D1::Point2F(left+(point.candidateIndex+.5F)*cell,top+plotSize-(point.templateIndex+.5F)*cell);if(hasPrevious)context_->DrawLine(previous,current,candidateBrush_.Get(),3.2F);previous=current;hasPrevious=true;}
        if(hasPrevious)context_->FillEllipse(D2D1::Ellipse(previous,5.5F,5.5F),templateBrush_.Get());

        // Fig. 7-style companion view: palm velocity is the PR feature that
        // directly describes temporal motion while remaining one-dimensional.
        // The actual multivariate DTW path, not this projection, supplies the
        // correspondence lines.
        const float seriesLeft=left+plotSize+28,seriesRight=pane.right-pad;
        if(seriesRight-seriesLeft<120.0F)return;
        float minimum=packet.candidateFrames[0].palmVelocity,maximum=minimum;
        for(std::size_t i=0;i<32;++i){minimum=(std::min)({minimum,packet.candidateFrames[i].palmVelocity,packet.templateFrames[i].palmVelocity});maximum=(std::max)({maximum,packet.candidateFrames[i].palmVelocity,packet.templateFrames[i].palmVelocity});}
        const float span=(std::max)(.001F,maximum-minimum),upperBase=top+plotSize*.28F,lowerBase=top+plotSize*.72F,amplitude=plotSize*.16F;
        const auto seriesPoint=[&](const bool candidate,const std::size_t index){const auto& frame=candidate?packet.candidateFrames[index]:packet.templateFrames[index];const float x=seriesLeft+(seriesRight-seriesLeft)*static_cast<float>(index)/31.0F;const float base=candidate?upperBase:lowerBase;return D2D1::Point2F(x,base-(frame.palmVelocity-minimum)/span*amplitude);};
        gridBrush_->SetOpacity(.32F);for(std::size_t i=0;i<packet.pathCount;++i){const auto point=packet.path[i];context_->DrawLine(seriesPoint(true,point.candidateIndex),seriesPoint(false,point.templateIndex),gridBrush_.Get(),.8F);}gridBrush_->SetOpacity(1);
        for(std::size_t i=1;i<32;++i){context_->DrawLine(seriesPoint(true,i-1),seriesPoint(true,i),candidateBrush_.Get(),2.2F);context_->DrawLine(seriesPoint(false,i-1),seriesPoint(false,i),templateBrush_.Get(),2.2F);}
        if(packet.pathCount>0){const auto point=packet.path[packet.pathCount-1];context_->FillEllipse(D2D1::Ellipse(seriesPoint(true,point.candidateIndex),4,4),candidateBrush_.Get());context_->FillEllipse(D2D1::Ellipse(seriesPoint(false,point.templateIndex),4,4),templateBrush_.Get());}
    }

    HRESULT renderOnce(const GestureDtwDebugRenderPacket& packet, std::string& error)
    {
        const auto layout=createGestureDtwDebugLayout(static_cast<float>(width_),static_cast<float>(height_));
        if(!layout.valid){error="DTW viewport is too small.";return E_INVALIDARG;}
        context_->BeginDraw(); context_->Clear(D2D1::ColorF(0.025F,0.032F,0.050F));
        for(const auto& pane:{layout.candidate,layout.templateView,layout.diagnostics}) context_->DrawRectangle(D2D1::RectF(pane.left,pane.top,pane.right,pane.bottom),gridBrush_.Get(),1);
        const std::size_t candidateIndex=packet.candidateFrameCount==0?0:packet.candidateFrameCount-1;
        std::size_t templateIndex=packet.templateFrameCount==0?0:(std::min)(candidateIndex,static_cast<std::size_t>(packet.templateFrameCount-1));
        for(std::size_t i=0;i<packet.pathCount;++i)if(packet.path[i].candidateIndex==candidateIndex)templateIndex=packet.path[i].templateIndex;
        if(packet.liveHandValid){GestureDtwDebugSequenceFrame live{};live.palmCenterX=packet.livePalmCenterX;live.palmCenterY=packet.livePalmCenterY;live.landmarks=packet.liveLandmarks;drawSequence(&live,1,0,layout.candidate,candidateBrush_.Get());}
        else drawSequence(packet.candidateFrames.data(),packet.candidateFrameCount,candidateIndex,layout.candidate,candidateBrush_.Get());
        drawSequence(packet.templateFrames.data(),packet.templateFrameCount,templateIndex,layout.templateView,templateBrush_.Get());
        drawPalmTrail(packet.candidateFrames.data(),packet.candidateFrameCount,layout.candidate,candidateBrush_.Get());
        drawPalmTrail(packet.templateFrames.data(),packet.templateFrameCount,layout.templateView,templateBrush_.Get());
        drawDtwPlot(packet,layout.diagnostics);
        auto result=context_->EndDraw(); if(FAILED(result)){error=failure("D2D EndDraw",result);return result;}
        result=swapChain_->Present(1,0); if(FAILED(result))error=failure("Present",result); return result;
    }

    std::shared_ptr<runtime::D3d11Device> device_; HWND hwnd_{}; std::uint32_t width_{},height_{};
    ComPtr<IDXGISwapChain1> swapChain_; ComPtr<ID2D1Factory1> factory_; ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> context_; ComPtr<ID2D1Bitmap1> target_;
    ComPtr<ID2D1SolidColorBrush> candidateBrush_,templateBrush_,gridBrush_,textBrush_;
};

GestureDtwDebugRenderer::GestureDtwDebugRenderer():impl_{std::make_unique<Impl>()}{}
GestureDtwDebugRenderer::~GestureDtwDebugRenderer()=default;
bool GestureDtwDebugRenderer::initialize(std::shared_ptr<runtime::D3d11Device> d,HWND h,std::uint32_t w,std::uint32_t x,std::string& e){return impl_->initialize(std::move(d),h,w,x,e);}
bool GestureDtwDebugRenderer::resize(std::uint32_t w,std::uint32_t h,std::string& e){return impl_->resize(w,h,e);}
bool GestureDtwDebugRenderer::render(const GestureDtwDebugRenderPacket& p,std::string& e){return impl_->render(p,e);}
}
