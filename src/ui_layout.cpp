#include "ui_layout.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using nlohmann::json;
static int readIntegerOption(const json& object,const char* key,int fallback,int minimum,int maximum){
    if(!object.contains(key))return fallback;
    const auto& value=object.at(key);
    if(!value.is_number_integer()||value.get<double>()<minimum||value.get<double>()>maximum)
        throw std::runtime_error("Invalid integer display option.");
    return value.get<int>();
}
void CameraDisplayOptions::validate() const {
    if(mode<0||mode>1||!std::isfinite(brightness)||brightness<-100||brightness>100||
       !std::isfinite(contrast)||contrast<.25f||contrast>3.f)
        throw std::runtime_error("Invalid camera display options.");
}
static void validate(const UiLayout& s){
    s.sceneView.validate();s.sceneOptions.validate();
    auto r=s.mainWindow;
    if(std::abs(double(r.x))>1000000||std::abs(double(r.y))>1000000||r.width<320||r.height<240||r.width>32768||r.height>32768)
        throw std::runtime_error("Invalid saved main window geometry.");
    if(s.imgui.size()>4*1024*1024||s.cameraWindows.size()>64||s.cameraDisplay.size()>64)throw std::runtime_error("UI layout is too large.");
    for(const auto& [serial,options]:s.cameraDisplay){
        if(serial.empty()||serial.size()>256)throw std::runtime_error("Invalid camera display identity.");
        options.validate();
    }
    if(s.comparisonMode<1||s.comparisonMode>2||s.comparisonLineCount<1||s.comparisonLineCount>12)
        throw std::runtime_error("Invalid stereo comparison options.");
    for(const auto& serial:s.comparisonSerials)if(serial.size()>256)throw std::runtime_error("Invalid comparison camera identity.");
    for(const auto& serial:s.triangulationSerials)if(serial.size()>256)throw std::runtime_error("Invalid triangulation camera identity.");
}
UiLayout loadUiLayout(const std::filesystem::path& path){
    UiLayout s;if(!std::filesystem::exists(path))return s;
    if(std::filesystem::file_size(path)>8*1024*1024)throw std::runtime_error("UI layout is too large.");
    std::ifstream file(path);json j;file>>j;
    if(j.at("schema_version")!=1)throw std::runtime_error("Unsupported UI layout schema.");
    const auto& w=j.at("main_window");s.mainWindow={w.at("x"),w.at("y"),w.at("width"),w.at("height")};
    s.maximized=w.at("maximized");s.devicesVisible=j.at("devices_visible");s.calibrationVisible=j.at("calibration_visible");
    s.stereoVisible=j.value("stereo_visible",false);
    s.originVisible=j.value("origin_visible",false);
    s.sceneVisible=j.value("scene_visible",false);
    s.processingVisible=j.value("processing_visible",false);s.recordingVisible=j.value("recording_visible",false);
    s.triangulationVisible=j.value("triangulation_visible",false);
    if(j.contains("triangulation_serials")){
        const auto& pair=j.at("triangulation_serials");if(!pair.is_array()||pair.size()!=2)throw std::runtime_error("Invalid triangulation camera pair.");
        s.triangulationSerials=pair.get<std::array<std::string,2>>();
    }
    if(j.contains("scene_view")){const auto& v=j.at("scene_view");auto eye=v.at("eye_m").get<std::vector<double>>();
        if(eye.size()!=3)throw std::runtime_error("Invalid saved 3D viewpoint.");s.sceneView.eye={eye[0],eye[1],eye[2]};
        s.sceneView.yaw=v.at("yaw");s.sceneView.pitch=v.at("pitch");s.sceneView.speed=v.at("speed_m_s");s.sceneView.initialized=v.at("initialized");}
    if(j.contains("scene_options")){const auto& v=j.at("scene_options");auto& o=s.sceneOptions;
        o.grid=v.at("grid");o.axes=v.at("axes");o.frusta=v.at("frusta");o.directions=v.at("directions");
        o.gridStep=v.at("grid_step_m");o.gridExtent=v.at("grid_extent_m");o.frustumLength=v.at("frustum_length_m");}
    s.cameraWindows=j.at("camera_windows").get<std::map<std::string,bool>>();s.imgui=j.at("imgui");
    if(j.contains("camera_display")){
        const auto& display=j.at("camera_display");
        if(!display.is_object())throw std::runtime_error("Invalid camera display options.");
        for(const auto& [serial,v]:display.items()){
            CameraDisplayOptions options;
            if(!v.is_object())throw std::runtime_error("Invalid camera display options.");
            options.mode=readIntegerOption(v,"mode",0,0,1);options.brightness=v.value("brightness",0.f);options.contrast=v.value("contrast",1.f);
            s.cameraDisplay.emplace(serial,options);
        }
    }
    s.comparisonVisible=j.value("comparison_visible",false);
    if(j.contains("comparison")){
        const auto& comparison=j.at("comparison");
        if(!comparison.is_object())throw std::runtime_error("Invalid stereo comparison options.");
        if(comparison.contains("serials")){
            auto serials=comparison.at("serials").get<std::vector<std::string>>();
            if(serials.size()!=2)throw std::runtime_error("Invalid comparison camera pair.");
            std::copy(serials.begin(),serials.end(),s.comparisonSerials.begin());
        }
        s.comparisonMode=std::max(1,readIntegerOption(comparison,"mode",1,0,2));s.comparisonLines=comparison.value("lines",true);
        s.comparisonLineCount=readIntegerOption(comparison,"line_count",4,1,12);
    }
    validate(s);return s;
}
std::string serializeUiLayout(const UiLayout& s){
    validate(s);auto r=s.mainWindow;
    json display=json::object();
    for(const auto& [serial,options]:s.cameraDisplay)
        display[serial]={{"mode",options.mode},{"brightness",options.brightness},{"contrast",options.contrast}};
    return json{{"schema_version",1},{"main_window",{{"x",r.x},{"y",r.y},{"width",r.width},{"height",r.height},{"maximized",s.maximized}}},
        {"devices_visible",s.devicesVisible},{"calibration_visible",s.calibrationVisible},{"stereo_visible",s.stereoVisible},{"origin_visible",s.originVisible},{"camera_windows",s.cameraWindows},{"imgui",s.imgui},
        {"camera_display",display},{"comparison_visible",s.comparisonVisible},{"processing_visible",s.processingVisible},{"recording_visible",s.recordingVisible},
        {"triangulation_visible",s.triangulationVisible},{"triangulation_serials",s.triangulationSerials},
        {"comparison",{{"serials",s.comparisonSerials},{"mode",s.comparisonMode},{"lines",s.comparisonLines},{"line_count",s.comparisonLineCount}}},
        {"scene_visible",s.sceneVisible},{"scene_view",{{"eye_m",{s.sceneView.eye[0],s.sceneView.eye[1],s.sceneView.eye[2]}},{"yaw",s.sceneView.yaw},{"pitch",s.sceneView.pitch},{"speed_m_s",s.sceneView.speed},{"initialized",s.sceneView.initialized}}},
        {"scene_options",{{"grid",s.sceneOptions.grid},{"axes",s.sceneOptions.axes},{"frusta",s.sceneOptions.frusta},{"directions",s.sceneOptions.directions},
            {"grid_step_m",s.sceneOptions.gridStep},{"grid_extent_m",s.sceneOptions.gridExtent},{"frustum_length_m",s.sceneOptions.frustumLength}}}}.dump(2);
}
void writeUiLayout(const std::filesystem::path& path,const UiLayout& s){
    auto encoded=serializeUiLayout(s);
    std::filesystem::create_directories(path.parent_path());auto temp=path;temp+=L".tmp";
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out<<encoded;out.flush();if(!out)throw std::runtime_error("UI layout write failed.");}
    loadUiLayout(temp);
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("UI layout replace failed.");
}
WindowRect fitToWorkArea(WindowRect w,const std::vector<WindowRect>& areas){
    if(areas.empty())return w;
    const WindowRect* best=&areas.front();double bestOverlap=-1,bestDistance=1e30;
    for(const auto& a:areas){
        double overlap=double(std::max(0,std::min(w.x+w.width,a.x+a.width)-std::max(w.x,a.x)))*
            std::max(0,std::min(w.y+w.height,a.y+a.height)-std::max(w.y,a.y));
        double distance=std::hypot(double(w.x)+w.width/2.-a.x-a.width/2.,double(w.y)+w.height/2.-a.y-a.height/2.);
        if(overlap>bestOverlap||(overlap==bestOverlap&&distance<bestDistance)){best=&a;bestOverlap=overlap;bestDistance=distance;}
    }
    w.width=std::min(w.width,best->width);w.height=std::min(w.height,best->height);
    w.x=std::clamp(w.x,best->x,best->x+best->width-w.width);
    w.y=std::clamp(w.y,best->y,best->y+best->height-w.height);return w;
}
