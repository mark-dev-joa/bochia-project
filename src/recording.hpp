#pragma once
#include "settings.hpp"
#include <chrono>
#include <memory>
#include <span>

struct RawFrameInfo {
    uint32_t width=0,height=0,paddingX=0,paddingY=0,pixelFormat=0,status=0;
    uint64_t id=0,timestamp=0,hostNs=0,size=0,offset=0;
    uint64_t storedSize=0,sourceGaps=0,sourceInvalid=0,sampledOut=0;
    int codec=0,filterStride=0; // 0 raw, 1 independent Zstandard frame; reversible byte delta.
};
struct RecordedCamera {
    std::string serial,model,name,configError;
    std::map<std::string,std::string> settings;
    uint64_t gaps=0,invalid=0;
};
struct RecordingStats {
    bool accepting=false,finished=false;
    uint64_t bytes=0,rawBytes=0,queuedBytes=0;
    double seconds=0;
    std::vector<uint64_t> frames,invalid,gaps,sampledOut;
    std::string error;
};
class RawRecorder {
public:
    RawRecorder(const std::filesystem::path& folder,const std::vector<RecordedCamera>& cameras,const Settings& snapshot,
                uint64_t queueLimit=256ull*1024*1024,RecordingOptions options={false,0});
    ~RawRecorder();
    RawRecorder(const RawRecorder&)=delete;
    void submit(size_t camera,RawFrameInfo info,std::span<const uint8_t> bytes) noexcept;
    void stop(std::string error={});
    RecordingStats statistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
// SDK-independent conversion using the installed camera runtime's ImageConvert DLL.
// Source bytes remain untouched; both live previews and replay use this function.
cv::Mat decodeRawFrame(const RawFrameInfo& info,std::span<const uint8_t> bytes);
struct ReplayFrame {cv::Mat image;RawFrameInfo info;};
class RawReplay {
public:
    explicit RawReplay(const std::filesystem::path& folder,std::stop_token stop={});
    std::vector<RecordedCamera> cameras;
    Settings settings;
    std::string warning;
    std::vector<std::vector<RawFrameInfo>> index;
    ReplayFrame read(size_t camera,size_t frame) const;
    std::vector<uint8_t> readRaw(size_t camera,size_t frame) const;
    size_t nearest(size_t camera,uint64_t hostNs) const;
    const std::filesystem::path& directory()const{return folder;}
private:
    std::filesystem::path folder;
};
