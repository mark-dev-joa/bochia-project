#pragma once
#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "scene3d.hpp"

struct WindowRect { int x=100,y=100,width=1440,height=960; };
struct CameraDisplayOptions {
    int mode=0;
    float brightness=0,contrast=1;
    void validate() const;
    bool operator==(const CameraDisplayOptions&) const = default;
};
struct UiLayout {
    WindowRect mainWindow;
    bool maximized=false,devicesVisible=true,calibrationVisible=false;
    bool stereoVisible=false,originVisible=false;
    bool sceneVisible=false;
    bool processingVisible=false,recordingVisible=false;
    FpsView sceneView;
    SceneOptions sceneOptions;
    std::map<std::string,bool> cameraWindows;
    std::map<std::string,CameraDisplayOptions> cameraDisplay;
    bool comparisonVisible=false;
    std::array<std::string,2> comparisonSerials;
    int comparisonMode=1;
    bool comparisonLines=true;
    int comparisonLineCount=4;
    std::string imgui;
};
UiLayout loadUiLayout(const std::filesystem::path& path);
std::string serializeUiLayout(const UiLayout& layout);
void writeUiLayout(const std::filesystem::path& path,const UiLayout& layout);
WindowRect fitToWorkArea(WindowRect window,const std::vector<WindowRect>& workAreas);
