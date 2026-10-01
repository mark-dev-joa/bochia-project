#include "camera.hpp"
#include <chrono>
#include <stdexcept>
#include <sstream>
#include <windows.h>

static void check(int code,const char* operation){
    if(code!=IMV_OK) throw std::runtime_error(std::string(operation)+" failed (SDK "+std::to_string(code)+"). Close MV Viewer camera connection if busy.");
}
std::vector<Device> Camera::enumerate(){
    IMV_DeviceList list{};check(IMV_EnumDevices(&list,interfaceTypeAll),"Enumerate");
    std::vector<Device> result;
    for(unsigned i=0;i<list.nDevNum;++i){auto& d=list.pDevInfo[i];result.push_back({d.cameraKey,d.serialNumber,d.modelName});}
    return result;
}
void Camera::open(){
    if(replay)return;
    if(handle)return;
    check(IMV_CreateHandle(&handle,modeByCameraKey,device.key.data()),"CreateHandle");
    int status=IMV_Open(handle);
    if(status!=IMV_OK){IMV_DestroyHandle(handle);handle=nullptr;check(status,"Open");}
    settings.clear();
    for(const char* name:{"TriggerMode","TriggerSelector","TriggerSource","TriggerActivation","PixelFormat","AcquisitionMode","ExposureAuto","GainAuto","BalanceWhiteAuto","LineSelector","LineMode","LineSource"}){
        IMV_String value{};
        if(IMV_GetEnumFeatureSymbol(handle,name,&value)==IMV_OK)settings[name]=value.str;
    }
    for(const char* name:{"Width","Height","OffsetX","OffsetY","BinningHorizontal","BinningVertical","DecimationHorizontal","DecimationVertical"}){
        int64_t value=0;if(IMV_GetIntFeatureValue(handle,name,&value)==IMV_OK)settings[name]=std::to_string(value);
    }
    for(const char* name:{"ReverseX","ReverseY"}){
        bool value=false;if(IMV_GetBoolFeatureValue(handle,name,&value)==IMV_OK)settings[name]=value?"true":"false";
    }
    for(const char* name:{"ExposureTime","AcquisitionFrameRate","Gain","Gamma","BlackLevel","TriggerDelay"}){
        double value=0;if(IMV_GetDoubleFeatureValue(handle,name,&value)==IMV_OK)settings[name]=std::to_string(value);
    }
}
void Camera::start(){
    if(replay){active=true;return;}
    if(active)return;
    if(!handle)throw std::runtime_error("Connect camera first.");
    if(worker.joinable())worker.join();
    check(IMV_StartGrabbing(handle),"StartGrabbing");
    {std::lock_guard lock(mutex);stats={};frame={};}
    active=true;worker=std::thread(&Camera::loop,this);
}
void Camera::stop(){
    if(!replay){std::lock_guard lock(recorderMutex);if(recorder)recorder->stop("Camera Grab stopped during recording.");}
    active=false;
    if(worker.joinable())worker.join();
    if(handle && IMV_IsGrabbing(handle))IMV_StopGrabbing(handle);
}
void Camera::close(){stop();if(handle){IMV_Close(handle);IMV_DestroyHandle(handle);handle=nullptr;}}
CameraFrame Camera::latest(){std::lock_guard lock(mutex);return frame;}
CameraStats Camera::statistics(){std::lock_guard lock(mutex);return stats;}
void Camera::setRecorder(std::shared_ptr<RawRecorder> value,size_t index){std::lock_guard lock(recorderMutex);recorder=std::move(value);recorderIndex=index;}
void Camera::saveDeviceConfig(const std::filesystem::path& path){
    if(!handle)throw std::runtime_error("Camera is not connected.");
    // Export to an ASCII temporary path because the SDK accepts ANSI filenames.
    wchar_t tempDir[MAX_PATH]{},tempFile[MAX_PATH]{};
    if(!GetTempPathW(MAX_PATH,tempDir)||!GetTempFileNameW(tempDir,L"imv",0,tempFile))throw std::runtime_error("Cannot create camera configuration export.");
    std::filesystem::path temporary(tempFile);
    try{check(IMV_SaveDeviceCfg(handle,temporary.string().c_str()),"SaveDeviceCfg");std::filesystem::copy_file(temporary,path,std::filesystem::copy_options::overwrite_existing);std::filesystem::remove(temporary);}
    catch(...){std::error_code ec;std::filesystem::remove(temporary,ec);throw;}
}
void Camera::setReplayFrame(CameraFrame value,double fps,uint64_t gaps,uint64_t invalid){if(!replay)throw std::logic_error("Replay frame requires a replay camera.");std::lock_guard lock(mutex);frame=std::move(value);stats.received=frame.generation;stats.fps=fps;stats.gaps=gaps;stats.invalid=invalid;active=true;}
void Camera::loop(){
    using clock=std::chrono::steady_clock;
    auto rateStart=clock::now(),previewTime=rateStart-std::chrono::seconds(1);
    uint64_t rateFrames=0,previous=0,generation=0;
    while(active){
        IMV_Frame f{};int rc=IMV_GetFrame(handle,&f,150);
        if(rc==IMV_TIMEOUT)continue;
        if(rc!=IMV_OK){std::lock_guard lock(mutex);stats.error="GetFrame SDK error "+std::to_string(rc);active=false;{std::lock_guard recLock(recorderMutex);if(recorder)recorder->stop(stats.error);}break;}
        // Every successful GetFrame is paired with ReleaseFrame even after conversion failures.
        try{
            auto now=clock::now();
            RawFrameInfo raw{f.frameInfo.width,f.frameInfo.height,f.frameInfo.paddingX,f.frameInfo.paddingY,unsigned(f.frameInfo.pixelFormat),f.frameInfo.status,
                f.frameInfo.blockId,f.frameInfo.timeStamp,uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()),f.frameInfo.size};
            {std::lock_guard lock(recorderMutex);if(recorder)recorder->submit(recorderIndex,raw,{f.pData,size_t(raw.size)});}
            {std::lock_guard lock(mutex);
                ++stats.received;++rateFrames;
                stats.frameBytes=f.frameInfo.size;
                if(f.frameInfo.status)++stats.invalid;
                if(previous && f.frameInfo.blockId>previous+1)stats.gaps+=f.frameInfo.blockId-previous-1;
                previous=f.frameInfo.blockId;
                double elapsed=std::chrono::duration<double>(now-rateStart).count();
                if(elapsed>=0.5){stats.fps=rateFrames/elapsed;rateFrames=0;rateStart=now;}
            }
            if(!f.frameInfo.status && now-previewTime>=std::chrono::milliseconds(70)){
                cv::Mat image=decodeRawFrame(raw,{f.pData,size_t(raw.size)});
                {std::lock_guard lock(mutex);frame={image,f.frameInfo.blockId,f.frameInfo.timeStamp,++generation,now};}
                previewTime=now;
            }
        }catch(const std::exception& e){std::lock_guard lock(mutex);stats.error=e.what();active=false;std::lock_guard recLock(recorderMutex);if(recorder)recorder->stop(stats.error);}
        IMV_ReleaseFrame(handle,&f);
    }
}
