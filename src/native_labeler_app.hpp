#pragma once
#include "native_labeler_client.hpp"
#include "native_labeler_view.hpp"
#include <glad/gl.h>
#include <imgui.h>
#include <array>
#include <chrono>
#include <functional>
#include <future>
#include <unordered_map>

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
    std::vector<std::vector<std::vector<cv::Point>>> contours;   // per ball, refreshed by repaint()
    bool dirty = false, repaint = true;
    float scale = 0, offsetX = 0, offsetY = 0;                   // scale 0 = fit or zoom to the selected ball on next draw
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
    bool settled() const { return !busy() && !thumbTask_.valid() && thumbWanted_.empty(); }
    void showEditor(int camera = 0);
    void showPage(int page);
    void selectSceneForCapture(int recordingNumber, int index);
    void selectForCapture(int camera, int rank);     // rank = left-to-right position of the ball in that camera
    bool selfCheckStep(int step);                    // scripted logic checks for the hidden smoke run
    bool stageCheckStep(int step);                   // the same for split and export; needs a fully confirmed project
    const std::string& error() const { return error_; }
    bool canClose() const;
    void preserveRecovery();
private:
    enum class Page { Prepare, Review, Split, Export };
    enum class Tool { Select, Ai, Polygon, Brush, Box };
    enum class Scope { Scene, Camera };
    struct SceneRow {
        int scene = -1, recording = 0, index = 0, scenes = 0;
        float seconds = 0;
        bool header = false, warn = false, color = false;
        std::array<MemberState, 3> state{};
        std::array<int, 3> count{};
        int cameras = 0;
        std::string thumb;
    };
    struct Paint { bool fill = true, boxes = false; int selected = -1, skip = -1; };
    using Thumbs = std::vector<std::pair<std::string, cv::Mat>>;
    Configuration config_;
    Client client_;
    Json catalog_, groups_ = Json::object(), splits_ = Json::object(), autoStatus_ = Json::object(), exportStatus_ = Json::object(), exports_ = Json::array();
    std::vector<FrameView> frames_;
    std::future<JobResult> task_, ahead_;
    std::function<void(JobResult)> completion_;
    std::function<void()> later_;                                // UI actions run after the frame is drawn
    std::vector<SceneRow> rows_;
    std::unordered_map<std::string, GLuint> thumbs_;
    std::vector<std::pair<std::string, bool>> thumbWanted_;
    std::future<Thumbs> thumbTask_;
    std::vector<std::vector<cv::Point>> proposalContours_;
    std::vector<std::string> picked_;                            // recordings ticked on the split page
    int scene_ = 0, camera_ = 0, selected_ = 0, filter_ = 0, pageCaptureScene_ = -1, aheadScene_ = -1, aheadAuto_ = -1,
        targetScenes_ = -1, emptyDrafts_ = 0;                    // counted from the catalog when targetScenes_ < 0
    Page page_ = Page::Review;
    Tool tool_ = Tool::Select;
    bool editor_ = false, masks_ = true, boxes_ = false, representatives_ = true, crops_ = true, dragging_ = false,
         scrollToCurrent_ = false, rowsDirty_ = true, panning_ = false,
         stale_ = false;                                         // labels changed on the engine while another page was open
    float brightness_ = 1.65f, brushSize_ = 6, radius_ = 16;
    Json proposal_;
    cv::Mat brushMask_;
    int replacement_ = -1;
    std::vector<cv::Point2d> polygon_;
    Json points_ = Json::array();
    cv::Point2d dragStart_, lastBrush_;
    ImVec2 dragScreen_{};
    size_t generation_ = 0;
    std::string error_, message_ = "촬영 데이터 연결 중…", busyLabel_;
    std::chrono::steady_clock::time_point changed_, poll_;
    void launch(std::string label, std::function<JobResult()> work, std::function<void(JobResult)> completed);
    void connect();
    void updateCatalog(Json catalog);
    void updateSummary(const Json& summary);
    void loadScene(int index, bool editor = false, int camera = 0);
    void navigate(int index);
    void openPage(Page page);
    void switchCamera(int camera);
    void focus(int camera);
    void select(int camera, int ball);
    void cycle(int step);
    void nudge(int edge, bool expand);
    void releaseFrames();
    FrameView& current();
    bool pending() const;
    bool guard();
    void checkpoint();
    void changed();
    void save(std::function<void()> next = {});
    void review(Scope scope, bool hold, int member = -1);
    void nextUnreviewed();
    int nextUnreviewedIndex() const;
    std::filesystem::path preferencesPath() const;
    void rememberPosition() const;
    void restorePosition();
    void cancel();
    void apply();
    void undo();
    void segment(cv::Point2d point, bool negative);
    void segmentBox(cv::Rect2d box);
    void polygonProposal();
    void keys();
    void topBar();
    void errorBar();
    void statusBar();
    void rebuildRows();
    void stepRow(int step);
    GLuint thumb(const std::string& key, bool color);
    void pumpThumbs();
    void queue();
    void reviewPane();
    void sceneBar(ImVec2 pos, float width);
    void overviewTile(int slot, ImVec2 pos, ImVec2 size);
    void smallTile(int slot, ImVec2 pos, ImVec2 size);
    void focusTile(int slot, ImVec2 pos, ImVec2 size);
    void tileHeader(int slot, ImVec2 pos, float width, bool compact, const std::string& meta);
    void cropGrid(int slot, ImVec2 pos, ImVec2 size, bool compact);
    void canvas(FrameView& frame, ImVec2 pos, ImVec2 size);
    void canvasInput(FrameView& frame, ImVec2 pos, ImVec2 size, bool hovered);
    void paintImage(FrameView& frame, ImVec2 a, ImVec2 b, cv::Rect2d region, Paint paint);
    void paintMarks(FrameView& frame, const std::function<ImVec2(double, double)>& at, Paint paint);
    void viewImage(int slot, ImVec2 a, ImVec2 b, bool compact);
    void emptyTile(ImVec2 pos, ImVec2 size);
    void hintBar(ImVec2 pos, float width);
    void canvasOverlays(FrameView& frame, ImVec2 pos, ImVec2 size);
    void sidePanel();
    void nudgePad(FrameView& frame, Json& box);
    void preparePage();
    void splitPage();
    void exportPage();
    void refreshStage();
    void proposeSplit();
    void mergePicked();
    void confirmSplit();
    void startExport();
    std::string dataVersion() const;
    void uploadTexture(GLuint& texture, const cv::Mat& bgr, bool rgba = false);
    void repaint(FrameView& frame);
    void fit(FrameView& frame, ImVec2 size, bool toBall);
    int hit(const FrameView& frame, cv::Point2d point) const;
    std::vector<int> sorted(const FrameView& frame) const;
    static std::string cameraLabel(const Json& member);
};
}
