#include "Rendering/cad_renderer.h"

#include "Runtime/d3d11_device.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <vector>

namespace ryoiki::rendering
{
namespace
{
using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct Vertex
{
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT4 color;
};

struct Constants
{
    XMFLOAT4X4 worldViewProjection;
    XMFLOAT4X4 world;
    XMFLOAT4 lightDirection;
    XMFLOAT4 lineColor;
};

std::string hresultMessage(const char* operation, const HRESULT result)
{
    std::ostringstream stream;
    stream << operation << " failed with HRESULT 0x"
        << std::hex << static_cast<unsigned long>(result) << '.';
    return stream.str();
}

void addFace(
    std::vector<Vertex>& vertices,
    std::vector<std::uint32_t>& triangles,
    std::vector<std::uint32_t>& edges,
    const std::array<XMFLOAT3, 4>& points,
    const XMFLOAT3 normal,
    const XMFLOAT4 color)
{
    const auto base = static_cast<std::uint32_t>(vertices.size());
    for (const auto point : points) vertices.push_back({point, normal, color});
    triangles.insert(triangles.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    edges.insert(edges.end(), {base, base + 1, base + 1, base + 2,
        base + 2, base + 3, base + 3, base});
}

void addBox(
    std::vector<Vertex>& vertices,
    std::vector<std::uint32_t>& triangles,
    std::vector<std::uint32_t>& edges,
    const XMFLOAT3 minimum,
    const XMFLOAT3 maximum,
    const XMFLOAT4 color)
{
    const float x0 = minimum.x, y0 = minimum.y, z0 = minimum.z;
    const float x1 = maximum.x, y1 = maximum.y, z1 = maximum.z;
    addFace(vertices, triangles, edges, {{{x0,y0,z0},{x0,y1,z0},{x1,y1,z0},{x1,y0,z0}}}, {0,0,-1}, color);
    addFace(vertices, triangles, edges, {{{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},{x0,y0,z1}}}, {0,0,1}, color);
    addFace(vertices, triangles, edges, {{{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{x0,y0,z0}}}, {-1,0,0}, color);
    addFace(vertices, triangles, edges, {{{x1,y0,z0},{x1,y1,z0},{x1,y1,z1},{x1,y0,z1}}}, {1,0,0}, color);
    addFace(vertices, triangles, edges, {{{x0,y1,z0},{x0,y1,z1},{x1,y1,z1},{x1,y1,z0}}}, {0,1,0}, color);
    addFace(vertices, triangles, edges, {{{x0,y0,z1},{x0,y0,z0},{x1,y0,z0},{x1,y0,z1}}}, {0,-1,0}, color);
}

void addCylinder(
    std::vector<Vertex>& vertices,
    std::vector<std::uint32_t>& triangles,
    std::vector<std::uint32_t>& edges,
    const float centerX,
    const float centerZ,
    const float bottom,
    const float top,
    const float radius,
    const XMFLOAT4 color)
{
    constexpr std::uint32_t kSegments = 24;
    constexpr float kTau = 6.28318530718F;
    for (std::uint32_t segment = 0; segment < kSegments; ++segment)
    {
        const float a0 = kTau * static_cast<float>(segment) / static_cast<float>(kSegments);
        const float a1 = kTau * static_cast<float>(segment + 1) / static_cast<float>(kSegments);
        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);
        const auto base = static_cast<std::uint32_t>(vertices.size());
        vertices.push_back({{centerX + c0*radius,bottom,centerZ + s0*radius},{c0,0,s0},color});
        vertices.push_back({{centerX + c0*radius,top,centerZ + s0*radius},{c0,0,s0},color});
        vertices.push_back({{centerX + c1*radius,top,centerZ + s1*radius},{c1,0,s1},color});
        vertices.push_back({{centerX + c1*radius,bottom,centerZ + s1*radius},{c1,0,s1},color});
        triangles.insert(triangles.end(), {base,base+1,base+2,base,base+2,base+3});
        edges.insert(edges.end(), {base,base+1,base,base+3,base+1,base+2});

        const auto cap = static_cast<std::uint32_t>(vertices.size());
        vertices.push_back({{centerX,bottom,centerZ},{0,-1,0},color});
        vertices.push_back({{centerX + c1*radius,bottom,centerZ + s1*radius},{0,-1,0},color});
        vertices.push_back({{centerX + c0*radius,bottom,centerZ + s0*radius},{0,-1,0},color});
        vertices.push_back({{centerX,top,centerZ},{0,1,0},color});
        vertices.push_back({{centerX + c0*radius,top,centerZ + s0*radius},{0,1,0},color});
        vertices.push_back({{centerX + c1*radius,top,centerZ + s1*radius},{0,1,0},color});
        triangles.insert(triangles.end(), {cap,cap+1,cap+2,cap+3,cap+4,cap+5});
    }
}

template<typename T>
bool createBuffer(
    ID3D11Device* device,
    const std::vector<T>& values,
    const UINT bindFlags,
    ComPtr<ID3D11Buffer>& buffer,
    std::string& error)
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = static_cast<UINT>(values.size() * sizeof(T));
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = bindFlags;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = values.data();
    const HRESULT result = device->CreateBuffer(&description, &data, &buffer);
    if (FAILED(result)) error = hresultMessage("ID3D11Device::CreateBuffer", result);
    return SUCCEEDED(result);
}
}

class CadRenderer::Impl final
{
public:
    bool initialize(
        std::shared_ptr<runtime::D3d11Device> device,
        const HWND hwnd,
        const std::uint32_t width,
        const std::uint32_t height,
        std::string& error)
    {
        deviceOwner_ = std::move(device);
        if (deviceOwner_ == nullptr) { error = "CAD renderer requires a D3D11 device."; return false; }
        hwnd_ = hwnd;
        width_ = width;
        height_ = height;
        device_ = deviceOwner_->device();
        context_ = deviceOwner_->immediateContext();
        return createPipeline(error) && createMesh(error) && createSizeResources(error);
    }

