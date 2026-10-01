#include "recording.hpp"
#define NOMINMAX
#include <windows.h>
#include <fstream>
#include <deque>
#include <condition_variable>
#include <thread>
#include <set>
#include <algorithm>
#include <zstd.h>

namespace fs=std::filesystem;
using nlohmann::json;
namespace {
constexpr uint64_t maxFrameBytes=256ull*1024*1024;
json encode(const RawFrameInfo& f){return {{"width",f.width},{"height",f.height},{"padding_x",f.paddingX},{"padding_y",f.paddingY},
    {"pixel_format",f.pixelFormat},{"status",f.status},{"id",f.id},{"device_timestamp",f.timestamp},{"host_ns",f.hostNs},{"size",f.size},{"offset",f.offset},
    {"stored_size",f.storedSize},{"codec",f.codec},{"filter_stride",f.filterStride},{"source_gaps",f.sourceGaps},{"source_invalid",f.sourceInvalid},{"sampled_out",f.sampledOut}};}
RawFrameInfo decode(const json& j){RawFrameInfo f;
    auto u=[&](const char* key)->uint64_t{const auto& v=j.at(key);if(!v.is_number_integer()||(v.is_number_integer()&&!v.is_number_unsigned()&&v.get<int64_t>()<0))throw std::runtime_error("Invalid frame index number.");return v.get<uint64_t>();};
    auto n=[&](const char* key)->uint32_t{auto v=u(key);if(v>UINT32_MAX)throw std::runtime_error("Frame index overflow.");return uint32_t(v);};
    f.width=n("width");f.height=n("height");f.paddingX=n("padding_x");f.paddingY=n("padding_y");f.pixelFormat=n("pixel_format");f.status=n("status");
    f.id=u("id");f.timestamp=u("device_timestamp");f.hostNs=u("host_ns");f.size=u("size");f.offset=u("offset");
    f.storedSize=j.contains("stored_size")?u("stored_size"):f.size;
    if(j.contains("codec"))f.codec=int(n("codec"));if(j.contains("filter_stride"))f.filterStride=int(n("filter_stride"));
    if(j.contains("source_gaps"))f.sourceGaps=u("source_gaps");if(j.contains("source_invalid"))f.sourceInvalid=u("source_invalid");if(j.contains("sampled_out"))f.sampledOut=u("sampled_out");
    if(f.codec<0||f.codec>1||f.filterStride<0||f.filterStride>8||(f.codec==0&&(f.filterStride||f.storedSize!=f.size)))throw std::runtime_error("Unsupported raw frame encoding.");return f;
}
void checkZstd(size_t status){if(ZSTD_isError(status))throw std::runtime_error(std::string("Raw compression: ")+ZSTD_getErrorName(status));}
int predictorStride(const RawFrameInfo& f){unsigned bits=(f.pixelFormat>>16)&255;
    if(bits==8){auto type=f.pixelFormat&65535;return type>=8&&type<=11?2:1;}
    return bits%8==0&&bits<=64?int(bits/8):0;
}
fs::path cameraFolder(const fs::path& folder,size_t i){return folder/("camera-"+std::to_string(i));}
void writeManifest(const fs::path& folder,const json& j){auto tmp=folder/"session.json.tmp",path=folder/"session.json";
    {std::ofstream out(tmp,std::ios::binary);out<<j.dump(2);out.flush();if(!out)throw std::runtime_error("Cannot write recording manifest.");}
    if(!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot commit recording manifest.");
}
void validateFrame(const RawFrameInfo& f,uint64_t bytes){
    if(f.status||!f.width||!f.height||f.width>32768||f.height>32768||uint64_t(f.width)*f.height>64ull*1024*1024||f.size!=bytes||bytes>maxFrameBytes)
        throw std::runtime_error("Invalid raw frame dimensions or byte count.");
    unsigned bits=(f.pixelFormat>>16)&255;
    if(bits<1||bits>64||f.paddingX>65536||f.paddingY>65536)throw std::runtime_error("Invalid raw pixel format or padding.");
    uint64_t required=((uint64_t(f.width)*bits+7)/8+f.paddingX)*f.height+f.paddingY;
    if(bytes<required)throw std::runtime_error("Truncated raw frame payload.");
}
}
cv::Mat decodeRawFrame(const RawFrameInfo& f,std::span<const uint8_t> bytes){
    validateFrame(f,bytes.size());
    struct Params {int width,height,paddingX,paddingY,dataSize;unsigned pixelFormat;};
    using Convert=int(__stdcall*)(unsigned char*,Params*,unsigned char*,int*);
    static Convert convert=[](){wchar_t module[32768]{};auto length=GetModuleFileNameW(nullptr,module,32768);
        if(!length||length>=32768)throw std::runtime_error("Cannot resolve image converter path.");
        auto path=fs::path(module).parent_path()/"ImageConvert.dll";
        auto library=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!library)throw std::runtime_error("ImageConvert.dll is missing beside the application.");
        auto fn=reinterpret_cast<Convert>(GetProcAddress(library,"IMGCNV_ConvertToBGR24"));
        if(!fn){FreeLibrary(library);throw std::runtime_error("Camera image converter is unavailable.");}return fn;
    }();
    cv::Mat bgr(int(f.height),int(f.width),CV_8UC3);int capacity=int(bgr.total()*3);
    Params p{int(f.width),int(f.height),int(f.paddingX),int(f.paddingY),int(bytes.size()),f.pixelFormat};
    // The vendor C API is not const-qualified; it only reads its source buffer.
    static std::mutex conversionMutex;std::lock_guard lock(conversionMutex);
    int rc=convert(const_cast<unsigned char*>(bytes.data()),&p,bgr.data,&capacity);
    if(rc||capacity!=int(bgr.total()*3))throw std::runtime_error("Raw pixel conversion failed: "+std::to_string(rc));return bgr;
}
struct RawRecorder::Impl {
    struct Packet {size_t camera;RawFrameInfo info;std::vector<uint8_t> bytes;};
    struct Sink {std::ofstream raw,index;std::deque<Packet> queue;uint64_t offset=0,previous=0,bucket=UINT64_MAX;};
    fs::path folder;json manifest;std::vector<Sink> sinks;
    mutable std::mutex mutex;std::condition_variable changed;
    RecordingStats stats;uint64_t limit,origin=0;bool hasOrigin=false;
    RecordingOptions options;std::vector<std::thread> workers;size_t finishedWorkers=0;
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    Impl(const fs::path& dir,const std::vector<RecordedCamera>& cameras,const Settings& snapshot,uint64_t queueLimit,RecordingOptions configured):folder(dir),limit(queueLimit),options(configured){
        options.validate();
        if(cameras.empty()||cameras.size()>3||limit<1)throw std::runtime_error("Recording requires 1–3 cameras.");
        std::set<std::string> ids;for(const auto& c:cameras)if(c.serial.empty()||!ids.insert(c.serial).second)throw std::runtime_error("Invalid recording camera identities.");
        if(!fs::create_directory(folder))throw std::runtime_error("Recording folder already exists.");
        manifest={{"schema_version",1},{"format","irayple-raw-v2"},{"complete",false},{"error",""},
            {"compression",options.lossless?"zstd-lossless":"none"},{"requested_fps",options.fps},
            {"started_unix_ms",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
            {"time_basis","host_ns: steady clock receive time; device clocks are not assumed synchronized"},{"cameras",json::array()}};
        for(size_t i=0;i<cameras.size();++i){const auto& c=cameras[i];fs::create_directory(cameraFolder(folder,i));Sink sink;
            sink.raw.open(cameraFolder(folder,i)/"frames.raw",std::ios::binary);sink.index.open(cameraFolder(folder,i)/"frames.jsonl",std::ios::binary);
            if(!sink.raw||!sink.index)throw std::runtime_error("Cannot create raw recording files.");sinks.push_back(std::move(sink));
            manifest["cameras"].push_back({{"serial",c.serial},{"model",c.model},{"name",c.name},{"settings",c.settings},{"config_error",c.configError}});}
        writeSettings(folder/"calibration.json",snapshot);writeManifest(folder,manifest);
        stats.frames.resize(cameras.size());stats.invalid.resize(cameras.size());stats.gaps.resize(cameras.size());stats.sampledOut.resize(cameras.size());stats.accepting=true;
        try{for(size_t i=0;i<sinks.size();++i)workers.emplace_back([this,i]{run(i);});}
        catch(...){stop("Cannot start recording workers.");for(auto& worker:workers)worker.join();throw;}
    }
    ~Impl(){stop({});for(auto& worker:workers)if(worker.joinable())worker.join();}
    void stop(std::string error){std::lock_guard lock(mutex);stats.accepting=false;if(stats.error.empty())stats.error=std::move(error);changed.notify_all();}
    void run(size_t camera){auto& sink=sinks[camera];try{
        std::unique_ptr<ZSTD_CCtx,decltype(&ZSTD_freeCCtx)> compressor(options.lossless?ZSTD_createCCtx():nullptr,ZSTD_freeCCtx);
        if(options.lossless&&!compressor)throw std::runtime_error("Cannot allocate lossless compressor.");
        if(compressor){checkZstd(ZSTD_CCtx_setParameter(compressor.get(),ZSTD_c_compressionLevel,1));checkZstd(ZSTD_CCtx_setParameter(compressor.get(),ZSTD_c_checksumFlag,1));}
        auto lastFlush=std::chrono::steady_clock::now();std::vector<uint8_t> filtered,compressed;
        for(;;){Packet packet;
            {std::unique_lock lock(mutex);changed.wait(lock,[&]{return !sink.queue.empty()||!stats.accepting;});
                if(sink.queue.empty())break;packet=std::move(sink.queue.front());sink.queue.pop_front();stats.queuedBytes-=packet.bytes.size();}
            auto& info=packet.info;info.offset=sink.offset;info.storedSize=info.size;info.codec=0;info.filterStride=0;
            std::span<const uint8_t> stored=packet.bytes;
            if(compressor&&!packet.bytes.empty()){
                int stride=predictorStride(info);std::span<const uint8_t> input=packet.bytes;
                if(stride&&packet.bytes.size()>size_t(stride)){filtered.resize(packet.bytes.size());std::copy_n(packet.bytes.begin(),stride,filtered.begin());
                    for(size_t n=stride;n<packet.bytes.size();++n)filtered[n]=uint8_t(packet.bytes[n]-packet.bytes[n-stride]);input=filtered;
                }else stride=0;
                compressed.resize(ZSTD_compressBound(input.size()));auto size=ZSTD_compress2(compressor.get(),compressed.data(),compressed.size(),input.data(),input.size());checkZstd(size);
                if(size<packet.bytes.size()){info.codec=1;info.filterStride=stride;info.storedSize=size;stored={compressed.data(),size};}
            }
            sink.raw.write(reinterpret_cast<const char*>(stored.data()),std::streamsize(stored.size()));
            sink.index<<encode(info).dump()<<'\n';if(!sink.raw||!sink.index)throw std::runtime_error("Disk write failed; recording is partial.");sink.offset+=stored.size();
            {std::lock_guard lock(mutex);++stats.frames[camera];stats.bytes+=stored.size();stats.rawBytes+=info.size;}
            if(std::chrono::steady_clock::now()-lastFlush>std::chrono::seconds(1)){
                sink.raw.flush();sink.index.flush();if(!sink.raw||!sink.index)throw std::runtime_error("Recording flush failed.");
                std::error_code ec;auto space=fs::space(folder,ec);if(!ec&&space.available<512ull*1024*1024)stop("Disk space is low; recording stopped.");lastFlush=std::chrono::steady_clock::now();}
        }
        sink.raw.flush();sink.index.flush();if(!sink.raw||!sink.index)throw std::runtime_error("Recording flush failed.");
    }catch(const std::exception& e){stop(e.what());}
        sink.raw.close();sink.index.close();bool last=false;
        {std::lock_guard lock(mutex);for(const auto& packet:sink.queue)stats.queuedBytes-=packet.bytes.size();sink.queue.clear();
            last=++finishedWorkers==sinks.size();if(last){stats.accepting=false;
                manifest["complete"]=stats.error.empty();manifest["error"]=stats.error;manifest["frames"]=stats.frames;manifest["invalid"]=stats.invalid;
                manifest["gaps"]=stats.gaps;manifest["sampled_out"]=stats.sampledOut;manifest["bytes"]=stats.bytes;manifest["raw_bytes"]=stats.rawBytes;}}
        if(last){try{writeManifest(folder,manifest);}catch(const std::exception& e){std::lock_guard lock(mutex);stats.error=e.what();}
            std::lock_guard lock(mutex);stats.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();stats.finished=true;}
    }
};
RawRecorder::RawRecorder(const fs::path& folder,const std::vector<RecordedCamera>& cameras,const Settings& snapshot,uint64_t limit,RecordingOptions options):impl(std::make_unique<Impl>(folder,cameras,snapshot,limit,options)){}
RawRecorder::~RawRecorder()=default;
void RawRecorder::stop(std::string error){impl->stop(std::move(error));}
RecordingStats RawRecorder::statistics()const{std::lock_guard lock(impl->mutex);auto result=impl->stats;if(!result.finished)result.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-impl->started).count();return result;}
void RawRecorder::submit(size_t camera,RawFrameInfo info,std::span<const uint8_t> bytes)noexcept{
    try{std::lock_guard lock(impl->mutex);auto& s=impl->stats;if(!s.accepting)return;
        if(camera>=impl->sinks.size()||bytes.size()>maxFrameBytes||bytes.size()!=info.size){s.error="Invalid source frame; recording stopped.";s.accepting=false;impl->changed.notify_all();return;}
        auto& sink=impl->sinks[camera];
        if(sink.previous&&info.id>sink.previous&&info.id-sink.previous>1)s.gaps[camera]+=info.id-sink.previous-1;sink.previous=info.id;
        if(info.status)++s.invalid[camera];
        if(impl->options.fps>0){
            if(!impl->hasOrigin){impl->origin=info.hostNs;impl->hasOrigin=true;}
            uint64_t elapsed=info.hostNs>=impl->origin?info.hostNs-impl->origin:0;
            uint64_t bucket=uint64_t(static_cast<long double>(elapsed)*impl->options.fps/1000000000.L);
            if(sink.bucket!=UINT64_MAX&&bucket<=sink.bucket){++s.sampledOut[camera];return;}sink.bucket=bucket;
        }
        if(bytes.size()>impl->limit-s.queuedBytes||sink.queue.size()>=65536){s.error="Raw writer queue overflow; recording stopped.";s.accepting=false;impl->changed.notify_all();return;}
        info.sourceGaps=s.gaps[camera];info.sourceInvalid=s.invalid[camera];info.sampledOut=s.sampledOut[camera];
        sink.queue.push_back({camera,info,{bytes.begin(),bytes.end()}});s.queuedBytes+=bytes.size();impl->changed.notify_all();
    }catch(...){impl->stop("Cannot queue raw frame; recording stopped.");}
}
RawReplay::RawReplay(const fs::path& dir,std::stop_token stop):folder(dir){
    if(!fs::exists(folder/"session.json")||fs::file_size(folder/"session.json")>1024*1024)throw std::runtime_error("Choose a folder containing session.json.");
    std::ifstream in(folder/"session.json");json j;in>>j;
    if(j.at("schema_version")!=1||(j.at("format")!="irayple-raw-v1"&&j.at("format")!="irayple-raw-v2")||!j.at("cameras").is_array()||j["cameras"].empty()||j["cameras"].size()>3)throw std::runtime_error("Unsupported raw recording.");
    if(!j.value("complete",false)||!j.value("error",std::string{}).empty())warning="Partial recording: "+j.value("error",std::string{});
    if(!fs::exists(folder/"calibration.json")||fs::file_size(folder/"calibration.json")>16*1024*1024)throw std::runtime_error("Missing recording calibration snapshot.");
    settings=loadSettings(folder/"calibration.json");std::set<std::string> ids;
    for(const auto& item:j.at("cameras")){
        RecordedCamera c;c.serial=item.at("serial");c.model=item.at("model");c.name=item.at("name");c.settings=item.at("settings").get<std::map<std::string,std::string>>();c.configError=item.value("config_error",std::string{});
        if(c.serial.empty()||c.serial.size()>256||!ids.insert(c.serial).second)throw std::runtime_error("Invalid recording camera identity.");
        auto base=cameraFolder(folder,cameras.size());auto rawSize=fs::file_size(base/"frames.raw");std::ifstream frames(base/"frames.jsonl");if(!frames)throw std::runtime_error("Missing frame index.");
        std::vector<RawFrameInfo> entries;std::string line;uint64_t end=0,lastTime=0,previous=0;
        while(std::getline(frames,line)){
            if(stop.stop_requested())throw std::runtime_error("Replay loading cancelled.");
            if(line.size()>4096)throw std::runtime_error("Frame index entry is too large.");
            RawFrameInfo f;try{f=decode(json::parse(line));}catch(const std::exception&){if(frames.peek()==EOF){warning="Partial recording: incomplete final index entry.";break;}throw;}
            if(f.offset!=end||f.size>maxFrameBytes||f.storedSize>maxFrameBytes||f.offset>rawSize||f.storedSize>rawSize-f.offset){warning="Partial recording: incomplete raw payload.";break;}
            if(f.hostNs<lastTime||f.hostNs>INT64_MAX)throw std::runtime_error("Recording timestamps are not ordered or out of range.");lastTime=f.hostNs;end=f.offset+f.storedSize;
            if(j.at("format")=="irayple-raw-v2"){c.gaps=std::max(c.gaps,f.sourceGaps);c.invalid=std::max(c.invalid,f.sourceInvalid);}
            else if(previous&&f.id>previous+1)c.gaps+=f.id-previous-1;previous=f.id;
            if(!f.status){validateFrame(f,f.size);entries.push_back(f);}else if(j.at("format")=="irayple-raw-v1")++c.invalid;
            if(entries.size()>10000000)throw std::runtime_error("Recording index exceeds supported length.");
        }
        if(j.at("format")=="irayple-raw-v2"){auto camera=cameras.size();if(j.contains("gaps")&&j.at("gaps").size()>camera)c.gaps=std::max(c.gaps,j.at("gaps")[camera].get<uint64_t>());
            if(j.contains("invalid")&&j.at("invalid").size()>camera)c.invalid=std::max(c.invalid,j.at("invalid")[camera].get<uint64_t>());}
        if(entries.empty())throw std::runtime_error("A recorded camera has no valid frames.");
        if(c.gaps||c.invalid){if(!warning.empty())warning+="\n";warning+=c.serial+": "+std::to_string(c.gaps)+" missing frame IDs, "+std::to_string(c.invalid)+" invalid frames (skipped).";}
        cameras.push_back(std::move(c));index.push_back(std::move(entries));
    }
}
size_t RawReplay::nearest(size_t camera,uint64_t time)const{
    const auto& v=index.at(camera);auto it=std::lower_bound(v.begin(),v.end(),time,[](const auto& f,uint64_t t){return f.hostNs<t;});
    if(it==v.end())return v.size()-1;if(it==v.begin())return 0;
    return size_t(it-v.begin())-((time-(it-1)->hostNs<=it->hostNs-time)?1:0);
}
std::vector<uint8_t> RawReplay::readRaw(size_t camera,size_t frame)const{
    const auto& info=index.at(camera).at(frame);std::ifstream raw(cameraFolder(folder,camera)/"frames.raw",std::ios::binary);
    raw.seekg(std::streamoff(info.offset));std::vector<uint8_t> bytes(size_t(info.storedSize));raw.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));
    if(!raw)throw std::runtime_error("Cannot read recorded frame.");
    if(info.codec==0)return bytes;
    if(ZSTD_getFrameContentSize(bytes.data(),bytes.size())!=info.size)throw std::runtime_error("Compressed frame length does not match its index.");
    std::vector<uint8_t> decoded(size_t(info.size));auto count=ZSTD_decompress(decoded.data(),decoded.size(),bytes.data(),bytes.size());checkZstd(count);
    if(count!=info.size)throw std::runtime_error("Truncated compressed frame.");
    for(size_t n=size_t(info.filterStride);info.filterStride&&n<decoded.size();++n)decoded[n]=uint8_t(decoded[n]+decoded[n-info.filterStride]);
    return decoded;
}
ReplayFrame RawReplay::read(size_t camera,size_t frame)const{
    const auto& info=index.at(camera).at(frame);auto bytes=readRaw(camera,frame);return {decodeRawFrame(info,bytes),info};
}
