#include "rig.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

void RigCalibration::validate()const{
    if(fromReference.empty()||fromReference.size()>2)throw std::runtime_error("A rig needs two or three cameras.");
    for(const auto& edge:fromReference){edge.validate();if(edge.perPairErrors.size()!=fromReference.front().perPairErrors.size())throw std::runtime_error("Rig diagnostics must describe the same capture groups.");}
}
bool usableRigTiming(const std::vector<double>& times,double now){
    if(times.size()<2||times.size()>3||!std::isfinite(now))return false;
    for(double t:times)if(!std::isfinite(t)||t>now||now-t>1.5)return false;
    auto [lo,hi]=std::minmax_element(times.begin(),times.end());return *hi-*lo<=.1;
}
void validateRigSample(const RigSample& sample,const BoardConfig& board,size_t count){
    if(count<2||count>3||sample.cameras.size()!=count||!std::isfinite(sample.receiptSkewMs)||sample.receiptSkewMs<0||sample.receiptSkewMs>100)
        throw std::runtime_error("모든 카메라의 유효한 이미지와 100ms 이내 수신 시각이 필요합니다.");
    for(size_t i=1;i<count;++i)matchStereoPoints({sample.cameras[0],sample.cameras[i],sample.receiptSkewMs},board);
}
RigCalibration calibrateRig(const std::vector<RigSample>& samples,const BoardConfig& board,const std::vector<Calibration>& intrinsics,std::stop_token stop){
    if(intrinsics.size()<2||intrinsics.size()>3||samples.size()<10)throw std::runtime_error("개별 보정된 2~3대와 최소 10묶음이 필요합니다.");
    for(const auto& sample:samples){
        if(stop.stop_requested())throw std::runtime_error("Rig calculation cancelled.");
        validateRigSample(sample,board,intrinsics.size());
        for(size_t i=0;i<intrinsics.size();++i)if(sample.cameras[i].image.size()!=intrinsics[i].imageSize)throw std::runtime_error("Camera resolution differs from saved intrinsics.");
    }
    RigCalibration result;
    for(size_t i=1;i<intrinsics.size();++i){
        std::vector<StereoPair> pairs;for(const auto& sample:samples)pairs.push_back({sample.cameras[0],sample.cameras[i],sample.receiptSkewMs});
        result.fromReference.push_back(calibrateStereo(pairs,board,intrinsics[0],intrinsics[i],stop));
    }
    // No partial result is published when one of the cameras fails/cancels.
    result.validate();return result;
}
void RigCapture::start(size_t count,const AutoCaptureConfig& options){
    if(count<2||count>3)throw std::runtime_error("Select two or three cameras.");
    options.validate();captures.assign(count,AutoCapture{});for(auto& c:captures)c.start(options);
}
void RigCapture::stop(){for(auto& c:captures)c.stop();}
void RigCapture::tick(double now){for(auto& c:captures)c.tick(now);}
bool RigCapture::running()const{return !captures.empty()&&std::all_of(captures.begin(),captures.end(),[](const auto& c){return c.running();});}
float RigCapture::progress()const{float p=1;for(const auto& c:captures)p=std::min(p,c.progress());return captures.empty()?0:p;}
CaptureStatus RigCapture::status(size_t camera)const{return camera<captures.size()?captures[camera].status():CaptureStatus::Stopped;}
float RigCapture::progress(size_t camera)const{return camera<captures.size()?captures[camera].progress():0;}
bool RigCapture::observe(const RigSample& sample,const BoardConfig& board,const std::vector<RigSample>& existing,const std::vector<double>& times,double now){
    if(!running())return false;
    bool valid=times.size()==captures.size()&&usableRigTiming(times,now);
    try{validateRigSample(sample,board,captures.size());}catch(const std::exception&){valid=false;}
    bool ready=valid;
    for(size_t i=0;i<captures.size();++i){
        std::vector<Observation> previous;for(const auto& s:existing){if(s.cameras.size()!=captures.size())throw std::runtime_error("Mixed camera count in capture session.");previous.push_back(s.cameras[i]);}
        bool settled=captures[i].observe(valid?sample.cameras[i]:Observation{},board,previous,valid?times[i]:now,now);
        ready=ready&&settled;
    }
    return ready;
}
void RigCapture::captured(double now,size_t count){for(auto& c:captures)c.captured(now,count);}
void saveRigArchive(const std::string& directory,const std::vector<std::string>& serials,const BoardConfig& board,const std::vector<RigSample>& samples,
                    const std::vector<Calibration>& intrinsics,const RigCalibration& result){
    result.validate();
    if(serials.size()!=result.fromReference.size()+1||intrinsics.size()!=serials.size()||samples.size()!=result.fromReference.front().perPairErrors.size()||
       std::set<std::string>(serials.begin(),serials.end()).size()!=serials.size())throw std::runtime_error("Invalid rig archive identities or sample counts.");
    for(const auto& sample:samples)validateRigSample(sample,board,serials.size());
    namespace fs=std::filesystem;fs::path dir(std::u8string(directory.begin(),directory.end()));
    if(fs::exists(dir))throw std::runtime_error("Rig archive already exists.");fs::create_directories(dir);
    for(size_t i=0;i<serials.size();++i){
        std::vector<Observation> observations;for(const auto& s:samples)observations.push_back(s.cameras[i]);
        auto camera=intrinsics[i];camera.perViewErrors=result.fromReference[i?i-1:0].perPairErrors;
        saveCalibration(directory+"/"+char('A'+i),serials[i],board,observations,camera);
    }
    cv::FileStorage f("rig.yml",cv::FileStorage::WRITE|cv::FileStorage::MEMORY);
    f<<"schema_version"<<1<<"camera_serials"<<serials<<"reference_camera"<<serials[0]
     <<"convention"<<"X_i = R_A_to_i * X_A + T_A_to_i; metres; x right, y down, z forward"
     <<"diagnostics"<<"Per-view errors in camera subfolders are stereo pair RMS, not an intrinsic refit. A uses the A-B error."
     <<"from_reference"<<"[";
    for(size_t i=0;i<result.fromReference.size();++i){const auto& edge=result.fromReference[i];
        f<<"{"<<"target_camera"<<serials[i+1]<<"R"<<edge.R<<"T_m"<<edge.T<<"center_in_A_m"<<edge.secondCenterInFirst()<<"rms_px"<<edge.rms
         <<"group_errors_px"<<edge.perPairErrors<<"target_corner_quarter_turns"<<edge.secondRotations<<"}";
    }
    f<<"]"<<"group_receipt_span_ms"<<"[";for(const auto& s:samples)f<<s.receiptSkewMs;f<<"]";
    std::ofstream out(dir/"rig.yml",std::ios::binary);out<<f.releaseAndGetString();if(!out)throw std::runtime_error("Rig archive write failed.");
}