    bool resize(const std::uint32_t width, const std::uint32_t height, std::string& error)
    {
        width_ = width; height_ = height;
        if (width == 0 || height == 0) return true;
        renderTarget_.Reset(); depthView_.Reset(); depthTexture_.Reset();
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        const HRESULT result = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(result)) { error = hresultMessage("IDXGISwapChain::ResizeBuffers", result); return false; }
        return createSizeResources(error);
    }

    void setView(CadView view) noexcept
    {
        view.pitchRadians = std::clamp(view.pitchRadians, -1.48353F, 1.48353F);
        view.zoom = std::clamp(view.zoom, 0.35F, 4.0F);
        view.panX = std::clamp(view.panX, -4.0F, 4.0F);
        view.panY = std::clamp(view.panY, -4.0F, 4.0F);
        view_ = view;
    }

    bool render(std::string& error)
    {
        if (width_ == 0 || height_ == 0) return true;
        std::scoped_lock contextLock{deviceOwner_->immediateContextMutex()};
        constexpr float background[]{0.035F, 0.047F, 0.065F, 1.0F};
        context_->ClearRenderTargetView(renderTarget_.Get(), background);
        context_->ClearDepthStencilView(depthView_.Get(), D3D11_CLEAR_DEPTH, 1.0F, 0);
        context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), depthView_.Get());
        D3D11_VIEWPORT viewport{0,0,static_cast<float>(width_),static_cast<float>(height_),0,1};
        context_->RSSetViewports(1, &viewport);
        context_->IASetInputLayout(inputLayout_.Get());
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        context_->VSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());

        const XMMATRIX world = XMMatrixRotationX(view_.pitchRadians) * XMMatrixRotationY(view_.yawRadians);
        const XMMATRIX view = XMMatrixLookAtLH(
            XMVectorSet(view_.panX, 1.4F + view_.panY, -6.0F / view_.zoom, 1.0F),
            XMVectorSet(view_.panX, 0.25F + view_.panY, 0.0F, 1.0F),
            XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F));
        const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
        const XMMATRIX projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(42.0F), aspect, 0.05F, 100.0F);
        Constants constants{};
        XMStoreFloat4x4(&constants.worldViewProjection, XMMatrixTranspose(world * view * projection));
        XMStoreFloat4x4(&constants.world, XMMatrixTranspose(world));
        constants.lightDirection = {-0.35F, -0.75F, -0.55F, 0.0F};
        constants.lineColor = {0,0,0,0};
        context_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &constants, 0, 0);

        constexpr UINT stride = sizeof(Vertex), offset = 0;
        context_->IASetVertexBuffers(0, 1, gridVertexBuffer_.GetAddressOf(), &stride, &offset);
        context_->IASetIndexBuffer(gridIndexBuffer_.Get(), DXGI_FORMAT_R32_UINT, 0);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        context_->RSSetState(wireRasterizer_.Get());
        context_->OMSetDepthStencilState(depthEnabled_.Get(), 0);
        context_->DrawIndexed(gridCount_, 0, 0);

        context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
        context_->IASetIndexBuffer(triangleBuffer_.Get(), DXGI_FORMAT_R32_UINT, 0);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->RSSetState(solidRasterizer_.Get());
        context_->OMSetDepthStencilState(depthEnabled_.Get(), 0);
        context_->DrawIndexed(triangleCount_, 0, 0);

        context_->IASetIndexBuffer(edgeBuffer_.Get(), DXGI_FORMAT_R32_UINT, 0);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        context_->RSSetState(wireRasterizer_.Get());
        context_->OMSetDepthStencilState(depthDisabled_.Get(), 0);
        constants.lineColor = {0.08F,0.82F,0.92F,1.0F};
        context_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &constants, 0, 0);
        context_->DrawIndexed(edgeCount_, 0, 0);

        const HRESULT result = swapChain_->Present(1, 0);
        if (FAILED(result)) { error = hresultMessage("IDXGISwapChain::Present", result); return false; }
        return true;
    }

