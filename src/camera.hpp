#pragma once
#include <opencv2/core.hpp>
#include <IMVApi.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "recording.hpp"

struct Device { std::string key, serial, model; };
struct CameraFrame { cv::Mat image; uint64_t id=0, timestamp=0, generation=0; std::chrono::steady_clock::time_point receivedAt{}; };
struct CameraStats { uint64_t received=0, invalid=0, gaps=0; double fps=0; std::string error; uint64_t frameBytes=0; };
class Camera {
public:
    explicit Camera(Device d,bool playback=false):device(std::move(d)),replay(playback){}
    ~Camera(){close();}
    Camera(const Camera&)=delete;
    static std::vector<Device> enumerate();
    void open();
    void start();
    void stop();
    void close();
    bool connected()const{return replay||handle!=nullptr;}
    bool running()const{return active.load();}
    CameraFrame latest();
    CameraStats statistics();
    void setRecorder(std::shared_ptr<RawRecorder> recorder,size_t index=0);
    void saveDeviceConfig(const std::filesystem::path& path);
    void setReplayFrame(CameraFrame value,double fps=0,uint64_t gaps=0,uint64_t invalid=0);
    bool isReplay()const{return replay;}
    Device device;
    std::map<std::string,std::string> settings;
private:
    void loop();
    IMV_HANDLE handle=nullptr;
    std::atomic<bool> active=false;
    std::thread worker;
    std::mutex mutex;
    CameraFrame frame;
    CameraStats stats;
    bool replay=false;
    std::mutex recorderMutex;
    std::shared_ptr<RawRecorder> recorder;
    size_t recorderIndex=0;
};
