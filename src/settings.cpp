#include "settings.hpp"
#include <fstream>
#include <stdexcept>
#include <cmath>
#include <set>
#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
using nlohmann::json;
void RecordingOptions::validate()const{if(!std::isfinite(fps)||fps<0||fps>1000||(fps>0&&fps<1))throw std::runtime_error("Recording FPS must be 1–1000, or zero for original FPS.");}
RecordState calibrationState(const Settings& s,const std::string& serial,const std::map<std::string,std::string>* live){
    auto it=s.cameras.find(serial);if(it==s.cameras.end())return RecordState::Missing;
    return it->second.sourceKind!="live"||(live&&*live!=it->second.geometry)?RecordState::Stale:RecordState::Saved;
}
SavedPoseView savedPoseView(const Settings& s,const std::string& serial){
    SavedPoseView out;
    auto set=[&](const std::string& reference,const StereoCalibration* edge,bool current,std::vector<std::string> members){
        out.state=current?RecordState::Saved:RecordState::Stale;out.reference=reference;out.members=std::move(members);
        out.center=cv::Mat::zeros(3,1,CV_64F);out.rotation=cv::Mat::eye(3,3,CV_64F);
        if(edge){out.center=edge->secondCenterInFirst();out.rotation=edge->R.t();out.rms=edge->rms;}
    };
    if(s.rig){auto it=std::find(s.rig->serials.begin(),s.rig->serials.end(),serial);
        if(it!=s.rig->serials.end()){size_t i=size_t(it-s.rig->serials.begin());
            set(s.rig->serials[0],i?&s.rig->calibration.fromReference[i-1]:nullptr,s.rig->matches(s.cameras),s.rig->serials);return out;}}
    if(s.stereo&&(serial==s.stereo->first||serial==s.stereo->second))
        set(s.stereo->first,serial==s.stereo->first?nullptr:&s.stereo->calibration,s.stereo->matches(s.cameras),{s.stereo->first,s.stereo->second});
    return out;
}
SavedPoseView savedWorldPoseView(const Settings& s,const std::string& serial){
    SavedPoseView out;
    if(!s.origin||!s.rig)return out;
    auto it=std::find(s.rig->serials.begin(),s.rig->serials.end(),serial);
    if(it==s.rig->serials.end())return out;
    out.reference="World";out.members=s.rig->serials;
    out.state=s.originCurrent()?RecordState::Saved:RecordState::Stale;
    if(out.state!=RecordState::Saved)return out;
    size_t i=size_t(it-s.rig->serials.begin());const auto& world=s.origin->transform;
    out.center=world.cameraCenter(s.rig->calibration,i);
    out.rotation=world.R.t();
    if(i)out.rotation=out.rotation*s.rig->calibration.fromReference[i-1].R.t();
    if(world.measured(i))out.rms=world.errors[i];
    return out;
}
void Settings::SavedRig::validate()const{
    calibration.validate();board.validate();
    if(serials.size()!=calibration.fromReference.size()+1||intrinsicArchives.size()!=serials.size()||
       std::set<std::string>(serials.begin(),serials.end()).size()!=serials.size())throw std::runtime_error("Invalid saved rig identities.");
    for(size_t i=0;i<serials.size();++i)if(serials[i].empty()||intrinsicArchives[i].empty())throw std::runtime_error("Missing rig calibration identity.");
}
std::optional<cv::Vec3d> cameraPointToWorld(const Settings& s,const std::string& camera,cv::Vec3d point){
    for(auto value:point.val)if(!std::isfinite(value))throw std::runtime_error("Invalid 3D point.");
    const auto pose=savedWorldPoseView(s,camera);if(pose.state!=RecordState::Saved)return {};
    cv::Mat world=pose.rotation*cv::Mat(point)+pose.center;
    return cv::Vec3d(world.at<double>(0),world.at<double>(1),world.at<double>(2));
}
bool Settings::SavedRig::matches(const std::map<std::string,SavedCamera>& cameras)const{
    for(size_t i=0;i<serials.size();++i){auto it=cameras.find(serials[i]);if(it==cameras.end()||it->second.sourceKind!="live"||it->second.archive!=intrinsicArchives[i])return false;}
    return true;
}
bool SavedStereo::matches(const std::map<std::string,SavedCamera>& cameras)const{
    auto a=cameras.find(first),b=cameras.find(second);
    return a!=cameras.end()&&b!=cameras.end()&&a->second.sourceKind=="live"&&b->second.sourceKind=="live"&&
        a->second.archive==firstCalibrationArchive&&b->second.archive==secondCalibrationArchive;
}
StereoCalibration savedStereoTransform(const Settings& s,const std::string& first,const std::string& second){
    if(first.empty()||second.empty()||first==second)throw std::runtime_error("서로 다른 카메라 2대를 선택하세요.");
    if(calibrationState(s,first)!=RecordState::Saved||calibrationState(s,second)!=RecordState::Saved)
        throw std::runtime_error("선택한 카메라의 개별 보정이 필요합니다.");
    if(s.rig){
        const auto& rig=*s.rig;
        const auto i=std::find(rig.serials.begin(),rig.serials.end(),first),j=std::find(rig.serials.begin(),rig.serials.end(),second);
        if(i!=rig.serials.end()&&j!=rig.serials.end()){
            rig.validate();
            if(!rig.matches(s.cameras))throw std::runtime_error("개별 보정이 변경되었습니다. 상대 위치를 다시 보정하세요.");
            auto fromReference=[&](size_t index){
                if(!index)return std::pair<cv::Mat,cv::Mat>{cv::Mat::eye(3,3,CV_64F),cv::Mat::zeros(3,1,CV_64F)};
                const auto& edge=rig.calibration.fromReference[index-1];return std::pair<cv::Mat,cv::Mat>{edge.R,edge.T};
            };
            const auto [ri,ti]=fromReference(size_t(i-rig.serials.begin()));
            const auto [rj,tj]=fromReference(size_t(j-rig.serials.begin()));
            StereoCalibration out;out.R=rj*ri.t();out.T=tj-out.R*ti;out.validate();return out;
        }
    }
    if(s.stereo&&((s.stereo->first==first&&s.stereo->second==second)||(s.stereo->first==second&&s.stereo->second==first))){
        const auto& pair=*s.stereo;
        if(!pair.matches(s.cameras))throw std::runtime_error("개별 보정이 변경되었습니다. 상대 위치를 다시 보정하세요.");
        pair.calibration.validate();StereoCalibration out;
        if(pair.first==first){out.R=pair.calibration.R.clone();out.T=pair.calibration.T.clone();}
        else {out.R=pair.calibration.R.t();out.T=-out.R*pair.calibration.T;}
        out.validate();return out;
    }
    throw std::runtime_error("선택한 카메라 사이의 상대 위치 보정이 필요합니다.");
}
static json boardJson(const BoardConfig& b){return {{"pattern",b.pattern==Pattern::Chessboard?"chessboard":"charuco"},{"columns",b.columns},{"rows",b.rows},{"square_mm",b.squareMm},{"marker_mm",b.markerMm},{"legacy",b.legacy},{"dictionary","DICT_4X4_50"}};}
static BoardConfig readBoard(const json& j){
    BoardConfig b;auto type=j.at("pattern").get<std::string>();
    if(type!="chessboard" && type!="charuco")throw std::runtime_error("Unknown board type.");
    if(j.value("dictionary",std::string("DICT_4X4_50"))!="DICT_4X4_50")throw std::runtime_error("Unsupported dictionary.");
    b.pattern=type=="chessboard"?Pattern::Chessboard:Pattern::Charuco;
    b.columns=j.at("columns");b.rows=j.at("rows");b.squareMm=j.at("square_mm");b.markerMm=j.at("marker_mm");b.legacy=j.value("legacy",false);b.validate();return b;
}
Settings loadSettings(const std::filesystem::path& path){
    Settings s;if(!std::filesystem::exists(path))return s;
    std::ifstream file(path);json j;file>>j;
    if(j.at("schema_version")!=1)throw std::runtime_error("Unsupported settings schema.");
    s.board=readBoard(j.at("board"));s.boardConfirmed=j.value("board_confirmed",false);
    if(j.contains("auto_capture")){
        const auto& a=j.at("auto_capture");s.autoCapture.target=a.at("target");
        s.autoCapture.holdSeconds=a.at("hold_seconds");s.autoCapture.intervalSeconds=a.at("interval_seconds");
        s.autoCapture.validate();
    }
    s.selectedSerials=j.value("selected_camera_serials",std::vector<std::string>{});
    s.cameraNames=j.value("camera_names",std::map<std::string,std::string>{});
    s.autoReconnect=j.value("auto_reconnect",true);
    if(j.contains("recording")){const auto& v=j.at("recording");s.recording.lossless=v.value("lossless",true);s.recording.fps=v.value("fps",30.);s.recording.validate();}
    if(j.contains("ball_detection"))s.ball=j.at("ball_detection").get<BallSettings>();
    s.stereoSerials=j.value("stereo_camera_serials",std::vector<std::string>{});
    s.relativeCameraCount=j.value("relative_camera_count",3);
    if(s.relativeCameraCount<2||s.relativeCameraCount>3)throw std::runtime_error("Invalid relative camera count.");
    if(j.contains("software_brightness"))for(const auto& [serial,value]:j.at("software_brightness").items()){
        BrightnessOptions option;option.enabled=value.at("enabled");option.fitted=value.at("fitted");
        option.blackLevel=value.at("black_level");option.whiteLevel=value.at("white_level");option.validate();
        s.softwareBrightness[serial]=option;
    }
    for(const auto& item:j.at("cameras")){
        SavedCamera camera;camera.identity=item.at("identity");camera.model=item.value("model","");
        camera.sourceKind=item.at("source_kind");camera.archive=item.at("archive");camera.geometry=item.at("geometry").get<std::map<std::string,std::string>>();
        camera.board=readBoard(item.at("board"));auto& c=camera.calibration;
        c.imageSize={item.at("width"),item.at("height")};c.rms=item.at("rms_px");
        auto k=item.at("K").get<std::vector<double>>();auto d=item.at("D").get<std::vector<double>>();
        if(k.size()!=9||d.size()!=5||c.imageSize.width<=0||c.imageSize.height<=0||!std::isfinite(c.rms)||c.rms<0)
            throw std::runtime_error("Invalid saved calibration shape.");
        c.K=cv::Mat(3,3,CV_64F,k.data()).clone();c.distortion=cv::Mat(1,5,CV_64F,d.data()).clone();
        if(!cv::checkRange(c.K)||!cv::checkRange(c.distortion)||k[0]<=0||k[4]<=0||std::abs(k[8]-1)>1e-8)
            throw std::runtime_error("Invalid saved calibration values.");
        if(camera.identity.empty()||s.cameras.contains(camera.identity))throw std::runtime_error("Invalid/duplicate camera identity.");
        s.cameras.emplace(camera.identity,std::move(camera));
    }
    if(j.contains("stereo")&&!j.at("stereo").is_null()){
        const auto& v=j.at("stereo");SavedStereo p;p.first=v.at("camera_A");p.second=v.at("camera_B");p.archive=v.at("archive");
        p.firstCalibrationArchive=v.at("intrinsics_A_archive");p.secondCalibrationArchive=v.at("intrinsics_B_archive");p.board=readBoard(v.at("board"));
        auto r=v.at("R_A_to_B").get<std::vector<double>>(),t=v.at("T_A_to_B_m").get<std::vector<double>>();
        if(r.size()!=9||t.size()!=3||p.first.empty()||p.second.empty()||p.first==p.second)throw std::runtime_error("Invalid stereo camera identities or transform shape.");
        p.calibration.R=cv::Mat(3,3,CV_64F,r.data()).clone();p.calibration.T=cv::Mat(3,1,CV_64F,t.data()).clone();
        p.calibration.rms=v.at("rms_px");p.calibration.perPairErrors=v.at("pair_errors_px").get<std::vector<double>>();
        p.calibration.secondRotations=v.at("B_corner_quarter_turns").get<std::vector<int>>();p.calibration.validate();s.stereo=std::move(p);
    }
    if(j.contains("rig")&&!j.at("rig").is_null()){
        const auto& v=j.at("rig");Settings::SavedRig p;p.serials=v.at("camera_serials").get<std::vector<std::string>>();
        p.intrinsicArchives=v.at("intrinsic_archives").get<std::vector<std::string>>();p.archive=v.at("archive");p.board=readBoard(v.at("board"));
        for(const auto& edge:v.at("from_reference")){
            auto r=edge.at("R").get<std::vector<double>>(),t=edge.at("T_m").get<std::vector<double>>();
            if(r.size()!=9||t.size()!=3)throw std::runtime_error("Invalid rig transform shape.");
            StereoCalibration c;c.R=cv::Mat(3,3,CV_64F,r.data()).clone();c.T=cv::Mat(3,1,CV_64F,t.data()).clone();c.rms=edge.at("rms_px");
            c.perPairErrors=edge.at("group_errors_px").get<std::vector<double>>();c.secondRotations=edge.at("corner_quarter_turns").get<std::vector<int>>();
            p.calibration.fromReference.push_back(std::move(c));
        }
        p.validate();s.rig=std::move(p);
    }
    if(j.contains("world_origin")){
        const auto& v=j.at("world_origin");Settings::SavedOrigin o;o.archive=v.at("archive");o.rigArchive=v.at("rig_archive");
        auto r=v.at("R_world_to_A").get<std::vector<double>>(),t=v.at("T_world_to_A_m").get<std::vector<double>>();
        if(r.size()!=9||t.size()!=3||o.archive.empty()||o.rigArchive.empty())throw std::runtime_error("Invalid saved origin.");
        o.transform.R=cv::Mat(3,3,CV_64F,r.data()).clone();o.transform.T=cv::Mat(3,1,CV_64F,t.data()).clone();
        o.transform.errors=v.at("camera_errors_px").get<std::vector<double>>();o.transform.quarterTurns=v.at("quarter_turns");o.transform.surfaceHeightMm=v.at("surface_height_mm");
        o.transform.measuredCameras=v.value("measured_camera_indices",std::vector<int>{});
        o.method=v.value("method",std::string("board"));o.referenceCamera=v.value("reference_camera",std::string{});
        if(o.method=="rectangle"){
            o.rectangle.widthMm=v.at("rectangle_width_mm");o.rectangle.heightMm=v.at("rectangle_height_mm");o.rectangle.validate();
            for(const auto& xy:v.at("points_px")){if(xy.size()!=2)throw std::runtime_error("Invalid rectangle corner.");o.points.emplace_back(xy.at(0).get<float>(),xy.at(1).get<float>());}
            if(o.referenceCamera.empty()||o.points.size()!=4||o.transform.measuredCameras.size()!=1)throw std::runtime_error("Invalid rectangle origin metadata.");
            for(auto p:o.points)if(!std::isfinite(p.x)||!std::isfinite(p.y))throw std::runtime_error("Invalid rectangle corner.");
            if(s.rig&&s.rig->archive==o.rigArchive){int index=o.transform.measuredCameras[0];
                if(index<0||size_t(index)>=s.rig->serials.size()||s.rig->serials[index]!=o.referenceCamera)throw std::runtime_error("Rectangle observation camera differs from saved rig.");}
        }else if(o.method!="board")throw std::runtime_error("Unknown origin method.");
        o.transform.validate();s.origin=std::move(o);
    }
    return s;
}
void writeSettings(const std::filesystem::path& path,const Settings& s){
    s.autoCapture.validate();
    s.board.validate();json j={{"schema_version",1},{"board",boardJson(s.board)},{"board_confirmed",s.boardConfirmed},{"selected_camera_serials",s.selectedSerials},{"camera_names",s.cameraNames},{"cameras",json::array()}};
    j["auto_capture"]={{"target",s.autoCapture.target},{"hold_seconds",s.autoCapture.holdSeconds},{"interval_seconds",s.autoCapture.intervalSeconds}};
    j["auto_reconnect"]=s.autoReconnect;
    s.recording.validate();j["recording"]={{"lossless",s.recording.lossless},{"fps",s.recording.fps}};
    j["ball_detection"]=s.ball;
    j["stereo_camera_serials"]=s.stereoSerials;
    j["relative_camera_count"]=s.relativeCameraCount;
    j["software_brightness"]=json::object();
    for(const auto& [serial,option]:s.softwareBrightness){option.validate();j["software_brightness"][serial]={{"enabled",option.enabled},{"fitted",option.fitted},{"black_level",option.blackLevel},{"white_level",option.whiteLevel}};}
    for(const auto& [id,v]:s.cameras){
        const auto& c=v.calibration;
        if(c.K.empty()||c.distortion.empty()||c.K.type()!=CV_64F||c.K.total()!=9||c.distortion.total()!=5||!cv::checkRange(c.K)||!cv::checkRange(c.distortion))throw std::runtime_error("Cannot save invalid calibration.");
        std::vector<double> k(c.K.ptr<double>(),c.K.ptr<double>()+9),d(c.distortion.ptr<double>(),c.distortion.ptr<double>()+5);
        j["cameras"].push_back({{"identity",id},{"model",v.model},{"source_kind",v.sourceKind},{"archive",v.archive},{"geometry",v.geometry},
            {"board",boardJson(v.board)},{"width",c.imageSize.width},{"height",c.imageSize.height},{"K",k},{"D",d},{"rms_px",c.rms}});
    }
    if(s.stereo){const auto& p=*s.stereo;const auto& c=p.calibration;c.validate();
        j["stereo"]={{"camera_A",p.first},{"camera_B",p.second},{"archive",p.archive},{"intrinsics_A_archive",p.firstCalibrationArchive},
            {"intrinsics_B_archive",p.secondCalibrationArchive},{"board",boardJson(p.board)},{"rms_px",c.rms},
            {"R_A_to_B",std::vector<double>(c.R.ptr<double>(),c.R.ptr<double>()+9)},{"T_A_to_B_m",std::vector<double>(c.T.ptr<double>(),c.T.ptr<double>()+3)},
            {"pair_errors_px",c.perPairErrors},{"B_corner_quarter_turns",c.secondRotations}};
    }
    if(s.rig){const auto& p=*s.rig;p.validate();json edges=json::array();
        for(const auto& c:p.calibration.fromReference)edges.push_back({{"R",std::vector<double>(c.R.ptr<double>(),c.R.ptr<double>()+9)},
            {"T_m",std::vector<double>(c.T.ptr<double>(),c.T.ptr<double>()+3)},{"rms_px",c.rms},{"group_errors_px",c.perPairErrors},{"corner_quarter_turns",c.secondRotations}});
        j["rig"]={{"camera_serials",p.serials},{"intrinsic_archives",p.intrinsicArchives},{"archive",p.archive},{"board",boardJson(p.board)},{"from_reference",edges}};
    }
    if(s.origin){const auto& o=*s.origin;const auto& c=o.transform;c.validate();
        j["world_origin"]={{"archive",o.archive},{"rig_archive",o.rigArchive},{"R_world_to_A",std::vector<double>(c.R.ptr<double>(),c.R.ptr<double>()+9)},
            {"T_world_to_A_m",std::vector<double>(c.T.ptr<double>(),c.T.ptr<double>()+3)},{"camera_errors_px",c.errors},{"quarter_turns",c.quarterTurns},{"surface_height_mm",c.surfaceHeightMm},
            {"convention","X_A = R_world_to_A * X_world + T_world_to_A_m; board center; Z toward cameras; right handed; metres"}};
        auto& v=j["world_origin"];v["method"]=o.method;v["measured_camera_indices"]=c.measuredCameras;
        if(o.method=="rectangle"){
            o.rectangle.validate();v["reference_camera"]=o.referenceCamera;v["rectangle_width_mm"]=o.rectangle.widthMm;v["rectangle_height_mm"]=o.rectangle.heightMm;v["points_px"]=json::array();
            for(auto p:o.points)v["points_px"].push_back({p.x,p.y});
        }
    }
    std::filesystem::create_directories(path.parent_path());auto temp=path;temp+=L".tmp";
    {std::ofstream file(temp,std::ios::binary|std::ios::trunc);file<<j.dump(2);file.flush();if(!file)throw std::runtime_error("Settings write failed.");}
    // Validate the complete replacement before committing it; never truncate the current profile.
    loadSettings(temp);
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Settings replace failed: "+std::to_string(GetLastError()));
}
