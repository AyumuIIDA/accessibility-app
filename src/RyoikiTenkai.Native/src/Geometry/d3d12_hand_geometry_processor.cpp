#include "Geometry/d3d12_hand_geometry_processor.h"

#include <d3dcompiler.h>
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
constexpr char kShader[] = R"(
Texture2D<float4> sourceTexture : register(t0);
RWByteAddressBuffer outputTensor : register(u0);
cbuffer Parameters : register(b0) {
 float4 transform0; float2 transform1; uint2 sourceSize; uint2 outputSize;
};
float4 loadOrZero(int2 p) {
 if (p.x < 0 || p.y < 0 || p.x >= int(sourceSize.x) || p.y >= int(sourceSize.y)) return 0;
 return sourceTexture.Load(int3(p, 0));
}
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
 if (id.x >= outputSize.x || id.y >= outputSize.y) return;
 float2 source=float2(transform0.x*id.x+transform0.y*id.y+transform0.z,
  transform0.w*id.x+transform1.x*id.y+transform1.y);
 int2 base=int2(floor(source)); float2 f=source-base;
 float4 top=lerp(loadOrZero(base),loadOrZero(base+int2(1,0)),f.x);
 float4 bottom=lerp(loadOrZero(base+int2(0,1)),loadOrZero(base+int2(1,1)),f.x);
 float4 rgba=lerp(top,bottom,f.y); uint offset=(id.y*outputSize.x+id.x)*3;
 outputTensor.Store(offset*4,asuint(rgba.r));
 outputTensor.Store((offset+1)*4,asuint(rgba.g));
 outputTensor.Store((offset+2)*4,asuint(rgba.b));
})";

struct Parameters { float transform[6]{}; std::uint32_t source[2]{}; std::uint32_t output[2]{}; };

std::string failed(const char* operation, HRESULT hr) {
 std::ostringstream s; s << operation << " failed with HRESULT 0x" << std::hex
  << static_cast<unsigned long>(hr) << '.'; return s.str();
}

AffineTransform palmTransform(const buffers::FrameBuffer& frame, PalmPreprocessResult& result) {
 constexpr float size=192.0F; const float ratio=(std::min)(size/frame.uprightWidth(),size/frame.uprightHeight());
 const int width=(std::max)(1,static_cast<int>(frame.uprightWidth()*ratio));
 const int height=(std::max)(1,static_cast<int>(frame.uprightHeight()*ratio));
 const int left=(192-width)/2, top=(192-height)/2;
 result.transform={width/static_cast<float>(frame.uprightWidth()),height/static_cast<float>(frame.uprightHeight()),static_cast<float>(left),static_cast<float>(top)};
 const AffineTransform tensorToUpright{{1/result.transform.scaleX,0,(0.5F-result.transform.padLeft)/result.transform.scaleX-0.5F,0,1/result.transform.scaleY,(0.5F-result.transform.padTop)/result.transform.scaleY-0.5F}};
 AffineTransform uprightToStorage{}; (void)invert(createStorageToUprightTransform(frame.width(),frame.height(),frame.orientation()),uprightToStorage);
 return compose(tensorToUpright,toPixelCenterTransform(uprightToStorage));
}

AffineTransform handTransform(const buffers::FrameBuffer& frame,const RotatedRegion& region,HandPreprocessResult& result) {
 constexpr float size=224.0F; const float c=std::cos(region.rotationRadiansClockwise),s=std::sin(region.rotationRadiansClockwise);
 result.tensorToSource={{{c*region.width/size,-s*region.height/size,region.center.x-c*region.width*0.5F+s*region.height*0.5F,s*region.width/size,c*region.height/size,region.center.y-s*region.width*0.5F-c*region.height*0.5F}}};
 (void)invert(result.tensorToSource,result.sourceToTensor); AffineTransform uprightToStorage{};
 (void)invert(createStorageToUprightTransform(frame.width(),frame.height(),frame.orientation()),uprightToStorage);
 return compose(result.tensorToSource,toPixelCenterTransform(uprightToStorage));
}
}

