#pragma once
#include "calibration.hpp"
#include "auto_capture.hpp"
#include "brightness.hpp"
#include "stereo.hpp"
#include "rig.hpp"
#include "world_origin.hpp"
#include "ball_detection.hpp"
#include <optional>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <map>
struct SavedCamera {
    std::string identity,model,sourceKind,archive;
    std::map<std::string,std::string> geometry;
    BoardConfig board;
    Calibration calibration;
};
struct SavedStereo {
    std::string first,second,firstCalibrationArchive,secondCalibrationArchive,archive;
    BoardConfig board;
    StereoCalibration calibration;
    bool matches(const std::map<std::string,SavedCamera>& cameras)const;
};
struct RecordingOptions {
    bool lossless=true;
    double fps=30; // Zero keeps every source frame.
    void validate()const;
};
struct Settings {
    RecordingOptions recording;
    BallSettings ball;
    AutoCaptureConfig autoCapture;
    BoardConfig board;
    bool boardConfirmed=false;
    bool autoReconnect=true;
    std::vector<std::string> selectedSerials;
    std::vector<std::string> stereoSerials;
    std::map<std::string,SavedCamera> cameras;
    std::map<std::string,std::string> cameraNames;
    std::map<std::string,BrightnessOptions> softwareBrightness;
    std::optional<SavedStereo> stereo;
    struct SavedRig {
        std::vector<std::string> serials,intrinsicArchives;
        std::string archive;
        BoardConfig board;
        RigCalibration calibration;
        bool matches(const std::map<std::string,SavedCamera>& cameras)const;
        void validate()const;
    };
    std::optional<SavedRig> rig;
    struct SavedOrigin {
        std::string rigArchive,archive;
        WorldOrigin transform;
        std::string method="board",referenceCamera;
        RectangleOriginConfig rectangle;
        std::vector<cv::Point2f> points;
    };
    std::optional<SavedOrigin> origin;
    bool originCurrent()const{return origin&&rig&&origin->rigArchive==rig->archive&&rig->matches(cameras)&&origin->transform.errors.size()==rig->serials.size();}
    int relativeCameraCount=3;
};
Settings loadSettings(const std::filesystem::path& path);
void writeSettings(const std::filesystem::path& path,const Settings& settings);
enum class RecordState { Missing, Saved, Stale };
RecordState calibrationState(const Settings& settings,const std::string& serial,
    const std::map<std::string,std::string>* liveGeometry=nullptr);
struct SavedPoseView {
    RecordState state=RecordState::Missing;
    std::string reference;
    cv::Mat center,rotation; // Camera center and camera->reference orientation, metres.
    std::optional<double> rms;
    std::vector<std::string> members;
};
SavedPoseView savedPoseView(const Settings& settings,const std::string& serial);
SavedPoseView savedWorldPoseView(const Settings& settings,const std::string& serial);
// Geometry only: composed pairs have no independently measured reprojection error.
StereoCalibration savedStereoTransform(const Settings& settings,const std::string& first,const std::string& second);
std::optional<cv::Vec3d> cameraPointToWorld(const Settings& settings,const std::string& camera,cv::Vec3d point);