private:
    bool createPipeline(std::string& error)
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT result = device_.As(&dxgiDevice);
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(result)) result = dxgiDevice->GetAdapter(&adapter);
        ComPtr<IDXGIFactory2> factory;
        if (SUCCEEDED(result)) result = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(result)) { error = hresultMessage("Get DXGI factory", result); return false; }
        DXGI_SWAP_CHAIN_DESC1 swap{};
        swap.Width = width_; swap.Height = height_; swap.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swap.SampleDesc.Count = 1; swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap.BufferCount = 2; swap.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap.Scaling = DXGI_SCALING_STRETCH; swap.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        result = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &swap, nullptr, nullptr, &swapChain_);
        if (FAILED(result)) { error = hresultMessage("Create CAD swap chain", result); return false; }

        static constexpr char kShader[] = R"(
cbuffer Scene : register(b0) { float4x4 worldViewProjection; float4x4 world; float4 lightDirection; float4 lineColor; };
struct VSIn { float3 position:POSITION; float3 normal:NORMAL; float4 color:COLOR; };
struct VSOut { float4 position:SV_POSITION; float3 normal:NORMAL; float4 color:COLOR; };
VSOut VSMain(VSIn value) { VSOut o; o.position=mul(float4(value.position,1),worldViewProjection); o.normal=mul(float4(value.normal,0),world).xyz; o.color=value.color; return o; }
float4 PSMain(VSOut value):SV_TARGET { if (lineColor.a>0.5) return lineColor; float lighting=0.32+0.68*saturate(dot(normalize(value.normal),-normalize(lightDirection.xyz))); return float4(value.color.rgb*lighting,value.color.a); }
)";
        ComPtr<ID3DBlob> vsBlob, psBlob, messages;
        result = D3DCompile(kShader, sizeof(kShader), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, &messages);
        if (FAILED(result))
        {
            error = "CAD vertex shader compilation failed: ";
            if (messages != nullptr)
            {
                error.append(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
            }
            return false;
        }
        result = D3DCompile(kShader, sizeof(kShader), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, &messages);
        if (FAILED(result))
        {
            error = "CAD pixel shader compilation failed: ";
            if (messages != nullptr)
            {
                error.append(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
            }
            return false;
        }
        result = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader_);
        if (SUCCEEDED(result)) result = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader_);
        constexpr D3D11_INPUT_ELEMENT_DESC elements[]{
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(Vertex,position),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(Vertex,normal),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,offsetof(Vertex,color),D3D11_INPUT_PER_VERTEX_DATA,0}};
        if (SUCCEEDED(result)) result = device_->CreateInputLayout(elements, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout_);
        D3D11_BUFFER_DESC constant{}; constant.ByteWidth = sizeof(Constants); constant.Usage = D3D11_USAGE_DEFAULT; constant.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(result)) result = device_->CreateBuffer(&constant, nullptr, &constantBuffer_);
        D3D11_RASTERIZER_DESC raster{}; raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_BACK; raster.FrontCounterClockwise=FALSE; raster.DepthClipEnable=TRUE;
        if (SUCCEEDED(result)) result = device_->CreateRasterizerState(&raster, &solidRasterizer_);
        raster.CullMode=D3D11_CULL_NONE;
        if (SUCCEEDED(result)) result = device_->CreateRasterizerState(&raster, &wireRasterizer_);
        D3D11_DEPTH_STENCIL_DESC depth{}; depth.DepthEnable=TRUE; depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL; depth.DepthFunc=D3D11_COMPARISON_LESS;
        if (SUCCEEDED(result)) result = device_->CreateDepthStencilState(&depth, &depthEnabled_);
        depth.DepthEnable=FALSE;
        if (SUCCEEDED(result)) result = device_->CreateDepthStencilState(&depth, &depthDisabled_);
        if (FAILED(result)) { error = hresultMessage("Create CAD pipeline", result); return false; }
        return true;
    }

    bool createMesh(std::string& error)
    {
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> triangles, edges;
        const XMFLOAT4 blue{0.20F,0.58F,0.88F,1.0F};
        const XMFLOAT4 steel{0.42F,0.72F,0.82F,1.0F};
        addBox(vertices,triangles,edges,{-1.8F,-0.35F,-1.15F},{1.8F,0.0F,1.15F},blue);
        addBox(vertices,triangles,edges,{-1.45F,0.0F,-0.22F},{-0.75F,1.65F,0.22F},steel);
        addBox(vertices,triangles,edges,{0.75F,0.0F,-0.22F},{1.45F,1.05F,0.22F},steel);
        addCylinder(vertices,triangles,edges,0.0F,0.0F,0.0F,0.65F,0.58F,{0.92F,0.56F,0.20F,1.0F});
        triangleCount_ = static_cast<UINT>(triangles.size()); edgeCount_ = static_cast<UINT>(edges.size());
        std::vector<Vertex> gridVertices;
        std::vector<std::uint32_t> gridIndices;
        const auto addGridLine = [&](const XMFLOAT3 from, const XMFLOAT3 to, const XMFLOAT4 color)
        {
            const auto index = static_cast<std::uint32_t>(gridVertices.size());
            gridVertices.push_back({from,{0,1,0},color});
            gridVertices.push_back({to,{0,1,0},color});
            gridIndices.insert(gridIndices.end(), {index,index+1});
        };
        constexpr float kGridY = -0.38F;
        for (int step = -6; step <= 6; ++step)
        {
            const float coordinate = static_cast<float>(step) * 0.5F;
            const XMFLOAT4 color = step == 0
                ? XMFLOAT4{0.30F,0.38F,0.48F,1.0F}
                : XMFLOAT4{0.13F,0.18F,0.24F,1.0F};
            addGridLine({-3.0F,kGridY,coordinate},{3.0F,kGridY,coordinate},color);
            addGridLine({coordinate,kGridY,-3.0F},{coordinate,kGridY,3.0F},color);
        }
        addGridLine({0,kGridY,0},{2.7F,kGridY,0},{0.95F,0.20F,0.20F,1});
        addGridLine({0,kGridY,0},{0,2.4F,0},{0.25F,0.92F,0.38F,1});
        addGridLine({0,kGridY,0},{0,kGridY,2.7F},{0.20F,0.48F,1.0F,1});
        gridCount_ = static_cast<UINT>(gridIndices.size());
        return createBuffer(device_.Get(),vertices,D3D11_BIND_VERTEX_BUFFER,vertexBuffer_,error)
            && createBuffer(device_.Get(),triangles,D3D11_BIND_INDEX_BUFFER,triangleBuffer_,error)
            && createBuffer(device_.Get(),edges,D3D11_BIND_INDEX_BUFFER,edgeBuffer_,error)
            && createBuffer(device_.Get(),gridVertices,D3D11_BIND_VERTEX_BUFFER,gridVertexBuffer_,error)
            && createBuffer(device_.Get(),gridIndices,D3D11_BIND_INDEX_BUFFER,gridIndexBuffer_,error);
    }

    bool createSizeResources(std::string& error)
    {
        if (width_ == 0 || height_ == 0) return true;
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT result = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (SUCCEEDED(result)) result = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_);
        D3D11_TEXTURE2D_DESC depth{}; depth.Width=width_; depth.Height=height_; depth.MipLevels=1; depth.ArraySize=1; depth.Format=DXGI_FORMAT_D32_FLOAT; depth.SampleDesc.Count=1; depth.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        if (SUCCEEDED(result)) result = device_->CreateTexture2D(&depth, nullptr, &depthTexture_);
        if (SUCCEEDED(result)) result = device_->CreateDepthStencilView(depthTexture_.Get(), nullptr, &depthView_);
        if (FAILED(result)) { error = hresultMessage("Create CAD size resources", result); return false; }
        return true;
    }

    std::shared_ptr<runtime::D3d11Device> deviceOwner_;
    ComPtr<ID3D11Device> device_; ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_; ComPtr<ID3D11RenderTargetView> renderTarget_;
    ComPtr<ID3D11Texture2D> depthTexture_; ComPtr<ID3D11DepthStencilView> depthView_;
    ComPtr<ID3D11VertexShader> vertexShader_; ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_; ComPtr<ID3D11Buffer> constantBuffer_;
    ComPtr<ID3D11Buffer> vertexBuffer_, triangleBuffer_, edgeBuffer_, gridVertexBuffer_, gridIndexBuffer_;
    ComPtr<ID3D11RasterizerState> solidRasterizer_, wireRasterizer_;
    ComPtr<ID3D11DepthStencilState> depthEnabled_, depthDisabled_;
    HWND hwnd_{}; std::uint32_t width_{0}, height_{0}; UINT triangleCount_{0}, edgeCount_{0}, gridCount_{0};
    CadView view_{};
};

CadRenderer::CadRenderer() : impl_{std::make_unique<Impl>()} {}
CadRenderer::~CadRenderer() = default;
bool CadRenderer::initialize(std::shared_ptr<runtime::D3d11Device> device, const HWND hwnd, const std::uint32_t width, const std::uint32_t height, std::string& error) { return impl_->initialize(std::move(device),hwnd,width,height,error); }
bool CadRenderer::resize(const std::uint32_t width, const std::uint32_t height, std::string& error) { return impl_->resize(width,height,error); }
void CadRenderer::setView(const CadView view) noexcept { impl_->setView(view); }
bool CadRenderer::render(std::string& error) { return impl_->render(error); }
}
