#include "ryoiki_native.h"
#include "HandInput/Recognition/gesture_recording_playback_access.h"
#include "Rendering/gesture_dtw_debug_render_stage.h"
#include "Runtime/d3d11_device.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct RyoikiGestureRecordingPlaybackHandle
{
    HWND child{}; std::shared_ptr<ryoiki::runtime::D3d11Device> device;
    ryoiki::rendering::GestureDtwDebugRenderStage stage;
    RyoikiHandle* source{}; std::mutex mutex;
    ryoiki::hand_input::recognition::GestureRecordingProvenance recording;
    std::uint32_t definitionId{}; std::uint32_t takeIndex{}; std::size_t frameIndex{};
    bool playing{}; std::string error; std::atomic_bool stopping{false}; std::thread clock;
};
namespace
{
constexpr wchar_t kClassName[]=L"RyoikiTenkaiGestureRecordingPlayback";
LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){if(msg==WM_NCCREATE){const auto*c=reinterpret_cast<CREATESTRUCTW*>(l);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(c->lpCreateParams));}auto*h=reinterpret_cast<RyoikiGestureRecordingPlaybackHandle*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));if(msg==WM_ERASEBKGND)return 1;if(msg==WM_PAINT){PAINTSTRUCT p{};BeginPaint(hwnd,&p);if(h)h->stage.redraw();EndPaint(hwnd,&p);return 0;}if(msg==WM_SIZE&&h){h->stage.resize(LOWORD(l),HIWORD(l));return 0;}if(msg==WM_NCDESTROY)SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);return DefWindowProcW(hwnd,msg,w,l);}
bool reg(){static std::once_flag once;static bool ok{};std::call_once(once,[]{WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=kClassName;c.hCursor=LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(IDC_ARROW));ok=RegisterClassW(&c)!=0||GetLastError()==ERROR_CLASS_ALREADY_EXISTS;});return ok;}
ryoiki::rendering::GestureDtwDebugRenderPacket packet(const RyoikiGestureRecordingPlaybackHandle& h)
{
    ryoiki::rendering::GestureDtwDebugRenderPacket p{};if(h.recording.frames.empty())return p;
    const auto& f=h.recording.frames[(std::min)(h.frameIndex,h.recording.frames.size()-1)];
    p.frameId=h.frameIndex+1;p.timestampUs=h.recording.capturedAtUs+static_cast<std::uint64_t>(f.timeOffsetMs*1000.0);
    p.templateId=h.definitionId;p.candidateFrameCount=1;p.templateFrameCount=1;p.confidence=f.confidence[0];
    auto& a=p.candidateFrames[0];auto& b=p.templateFrames[0];a.timeOffsetMs=f.timeOffsetMs;b.timeOffsetMs=f.timeOffsetMs;
    for(std::size_t i=0;i<21;++i){const auto& v=f.normalizedSkeletons[0][i];a.landmarks[i]={v.x,v.y,v.z};b.landmarks[i]=a.landmarks[i];}
    return p;
}
}
RYOIKI_EXPORT RyoikiGestureRecordingPlaybackHandle* ryoiki_recording_playback_create(void* parent,RyoikiHandle* source)
{try{if(!parent||!source||!reg())return nullptr;auto h=std::make_unique<RyoikiGestureRecordingPlaybackHandle>();h->source=source;std::string e;h->device=ryoiki::runtime::D3d11Device::create(e);if(!h->device)return nullptr;RECT r{};GetClientRect(static_cast<HWND>(parent),&r);const auto width=static_cast<std::uint32_t>((std::max)(1L,r.right-r.left));const auto height=static_cast<std::uint32_t>((std::max)(1L,r.bottom-r.top));h->child=CreateWindowExW(0,kClassName,L"",WS_CHILD|WS_VISIBLE,0,0,width,height,static_cast<HWND>(parent),nullptr,GetModuleHandleW(nullptr),h.get());if(!h->child||!h->stage.start(h->device,h->child,width,height,e))return nullptr;auto* raw=h.get();raw->clock=std::thread{[raw]{while(!raw->stopping.load()){std::this_thread::sleep_for(std::chrono::milliseconds{33});std::lock_guard lock{raw->mutex};if(raw->playing&&!raw->recording.frames.empty()){if(++raw->frameIndex>=raw->recording.frames.size()){raw->frameIndex=raw->recording.frames.size()-1;raw->playing=false;}raw->stage.publish(packet(*raw));}}}};return h.release();}catch(...){return nullptr;}}
RYOIKI_EXPORT std::int32_t ryoiki_recording_playback_select(RyoikiGestureRecordingPlaybackHandle*h,std::uint32_t id,std::uint32_t take){if(!h)return 0;std::lock_guard lock{h->mutex};decltype(h->recording) value;std::string e;if(!ryoikiLoadGestureRecording(h->source,id,take,value,e)){h->error=e;return 0;}h->recording=std::move(value);h->definitionId=id;h->takeIndex=take;h->frameIndex=0;h->playing=false;h->error.clear();h->stage.publish(packet(*h));return 1;}
RYOIKI_EXPORT std::int32_t ryoiki_recording_playback_set_playing(RyoikiGestureRecordingPlaybackHandle*h,std::uint32_t value){if(!h)return 0;std::lock_guard lock{h->mutex};h->playing=value!=0&&!h->recording.frames.empty();return 1;}
RYOIKI_EXPORT std::int32_t ryoiki_recording_playback_seek(RyoikiGestureRecordingPlaybackHandle*h,float value){if(!h)return 0;std::lock_guard lock{h->mutex};if(h->recording.frames.empty())return 0;value=(std::clamp)(value,0.0F,1.0F);h->frameIndex=static_cast<std::size_t>(value*static_cast<float>(h->recording.frames.size()-1));h->stage.publish(packet(*h));return 1;}
RYOIKI_EXPORT std::int32_t ryoiki_recording_playback_get_status(RyoikiGestureRecordingPlaybackHandle*h,RyoikiGesturePlaybackStatus*out){if(!h||!out)return 0;std::lock_guard lock{h->mutex};*out={};out->abi_version=kRyoikiAbiVersion;out->struct_size=sizeof(*out);out->definition_id=h->definitionId;out->take_index=h->takeIndex;out->playing=h->playing?1U:0U;out->frame_index=static_cast<std::uint32_t>(h->frameIndex);out->frame_count=static_cast<std::uint32_t>(h->recording.frames.size());out->duration_ms=h->recording.durationMs;if(!h->recording.frames.empty()){out->position_ms=h->recording.frames[h->frameIndex].timeOffsetMs;out->normalized_progress=h->recording.frames.size()>1?static_cast<float>(h->frameIndex)/static_cast<float>(h->recording.frames.size()-1):0.0F;}return 1;}
RYOIKI_EXPORT std::int32_t ryoiki_recording_playback_resize(RyoikiGestureRecordingPlaybackHandle*h,std::int32_t w,std::int32_t t){return h&&h->child&&w>0&&t>0&&MoveWindow(h->child,0,0,w,t,TRUE)?1:0;}
RYOIKI_EXPORT std::int32_t ryoiki_recording_playback_get_last_error(RyoikiGestureRecordingPlaybackHandle*h,char*b,std::int32_t n){if(!h||!b||n<=0)return 0;std::lock_guard lock{h->mutex};const auto c=(std::min)(h->error.size(),static_cast<std::size_t>(n-1));std::memcpy(b,h->error.data(),c);b[c]=0;return h->error.empty()?0:1;}
RYOIKI_EXPORT void ryoiki_recording_playback_destroy(RyoikiGestureRecordingPlaybackHandle*h){if(!h)return;h->stopping.store(true);if(h->clock.joinable())h->clock.join();h->stage.stop();if(h->child)DestroyWindow(h->child);delete h;}
