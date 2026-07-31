#include "Rendering/gesture_dtw_debug_render_stage.h"
#include <chrono>
#include <utility>

namespace ryoiki::rendering
{
GestureDtwDebugRenderStage::~GestureDtwDebugRenderStage(){stop();}
bool GestureDtwDebugRenderStage::start(std::shared_ptr<runtime::D3d11Device> device,const HWND hwnd,
    const std::uint32_t width,const std::uint32_t height,std::string& error)
{
    stop(); {std::lock_guard lock{mutex_};stopping_=false;initialized_=false;redraw_=true;latest_.reset();last_.reset();pendingSize_.reset();initializationError_.clear();}
    worker_=std::thread{&GestureDtwDebugRenderStage::run,this,std::move(device),hwnd,Size{width,height}};
    std::unique_lock lock{mutex_};
    if(!condition_.wait_for(lock,std::chrono::seconds{5},[this]{return initialized_||!initializationError_.empty();}))
    {error="DTW renderer initialization timed out.";lock.unlock();stop();return false;}
    if(!initializationError_.empty()){error=initializationError_;lock.unlock();stop();return false;} return true;
}
void GestureDtwDebugRenderStage::stop(){{std::lock_guard lock{mutex_};stopping_=true;}condition_.notify_all();if(worker_.joinable())worker_.join();std::lock_guard lock{mutex_};initialized_=false;latest_.reset();last_.reset();pendingSize_.reset();}
void GestureDtwDebugRenderStage::publish(GestureDtwDebugRenderPacket packet){if(!validateGestureDtwDebugRenderPacket(packet))return;{std::lock_guard lock{mutex_};if(!initialized_||stopping_)return;latest_=std::move(packet);}condition_.notify_one();}
void GestureDtwDebugRenderStage::resize(const std::uint32_t w,const std::uint32_t h){{std::lock_guard lock{mutex_};if(!initialized_||stopping_)return;pendingSize_=Size{w,h};redraw_=true;}condition_.notify_one();}
void GestureDtwDebugRenderStage::redraw(){{std::lock_guard lock{mutex_};if(!initialized_||stopping_)return;redraw_=true;}condition_.notify_one();}
void GestureDtwDebugRenderStage::run(std::shared_ptr<runtime::D3d11Device> device,const HWND hwnd,const Size size)
{
    GestureDtwDebugRenderer renderer;std::string error;
    try{if(!renderer.initialize(std::move(device),hwnd,size.width,size.height,error)){std::lock_guard lock{mutex_};initializationError_=error.empty()?"DTW renderer initialization failed.":error;condition_.notify_all();return;}}
    catch(const std::exception& e){std::lock_guard lock{mutex_};initializationError_=e.what();condition_.notify_all();return;}
    catch(...){std::lock_guard lock{mutex_};initializationError_="Unknown DTW renderer initialization failure.";condition_.notify_all();return;}
    {std::lock_guard lock{mutex_};initialized_=true;}condition_.notify_all();bool suspended=size.width==0||size.height==0;
    while(true){std::optional<GestureDtwDebugRenderPacket> packet;std::optional<Size> resize;bool redraw=false;
        {std::unique_lock lock{mutex_};condition_.wait(lock,[this]{return stopping_||latest_.has_value()||pendingSize_.has_value()||redraw_;});if(stopping_)break;packet=std::move(latest_);latest_.reset();resize=pendingSize_;pendingSize_.reset();redraw=redraw_;redraw_=false;if(packet){last_=packet;}else if(redraw){packet=last_;}}
        if(resize){suspended=resize->width==0||resize->height==0;if(!suspended&&!renderer.resize(resize->width,resize->height,error))continue;}
        if(!suspended&&packet)
        {
            [[maybe_unused]] const bool rendered = renderer.render(*packet,error);
        }
    }
}
}