class D3d12HandGeometryProcessor::Impl final
{
public:
 explicit Impl(std::shared_ptr<runtime::DirectMlRuntime> runtime):runtime_{std::move(runtime)}{}
 bool initialize(std::string& error) {
  ComPtr<ID3DBlob> bytecode,diagnostics; HRESULT hr=D3DCompile(kShader,sizeof(kShader)-1,"d3d12_preprocess",nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&bytecode,&diagnostics);
  if(FAILED(hr)){error=diagnostics?std::string{static_cast<const char*>(diagnostics->GetBufferPointer()),diagnostics->GetBufferSize()}:failed("D3DCompile",hr);return false;}
  D3D12_DESCRIPTOR_RANGE ranges[2]{}; ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0}; ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};
  D3D12_ROOT_PARAMETER roots[3]{}; roots[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; roots[0].DescriptorTable={1,&ranges[0]}; roots[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; roots[1].DescriptorTable={1,&ranges[1]}; roots[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; roots[2].Constants={0,0,sizeof(Parameters)/4};
  D3D12_ROOT_SIGNATURE_DESC rootDesc{3,roots,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE}; ComPtr<ID3DBlob> serialized;
  hr=D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&diagnostics);
  if(SUCCEEDED(hr))hr=runtime_->d3d12Device()->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root_));
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature=root_.Get(); pipeline.CS={bytecode->GetBufferPointer(),bytecode->GetBufferSize()};
  if(SUCCEEDED(hr))hr=runtime_->d3d12Device()->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&pipeline_));
  D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
  if(SUCCEEDED(hr))hr=runtime_->d3d12Device()->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&heap_));
  commandListType_=runtime_->commandQueue()->GetDesc().Type;
  if(SUCCEEDED(hr))hr=runtime_->d3d12Device()->CreateCommandAllocator(commandListType_,IID_PPV_ARGS(&allocator_));
  if(SUCCEEDED(hr))hr=runtime_->d3d12Device()->CreateCommandList(0,commandListType_,allocator_.Get(),pipeline_.Get(),IID_PPV_ARGS(&list_));
  if(SUCCEEDED(hr))hr=list_->Close(); if(FAILED(hr)){error=failed("Initialize D3D12 preprocess",hr);return false;} return true;
 }
 bool process(const buffers::FrameBuffer& frame,const AffineTransform& transform,std::uint32_t size,buffers::FloatTensorBuffer& tensor,std::string& error) {
  std::lock_guard gpuAccessLock{frame.gpuAccessMutex()};
  const auto device=runtime_->d3d11Device();
  if(!device){error="D3D12 preprocessing requires a D3D11 device.";return false;}
  const bool unwrapped=device->isD3d11On12();
  ComPtr<ID3D12Resource> camera;
  if(unwrapped)camera=device->unwrapTexture(
   frame.gpuTexture(),error,runtime_->commandQueue());
  else { double copyMs=0; camera=runtime_->copyCameraToSharedTexture(frame.gpuTexture(),frame.frameId(),copyMs,error); }
  if(!camera)return false;
  const auto returnWithoutFence=[&]() { if(unwrapped){std::string ignored; device->returnTexture(frame.gpuTexture(),nullptr,0,ignored);} };
  const UINT64 bytes=static_cast<UINT64>(size)*size*3*sizeof(float); if(!ensureOutput(bytes,error)){returnWithoutFence();return false;}
  HRESULT hr=allocator_->Reset(); if(SUCCEEDED(hr))hr=list_->Reset(allocator_.Get(),pipeline_.Get()); if(FAILED(hr)){error=failed("Reset D3D12 preprocess",hr);returnWithoutFence();return false;}
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM; srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels=1;
  auto cpu=heap_->GetCPUDescriptorHandleForHeapStart(); const UINT step=runtime_->d3d12Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); runtime_->d3d12Device()->CreateShaderResourceView(camera.Get(),&srv,cpu); cpu.ptr+=step;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format=DXGI_FORMAT_R32_TYPELESS; uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER; uav.Buffer.NumElements=static_cast<UINT>(bytes/4); uav.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW; runtime_->d3d12Device()->CreateUnorderedAccessView(output_.Get(),nullptr,&uav,cpu);
  D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition={camera.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}; list_->ResourceBarrier(1,&barrier);
  ID3D12DescriptorHeap* heaps[]{heap_.Get()}; list_->SetDescriptorHeaps(1,heaps); list_->SetComputeRootSignature(root_.Get()); auto gpu=heap_->GetGPUDescriptorHandleForHeapStart(); list_->SetComputeRootDescriptorTable(0,gpu); gpu.ptr+=step; list_->SetComputeRootDescriptorTable(1,gpu);
  const auto& v=transform.values; Parameters parameters{{v[0],v[1],v[2],v[3],v[4],v[5]},{frame.width(),frame.height()},{size,size}}; list_->SetComputeRoot32BitConstants(2,sizeof(parameters)/4,&parameters,0); list_->Dispatch((size+7)/8,(size+7)/8,1);
  std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); list_->ResourceBarrier(1,&barrier); hr=list_->Close(); if(FAILED(hr)){error=failed("Close D3D12 preprocess",hr);returnWithoutFence();return false;}
  ID3D12CommandList* lists[]{list_.Get()}; runtime_->commandQueue()->ExecuteCommandLists(1,lists);
  const auto fenceValue=runtime_->nextFenceValue();
  if(!runtime_->signal(fenceValue,error)){returnWithoutFence();return false;}
  if(unwrapped&&!device->returnTexture(frame.gpuTexture(),runtime_->d3d12Fence(),fenceValue,error))return false;
  tensor.setGpuResource(output_,fenceValue); return true;
 }
