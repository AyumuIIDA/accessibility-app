#include "ryoiki_native.h"
#include "Rendering/gesture_dtw_debug_render_stage.h"
#include "Rendering/gesture_dtw_debug_source.h"
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

struct RyoikiGestureDtwDebugHandle
{
    HWND child{};
    std::shared_ptr<ryoiki::runtime::D3d11Device> device;
    std::shared_ptr<ryoiki::rendering::GestureDtwDebugSource> source;
    ryoiki::rendering::GestureDtwDebugRenderStage stage;
    std::atomic_bool stopping{false};
    std::thread bridge;
    std::mutex mutex;
    std::string error;
};

namespace
{
constexpr wchar_t kClassName[] = L"RyoikiTenkaiGestureDtwDebugViewport";
LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if(message==WM_NCCREATE){const auto* create=reinterpret_cast<CREATESTRUCTW*>(lparam);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(create->lpCreateParams));}
    auto* handle=reinterpret_cast<RyoikiGestureDtwDebugHandle*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    switch(message){case WM_ERASEBKGND:return 1;case WM_PAINT:{PAINTSTRUCT paint{};BeginPaint(hwnd,&paint);if(handle)handle->stage.redraw();EndPaint(hwnd,&paint);return 0;}case WM_SIZE:if(handle)handle->stage.resize(LOWORD(lparam),HIWORD(lparam));return 0;case WM_NCDESTROY:SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);break;}
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
bool registerWindowClass(){static std::once_flag once;static bool registered=false;std::call_once(once,[]{WNDCLASSW value{};value.lpfnWndProc=windowProc;value.hInstance=GetModuleHandleW(nullptr);value.lpszClassName=kClassName;value.hCursor=LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(IDC_ARROW));registered=RegisterClassW(&value)!=0||GetLastError()==ERROR_CLASS_ALREADY_EXISTS;});return registered;}
}

RYOIKI_EXPORT RyoikiGestureDtwDebugHandle* ryoiki_dtw_debug_create(void* parentHwnd,RyoikiHandle* sourceHandle)
{
    try
    {
        if(parentHwnd==nullptr||sourceHandle==nullptr||!registerWindowClass())return nullptr;
        auto handle=std::make_unique<RyoikiGestureDtwDebugHandle>();
        handle->source=ryoikiGestureDtwDebugSource(sourceHandle);
        if(handle->source==nullptr)return nullptr;
        std::string error;handle->device=ryoiki::runtime::D3d11Device::create(error);if(handle->device==nullptr)return nullptr;
        RECT bounds{};GetClientRect(static_cast<HWND>(parentHwnd),&bounds);
        const auto width=static_cast<std::uint32_t>((std::max)(1L,bounds.right-bounds.left));
        const auto height=static_cast<std::uint32_t>((std::max)(1L,bounds.bottom-bounds.top));
        handle->child=CreateWindowExW(0,kClassName,L"",WS_CHILD|WS_VISIBLE,0,0,width,height,static_cast<HWND>(parentHwnd),nullptr,GetModuleHandleW(nullptr),handle.get());
        if(handle->child==nullptr)return nullptr;
        if(!handle->stage.start(handle->device,handle->child,width,height,error)){DestroyWindow(handle->child);return nullptr;}
        auto* raw=handle.get();
        raw->bridge=std::thread{[raw]
        {
            std::uint64_t lastRevision=0;
            while(!raw->stopping.load(std::memory_order_acquire))
            {
                ryoiki::rendering::GestureDtwDebugRenderPacket packet{};
                std::uint64_t revision=0;
                if(raw->source->tryGet(packet,revision)&&revision!=lastRevision){lastRevision=revision;raw->stage.publish(std::move(packet));}
                std::this_thread::sleep_for(std::chrono::milliseconds{40});
            }
        }};
        return handle.release();
    }
    catch(...){return nullptr;}
}
RYOIKI_EXPORT std::int32_t ryoiki_dtw_debug_resize(RyoikiGestureDtwDebugHandle* handle,const std::int32_t width,const std::int32_t height){return handle&&handle->child&&width>0&&height>0&&MoveWindow(handle->child,0,0,width,height,TRUE)?1:0;}
RYOIKI_EXPORT std::int32_t ryoiki_dtw_debug_get_last_error(RyoikiGestureDtwDebugHandle* handle,char* buffer,const std::int32_t length){if(!handle||!buffer||length<=0)return 0;std::lock_guard lock{handle->mutex};const auto count=(std::min)(handle->error.size(),static_cast<std::size_t>(length-1));std::memcpy(buffer,handle->error.data(),count);buffer[count]='\0';return handle->error.empty()?0:1;}
RYOIKI_EXPORT void ryoiki_dtw_debug_destroy(RyoikiGestureDtwDebugHandle* handle){if(!handle)return;handle->stopping.store(true,std::memory_order_release);if(handle->bridge.joinable())handle->bridge.join();handle->stage.stop();if(handle->child)DestroyWindow(handle->child);delete handle;}
