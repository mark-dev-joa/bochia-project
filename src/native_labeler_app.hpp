#pragma once
#include "native_labeler_client.hpp"
#include <glad/gl.h>
#include <imgui.h>
#include <functional>
#include <future>
#include <chrono>

namespace boccia::labeler {
struct Configuration {
    int port = 8767;
    bool startServer = true;
    bool rememberUi = true;
    std::filesystem::path source, project, exports;
};
struct FrameView {
    Json member, annotation;
    cv::Mat image;
    GLuint texture = 0, overlay = 0;
    std::vector<Json> history;
    bool dirty = false, repaint = true;
    float scale = 0, offsetX = 0, offsetY = 0;
};
struct JobResult {
    Json data;
    std::vector<FrameView> frames;
};
class Application {
public:
    explicit Application(Configuration config);
    ~Application();
    void tick();
    void draw();
    bool ready() const { return catalog_.is_object() && !frames_.empty(); }
    bool busy() const { return task_.valid(); }
    void showEditor(int camera = 0);
    void showSplit();
    void selectSceneForCapture(int recordingNumber, int index);
    const std::string& error() const { return error_; }
    bool canClose() const;
    void preserveRecovery();
private:
    enum class Page { Prepare, Review, Split, Export };
    enum class Tool { Select, Ai, Polygon, Brush, Box };
    Configuration config_;
    Client client_;
    Json catalog_, groups_ = Json::object(), splits_ = Json::object(), autoStatus_ = Json::object(), exportStatus_ = Json::object();
    std::vector<FrameView> frames_;
    std::future<JobResult> task_;
    std::function<void(JobResult)> completion_;
    int scene_ = 0, camera_ = 0, selected_ = 0, filter_ = 0, pageCaptureScene_ = -1;
    Page page_ = Page::Review;
    Tool tool_ = Tool::Select;
    bool editor_ = false, masks_ = true, boxes_ = true, contour_ = true, representatives_ = true, crops_ = true, erase_ = false, dragging_ = false, scrollToCurrent_ = false, replaceSelected_ = false;
    float brightness_ = 1.65f, brushSize_ = 6, radius_ = 16;
    Json proposal_;
    cv::Mat brushMask_;
    int replacement_ = -1;
    std::vector<cv::Point2d> polygon_;
    Json points_ = Json::array();
    cv::Point2d dragStart_, lastBrush_;
    size_t generation_ = 0;
    std::string error_, message_ = "촬영 데이터 연결 중…", busyLabel_;
    std::chrono::steady_clock::time_point changed_, poll_;
    void launch(std::string label, std::function<JobResult()> work, std::function<void(JobResult)> completed);
    void connect();
    void updateCatalog(Json catalog);
    void updateSummary(const Json& summary);
    void loadScene(int index, bool editor = false, int camera = 0);
    void navigate(int index);
    void switchCamera(int camera);
    void releaseFrames();
    FrameView& current();
    bool pending() const;
    bool guard();
    void checkpoint();
    void changed();
    void save(std::function<void()> next = {});
    void review(bool next, bool hold = false, int member = -1);
    void nextUnreviewed(bool skipCurrent = false);
    std::filesystem::path preferencesPath() const;
    void rememberPosition() const;
    void restorePosition();
    void cancel();
    void apply();
    void undo();
    void segment(cv::Point2d point, bool negative);
    void polygonProposal();
    void preparePage();
    void queue();
    void overview();
    void editor();
    void properties();
    void splitPage();
    void exportPage();
    void refresh();
    void confirmSplit();
    void uploadTexture(GLuint& texture, const cv::Mat& bgr, bool rgba = false);
    void repaint(FrameView& frame);
    void imageCanvas(FrameView& frame, ImVec2 size, bool editable, bool detail = false);
    void fit(FrameView& frame, ImVec2 size, bool detail);
    int hit(const FrameView& frame, cv::Point2d point) const;
    static const char* statusLabel(const Json& member);
};
}
