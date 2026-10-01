#include "ui_layout.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void initImGui(){
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
    io.DisplaySize={1600,1000};io.DeltaTime=1.f/60;io.ConfigFlags|=ImGuiConfigFlags_DockingEnable;
    unsigned char* pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    ImGui::NewFrame();ImGui::EndFrame();
}
static void drawFrame(bool setCalibrationPosition=false){
    ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({1000,850});
    ImGui::Begin("Host",nullptr,ImGuiWindowFlags_NoSavedSettings);
    ImGui::DockSpace(0x1234,{950,750});ImGui::End();
    for(const auto* name:{"Camera A","Camera B"}){ImGui::Begin(name);ImGui::TextUnformatted("Connection pending");ImGui::End();}
    if(setCalibrationPosition){ImGui::SetNextWindowPos({1030,110});ImGui::SetNextWindowSize({480,650});}
    ImGui::Begin("Calibration",nullptr,ImGuiWindowFlags_NoDocking);ImGui::End();ImGui::Render();
}
int main(){try{
    const auto directory=std::filesystem::temp_directory_path()/("boccia-layout-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto path=directory/"layout.json";
    initImGui();ImGui::NewFrame();ImGui::DockBuilderAddNode(0x1234,ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(0x1234,{950,750});ImGuiID left,right;
    ImGui::DockBuilderSplitNode(0x1234,ImGuiDir_Left,.4f,&left,&right);
    ImGui::DockBuilderDockWindow("Camera A",left);ImGui::DockBuilderDockWindow("Camera B",right);ImGui::DockBuilderFinish(0x1234);ImGui::EndFrame();
    drawFrame(true);drawFrame();
    const ImGuiID savedDockA=ImGui::FindWindowByName("Camera A")->DockId,savedDockB=ImGui::FindWindowByName("Camera B")->DockId;
    require(savedDockA&&savedDockB&&savedDockA!=savedDockB,"Split docking constructed");
    UiLayout state;state.mainWindow={-1700,40,1300,900};state.maximized=true;state.devicesVisible=false;state.calibrationVisible=true;state.stereoVisible=true;
    state.sceneVisible=true;state.sceneView.eye={.3,-.5,1.2};state.sceneView.yaw=.7;state.sceneView.pitch=-.4;state.sceneView.speed=.8;state.sceneView.initialized=true;
    state.sceneOptions.gridStep=.2;state.sceneOptions.frusta=false;
    state.originVisible=true;state.cameraWindows={{"serial-A",true},{"serial-B",false}};state.imgui=ImGui::SaveIniSettingsToMemory();
    state.cameraDisplay={{"serial-A",{1,24.f,1.75f}},{"serial-B",{0,-17.f,.75f}}};
    state.comparisonVisible=true;state.comparisonSerials={"serial-B","serial-A"};state.comparisonMode=2;
    state.comparisonLines=false;state.comparisonLineCount=7;
    state.triangulationVisible=true;state.triangulationSerials={"serial-A","serial-B"};
    writeUiLayout(path,state);ImGui::DestroyContext();
    auto restored=loadUiLayout(path);
    require(restored.triangulationVisible&&restored.triangulationSerials==state.triangulationSerials,"Triangulation panel and selected camera identities persist");
    require(restored.mainWindow.x==-1700&&restored.mainWindow.width==1300&&restored.maximized,"Main geometry and maximize persist");
    require(!restored.devicesVisible&&restored.calibrationVisible&&restored.stereoVisible&&restored.originVisible&&!restored.cameraWindows.at("serial-B"),"Panel visibility persists");
    require(restored.imgui==state.imgui,"Complete ImGui settings persist losslessly");
    require(restored.cameraDisplay==state.cameraDisplay,"Display adjustments persist independently for each serial");
    const std::vector<std::string> reorderedDevices={"serial-B","new-camera","serial-A"};
    require(restored.cameraDisplay.at(reorderedDevices[0]).brightness==-17.f&&
        restored.cameraDisplay.at(reorderedDevices[2]).mode==1&&!restored.cameraDisplay.contains(reorderedDevices[1]),
        "Device discovery order and a new camera do not exchange display adjustments");
    require(restored.comparisonVisible&&restored.comparisonSerials==state.comparisonSerials&&restored.comparisonMode==2&&
        !restored.comparisonLines&&restored.comparisonLineCount==7,"Comparison pair order, mode and guide options persist");
    require(restored.sceneVisible&&restored.sceneView.initialized&&cv::norm(restored.sceneView.eye-state.sceneView.eye)<1e-10&&
        restored.sceneView.yaw==.7&&restored.sceneView.pitch==-.4&&restored.sceneView.speed==.8&&restored.sceneOptions.gridStep==.2&&!restored.sceneOptions.frusta,"3D view pose, speed, options and visibility persist");
    initImGui();ImGui::LoadIniSettingsFromMemory(restored.imgui.data(),restored.imgui.size());drawFrame();drawFrame();
    auto* a=ImGui::FindWindowByName("Camera A");auto* b=ImGui::FindWindowByName("Camera B");auto* calibration=ImGui::FindWindowByName("Calibration");
    require(a->DockId==savedDockA&&b->DockId==savedDockB,"Camera docking survives ImGui context restart");
    require(calibration->Pos.x==1030&&calibration->Pos.y==110&&calibration->SizeFull.x==480&&calibration->SizeFull.y==650,"Separate calibration position and size survive restart");
    ImGui::DestroyContext();
    auto fit=fitToWorkArea(state.mainWindow,{{0,0,1920,1040}});
    require(fit.x>=0&&fit.y>=0&&fit.x+fit.width<=1920&&fit.y+fit.height<=1040,"Disconnected monitor returns window to visible area");
    auto unchanged=fitToWorkArea(state.mainWindow,{{0,0,1920,1040},{-1920,0,1920,1040}});
    require(unchanged.x==state.mainWindow.x&&unchanged.width==state.mainWindow.width,"Connected negative-coordinate monitor preserved");
    auto invalid=state;invalid.mainWindow.width=0;bool rejected=false;
    try{writeUiLayout(path,invalid);}catch(const std::exception&){rejected=true;}
    require(rejected&&loadUiLayout(path).imgui==state.imgui,"Invalid save preserves previous layout");
    const auto preserved=serializeUiLayout(loadUiLayout(path));
    for(int i=0;i<7;++i){
        invalid=state;
        switch(i){
            case 0:invalid.cameraDisplay["serial-A"].brightness=101;break;
            case 1:invalid.cameraDisplay["serial-A"].contrast=.1f;break;
            case 2:invalid.cameraDisplay["serial-A"].contrast=std::numeric_limits<float>::quiet_NaN();break;
            case 3:invalid.cameraDisplay["serial-A"].mode=2;break;
            case 4:invalid.comparisonMode=3;break;
            case 5:invalid.comparisonLineCount=0;break;
            case 6:invalid.comparisonSerials[0]=std::string(257,'x');break;
        }
        rejected=false;try{writeUiLayout(path,invalid);}catch(const std::exception&){rejected=true;}
        require(rejected&&serializeUiLayout(loadUiLayout(path))==preserved,"Invalid display preferences preserve the complete existing file");
    }
    auto old=nlohmann::json::parse(preserved);old.erase("camera_display");old.erase("comparison_visible");old.erase("comparison");
    {std::ofstream f(directory/"legacy.json");f<<old;}
    const auto legacy=loadUiLayout(directory/"legacy.json");
    require(legacy.cameraDisplay.empty()&&!legacy.comparisonVisible&&legacy.comparisonSerials[0].empty()&&
        legacy.comparisonSerials[1].empty()&&legacy.comparisonMode==1&&legacy.comparisonLines&&legacy.comparisonLineCount==4,
        "Old schema defaults to original camera display and comparison guide defaults");
    old["comparison"]={{"mode",0}};
    {std::ofstream f(directory/"legacy-raw.json");f<<old;}
    require(loadUiLayout(directory/"legacy-raw.json").comparisonMode==1,"Removed Raw comparison mode migrates to Undistorted");
    require(legacy.imgui==state.imgui&&legacy.cameraWindows==state.cameraWindows,"New display fields preserve old docking and camera visibility");
    auto malformed=nlohmann::json::parse(preserved);malformed["comparison"]["serials"]={"serial-A"};
    {std::ofstream f(directory/"bad-pair.json");f<<malformed;}
    rejected=false;try{loadUiLayout(directory/"bad-pair.json");}catch(const std::exception&){rejected=true;}
    require(rejected,"A malformed saved camera pair is rejected");
    malformed=nlohmann::json::parse(preserved);malformed["camera_display"]["serial-A"]["mode"]=.5;
    {std::ofstream f(directory/"bad-mode.json");f<<malformed;}
    rejected=false;try{loadUiLayout(directory/"bad-mode.json");}catch(const std::exception&){rejected=true;}
    require(rejected,"Fractional display modes are rejected instead of silently truncated");
    {std::ofstream f(directory/"broken.json");f<<"{broken";}
    rejected=false;try{loadUiLayout(directory/"broken.json");}catch(const std::exception&){rejected=true;}
    require(rejected,"Corrupt layout rejected independently from calibration");
    std::cout<<"PASS: UI layout persistence, docking restart, window geometry, visibility, monitor fallback, failed-write preservation\n";return 0;
}catch(const std::exception& e){if(ImGui::GetCurrentContext())ImGui::DestroyContext();std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