private:
 bool ensureOutput(UINT64 bytes,std::string& error){if(output_&&outputBytes_==bytes)return true; D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;HRESULT hr=runtime_->d3d12Device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output_));if(FAILED(hr)){error=failed("Create D3D12 tensor",hr);return false;}outputBytes_=bytes;return true;}
 std::shared_ptr<runtime::DirectMlRuntime> runtime_; D3D12_COMMAND_LIST_TYPE commandListType_{D3D12_COMMAND_LIST_TYPE_COMPUTE}; ComPtr<ID3D12RootSignature> root_; ComPtr<ID3D12PipelineState> pipeline_; ComPtr<ID3D12DescriptorHeap> heap_; ComPtr<ID3D12CommandAllocator> allocator_; ComPtr<ID3D12GraphicsCommandList> list_; ComPtr<ID3D12Resource> output_; UINT64 outputBytes_{0};
};

std::unique_ptr<D3d12HandGeometryProcessor> D3d12HandGeometryProcessor::create(std::shared_ptr<runtime::DirectMlRuntime> runtime,std::string& error){if(!runtime){error="DirectML runtime is required.";return{};}auto impl=std::make_unique<Impl>(std::move(runtime));if(!impl->initialize(error))return{};return std::unique_ptr<D3d12HandGeometryProcessor>{new D3d12HandGeometryProcessor{std::move(impl)}};}
D3d12HandGeometryProcessor::D3d12HandGeometryProcessor(std::unique_ptr<Impl> impl):impl_{std::move(impl)}{} D3d12HandGeometryProcessor::~D3d12HandGeometryProcessor()=default;
buffers::MemoryLocation D3d12HandGeometryProcessor::inputMemoryLocation()const noexcept{return buffers::MemoryLocation::Gpu;} buffers::MemoryLocation D3d12HandGeometryProcessor::outputMemoryLocation()const noexcept{return buffers::MemoryLocation::Gpu;}
bool D3d12HandGeometryProcessor::preprocessPalm(const buffers::FrameBuffer& frame,buffers::FloatTensorBuffer& tensor,PalmPreprocessResult& result){std::string error;return tensor.shape()==std::array<std::int64_t,4>{1,192,192,3}&&impl_->process(frame,palmTransform(frame,result),192,tensor,error);}
bool D3d12HandGeometryProcessor::preprocessHand(const buffers::FrameBuffer& frame,const RotatedRegion& region,buffers::FloatTensorBuffer& tensor,HandPreprocessResult& result){std::string error;return tensor.shape()==std::array<std::int64_t,4>{1,224,224,3}&&region.width>0&&region.height>0&&impl_->process(frame,handTransform(frame,region,result),224,tensor,error);}
}
