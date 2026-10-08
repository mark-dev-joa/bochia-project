#pragma once
#include "labeler_engine.hpp"
#include "native_labeler_view.hpp"
#include <glad/gl.h>
#include <imgui.h>
#include <array>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <unordered_map>

namespace boccia::labeler {
struct Configuration {
    bool rememberUi = true;
    std::filesystem::path source, project, exports;   // the recordings, the label file and the export folder
    std::filesystem::path models, root;               // the EfficientSAM files and the repository
};
struct FrameView {
    Json member, annotation;
    cv::Mat image;
    GLuint texture = 0, overlay = 0;
    std::vector<Json> history;
    std::vector<std::vector<std::vector<cv::Point>>> contours;   // per ball, refreshed by repaint()
    std::vector<Circle> circles;                                 // per ball, fitted to the mask by repaint()
    bool dirty = false, repaint = true;
    float scale = 0, offsetX = 0, offsetY = 0;                   // scale 0 = fit or zoom to the selected ball on next draw
};
struct JobResult {
    Json data;
    std::vector<FrameView> frames;
    std::shared_ptr<Engine> engine;   // set by the connect job
};
class Application {
public:
    explicit Application(Configuration config);
    ~Application();
    void tick();
    void draw();
    bool ready() const { return catalog_.is_object() && !frames_.empty(); }
    bool busy() const { return task_.valid(); }
    bool settled() const { return !busy() && !thumbTask_.valid() && thumbWanted_.empty() && colorStatus_.value("status", "") != "running" && circleStatus_.value("status", "") != "running" && missingStatus_.value("status", "") != "running" && propagationStatus_.value("status", "") != "running" && occlusionStatus_.value("status", "") != "running"; }
    bool idle() const;                               // settled and nothing waiting to be saved; an open proposal does not count
    void showEditor(int camera = 0);
    void showPage(int page);
    void selectSceneForCapture(int recordingNumber, int index);
    void selectForCapture(int camera, int rank);     // rank = left-to-right position of the ball in that camera
    void foldForCapture() { listOpen_ = sideOpen_ = summaryOpen_ = false; }
    void showShortcutsForCapture() { shortcutsWanted_ = true; }
    void showViewForCapture(int mode) { view_ = std::clamp(mode, 0, viewCount - 1); for (auto& frame : frames_) frame.repaint = true; }
    void proposeForCapture();                        // a polygon proposal around the selected ball, to picture the preview
    void confirmForCapture() { review(Scope::Scene, false); }   // confirms the scene, so the prompt can be pictured
    void moveOnForCapture() { later_ = [this] { nextAsk_ = false; advance(); }; }   // the second Space, as the key does it
    void missingTrialForCapture(int frames) { missingTrial_ = frames; missingAsk_ = true; startMissing(true, false); }   // a trial of the missing-ball pass, to picture its dialog
    void missingApplyForCapture(int frames) { missingAsk_ = true; startMissing(false, false, frames); }           // applies the pass to a few frames, for the check of the apply path
    void propagateForCapture() { startPropagation(); }   // runs the propagation, for the check of its path
    void occlusionTrialForCapture(int frames) { occlTrial_ = frames; occludeAsk_ = true; startOcclusion(true, false); }
    void occlusionApplyForCapture(int frames) { occludeAsk_ = true; startOcclusion(false, false, frames); }
    bool selfCheckStep(int step);                    // scripted logic checks for the hidden smoke run
    bool stageCheckStep(int step);                   // the same for split and export; needs a fully confirmed project
    bool missingScriptStep(int step);                // trials of the missing-ball pass, their assembly and the apply of one
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
    struct Paint { bool fill = true, boxes = false, circles = false, outlines = true; int selected = -1, skip = -1, only = -1; };   // only: marks of that ball alone
    using Thumbs = std::vector<std::pair<std::string, cv::Mat>>;
    Configuration config_;
    std::shared_ptr<Engine> engine_;                             // the labels, the recordings and the models, in this process
    Json catalog_, groups_ = Json::object(), splits_ = Json::object(), autoStatus_ = Json::object(), exportStatus_ = Json::object(), exports_ = Json::array(),
         colorStatus_ = Json::object(),                          // the background colour fill
         circleStatus_ = Json::object(),                         // the background circle refinement
         missingStatus_ = Json::object(),                        // the pass that adds the detector's balls the draft lacks
         propagationStatus_ = Json::object(),                    // confirmed labels carried to repeated frames
         occlusionStatus_ = Json::object();                      // balls marked partly hidden by themselves
    float occlCoverage_ = .5f, occlCircularity_ = .8f, occlAspect_ = .85f;   // the dialog's thresholds; a blurred whole ball keeps about 60 % of its rim
    bool occlReviewed_ = false; int occlTrial_ = 30;
    bool propagateForce_ = false;                                // the dialog's "also the frames already up to date"
    std::vector<FrameView> missingSamples_;                      // crops of a trial's new balls, shown in the dialog
    int missingCameras_ = 0, missingTrial_ = 30;                 // 0 colour only, 1 all cameras; frames per trial
    float missingCoverage_ = .8f, missingMinRadius_ = 6, missingMaxRadius_ = 45;
    Json missingTrials_ = Json::array();                         // the saved trials, as the engine lists them
    char trialName_[33] = "A";                                   // the name the next saved trial gets
    int combineA_ = 0, combineOp_ = 0, combineB_ = 0;            // the assembly row: trial, operator, trial
    std::vector<FrameView> frames_;
    std::future<JobResult> task_, ahead_;
    std::function<void(JobResult)> completion_;
    std::function<void()> later_;                                // UI actions run after the frame is drawn
    std::function<void()> queued_;                               // a Space pressed while a job ran: done as soon as the job ends
    std::vector<GLuint> doomed_;                                 // textures of replaced frames: deleted at the start of the next frame,
                                                                 // since a draw list built this frame may still show them
    std::vector<SceneRow> rows_;
    std::unordered_map<std::string, GLuint> thumbs_;
    std::vector<std::pair<std::string, bool>> thumbWanted_;
    std::future<Thumbs> thumbTask_;
    std::vector<std::vector<cv::Point>> proposalContours_;
    std::vector<std::string> picked_;                            // recordings ticked on the split page
    std::vector<int> history_;                                   // scenes shown before the current one, for the back button
    int scene_ = 0, camera_ = 0, selected_ = 0, filter_ = 0, pageCaptureScene_ = -1, aheadScene_ = -1, aheadAuto_ = -1,
        targetScenes_ = -1, emptyDrafts_ = 0,                    // counted from the catalog when targetScenes_ < 0
        aheadHits_ = 0;                                          // scenes shown from a fetch made ahead; the self-test reads it
    Page page_ = Page::Review;
    Tool tool_ = Tool::Select;
    bool editor_ = false, masks_ = true, boxes_ = true, circles_ = true, outlines_ = true, representatives_ = true, crops_ = true, dragging_ = false,
         includeReviewed_ = true,                                // the refine dialog: confirmed images too
         remeasure_ = false,                                     // the refine dialog: measure balls that have a circle again
         scrollToCurrent_ = false, rowsDirty_ = true, panning_ = false,
         stale_ = false,                                         // labels changed on the engine while another page was open
         aheadDirty_ = true,                                     // the scene to fetch ahead may have changed
         planEdited_ = false,                                    // the split plan on screen is not the one the engine has stored
         listOpen_ = true, sideOpen_ = true, summaryOpen_ = true, // the collapsible panels
         backing_ = false,                                       // the next scene load comes from the back button
         shortcutsWanted_ = false,                               // open the shortcut list on the next frame
         growAsk_ = false,                                       // the grow-masks dialog is open
         colorAsk_ = false,                                      // the fill-colours dialog is open
         circleAsk_ = false,                                     // the refine-circles dialog is open
         nextAsk_ = false,                                       // the scene is confirmed; the prompt asks whether to move on
         missingAsk_ = false,                                    // the missing-ball dialog is open
         propagateAsk_ = false,                                  // the propagation dialog is open
         occludeAsk_ = false;                                    // the occlusion-marking dialog is open
    float brightness_ = 1.65f, brushSize_ = 6, radius_ = 16, circleMargin_ = .5f;
    int view_ = 0;                                               // how the frames are shown: original, contrast, gray, edges, gradient, edge overlay
    static constexpr int viewCount = 6;
    static const char* const viewNames[viewCount];
    cv::Mat displayImage(const FrameView& frame) const;
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
    void loadScene(int index, bool editor = false, int camera = 0, std::string note = {});   // note replaces the usual status line
    void prefetch();
    void navigate(int index);
    void openPage(Page page);
    void switchCamera(int camera);
    void focus(int camera);
    void select(int camera, int ball);
    void cycle(int step);
    void nudge(int edge, bool expand);
    void releaseFrames();
    void deleteDoomed();                             // frees the textures releaseFrames() set aside
    FrameView& current();
    bool pending() const;
    bool guard();
    void checkpoint();
    void changed(bool repaint = true);          // a box nudge leaves the mask overlay as it is
    void save(std::function<void()> next = {}, int slot = -1);   // slot -1: the current camera
    int dirtySlot() const;                                        // a camera of the scene with unsaved edits, or -1
    void review(Scope scope, bool hold, int member = -1);
    void nextUnreviewed();
    int advanceIndex() const;                        // the scene after this one with a review target, else the next unreviewed
    void advance();                                  // after a confirm or hold: that scene; queued while a job runs
    void back();
    int nextUnreviewedIndex() const;
    std::filesystem::path preferencesPath() const;
    void rememberPosition() const;
    void restorePosition();
    void cancel();
    void apply();
    bool measure(Json& box);                         // Canny + RANSAC circle on a proposal, box from the circle; false when the fit failed
    void undo();
    void removeSelected();
    void removeBall(int slot, int ball);
    void setOccluded(int slot, int ball, bool value);   // from the checkbox on a crop; that camera becomes current first
    void confirmGrowth();                            // the dialog behind growAsk_
    void growMasks(int pixels);
    void confirmColors();                            // the dialog behind colorAsk_
    void fillColors();
    void confirmCircles();                           // the dialog behind circleAsk_
    void confirmNext();                              // the prompt behind nextAsk_: Space moves on, Esc stays
    void confirmMissing();                           // the dialog behind missingAsk_: options, count, trial, samples, apply
    void confirmPropagation();                       // the dialog behind propagateAsk_
    void confirmOcclusion();                         // the dialog behind occludeAsk_: thresholds, count, trial, samples, apply
    void startOcclusion(bool dryRun, bool countOnly, int limit = -1);
    void sampleGrid(const char* heading);            // the sample crops of a trial, with the note or the measured circle
    void startPropagation();                         // carries confirmed labels to repeated frames
    void startMissing(bool dryRun, bool countOnly, int limit = -1);   // runs the pass with the dialog's options; limit overrides the frame count
    void loadMissingSamples(Json samples = Json());  // crops of new balls: the last trial's, or the given ones
    void refreshMissingTrials();                     // the saved trials, from the engine
    void missingAction(const std::string& action, Json payload, std::string done);   // a trial action, then the list again
    void refineCircles();
    void segment(cv::Point2d point, bool negative);
    void segmentBox(cv::Rect2d box);
    void polygonProposal();
    void keys();
    void topBar();
    void shortcutsPopup(ImVec2 below);
    bool collapsedStrip(const char* id, bool& open, bool pointsRight, float height);
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
    void zoomBy(FrameView& frame, float factor);     // zooms around the middle of the canvas, for the +/- keys and rail buttons
    ImVec2 canvasSize_{0, 0};                        // the canvas drawn last, so a key can zoom around its middle
    void canvasInput(FrameView& frame, ImVec2 pos, ImVec2 size, bool hovered);
    void paintImage(FrameView& frame, ImVec2 a, ImVec2 b, cv::Rect2d region, Paint paint);
    void paintMarks(FrameView& frame, const std::function<ImVec2(double, double)>& at, Paint paint);
    void viewImage(int slot, ImVec2 a, ImVec2 b, bool compact);
    void emptyTile(ImVec2 pos, ImVec2 size);
    void canvasOverlays(FrameView& frame, ImVec2 pos, ImVec2 size);
    void toolRail(FrameView& frame, ImVec2 pos);     // the tools, undo, fit and zoom in their own column left of the canvas
    void sidePanel();
    void nudgePad(FrameView& frame, Json& box);
    void preparePage();
    void splitPage();
    void exportPage();
    void refreshStage();
    void proposeSplit();
    void mergePicked();
    void assignSplit(const std::string& recording, const std::string& split);
    void confirmSplit();
    void startExport();
    std::string dataVersion() const;
    bool splitConfirmed() const { return catalog_.value("split_confirmed", false) && !planEdited_; }
    void uploadTexture(GLuint& texture, const cv::Mat& bgr, bool rgba = false);
    void repaint(FrameView& frame);
    void fit(FrameView& frame, ImVec2 size, bool toBall);
    int hit(const FrameView& frame, cv::Point2d point) const;
    int target(const FrameView& frame, cv::Point2d point) const;
    std::vector<int> sorted(const FrameView& frame) const;
    static std::string cameraLabel(const Json& member);
};
}
