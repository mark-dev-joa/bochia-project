#pragma once
// The labeler engine the screens talk to, in this process: the recordings, the labels, the proposal tools and the
// background jobs. Every method answers with the JSON the Python HTTP engine used to send, so the screens did
// not change when the engine moved in.
#include "labeler_assist.hpp"
#include "labeler_native.hpp"
#include <memory>

namespace boccia::labeler {
struct EnginePaths {
    std::filesystem::path source, project, exports;   // the recordings, the label file and the export folder
    std::filesystem::path models;                     // the folder with the EfficientSAM onnx files
    std::filesystem::path root;                       // the repository, for the automatic draft's seed caches
};
class Engine {
public:
    explicit Engine(EnginePaths paths);               // scans the recordings and opens the labels; throws with the Python messages
    ~Engine();                                        // stops the background jobs and waits for them
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    const EnginePaths paths;
    Json catalog() const;                             // GET /api/catalog
    Json nativeCatalog() const;                       // GET /api/native/catalog
    Json frame(const std::string& recording, const std::string& camera, int index) const;        // GET /api/frame
    cv::Mat image(const std::string& recording, const std::string& camera, int index) const;     // GET /api/image, BGR
    cv::Mat thumbnail(const std::string& key, int width = 128);                                   // key = "recording/camera/index"
    Json saveAnnotation(const Json& payload);         // POST /api/annotation → {"summary"}
    Json saveSplits(const Json& payload);             // POST /api/splits → {"summary"}
    Json nativeAction(const std::string& action, const Json& payload);   // POST /api/native/<action>
    Json assist(const std::string& method, const Json& payload);        // POST /api/assist/<method>
    Json assistStatus() const;                        // GET /api/assist-status
    Json exportStatus() const;                        // GET /api/export-status
    Json exports() const;                             // GET /api/native/exports → {"exports": [...]}
    Json colorFillStatus() const;                     // GET /api/native/color-fill
    Json autoStatus() const;                          // GET /api/auto-status → {"job", "summary"}
    Json autoStart();                                 // POST /api/auto-start
    Json autoStop();                                  // POST /api/auto-stop
    Project& project() { return project_; }
    const Project& project() const { return project_; }
private:
    Catalog catalog_;
    Project project_;
    BallAssist assist_;
    Json autoJob_;
};
}
