#pragma once
#include "labeler_engine.hpp"
#include "labeler_workspace.hpp"
#include "native_labeler_view.hpp"
#include "hub_client.hpp"
#include "labeler_settings.hpp"
#include <glad/gl.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <unordered_map>
#include <atomic>
#include <mutex>

namespace boccia::labeler {
struct Configuration {
    bool rememberUi = true;
    bool training = false;
    bool startScreen = false;
    std::shared_ptr<void> lease;
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
    std::shared_ptr<Configuration> configuration;
};
class Application {
public:
    explicit Application(Configuration config);
    ~Application();
    void tick();
    void draw();
    bool ready() const { return catalog_.is_object() && !frames_.empty(); }
    bool projectOpened() const { return engine_ && catalog_.is_object(); }
    bool busy() const { return task_.valid(); }
    bool settled() const { return !busy() && !thumbTask_.valid() && thumbWanted_.empty() && colorStatus_.value("status", "") != "running" && circleStatus_.value("status", "") != "running" && missingStatus_.value("status", "") != "running" && propagationStatus_.value("status", "") != "running" && occlusionStatus_.value("status", "") != "running" && redraftStatus_.value("status", "") != "running" && (exportDemo_ || exportStatus_.value("status", "") != "running"); }
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
    void applyPreviousForCapture() { applyPreviousLabels(); }   // presses 이전 라벨 가져오기 on the scene shown
    // Opens the batch drawer; "redraft", "missing" or "occlusion" also starts that tool, on the scope after a colon
    // (0 the scene, 1 its capture or folder, 2 everything; "missing:1").
    void exportStateForCapture(const std::string& state);   // "dialog": the formats dialog; "run": an export of what is chosen
    void batchDrawerForCapture(std::string run) {
        if (const auto colon = run.find(':'); colon != std::string::npos) { batchScope_ = std::clamp(std::stoi(run.substr(colon + 1)), 0, 2); run.resize(colon); }
        drawerRun_ = std::move(run); openBatchTools();
    }
    // Opens 원형 검출 on the scope after a colon ("find:1"); "find" also searches, "add" searches and adds what it found.
    void circlesForCapture(std::string state) {
        if (const auto colon = state.find(':'); colon != std::string::npos) { batchScope_ = std::clamp(std::stoi(state.substr(colon + 1)), 0, 2); state.resize(colon); }
        if (state != "settings") circleRun_ = state;
        openCircles();
    }
    void showErrorForCapture(std::string text) { error_ = std::move(text); errorDemo_ = true; }   // pictures the floating error notice
    void occlusionTrialForCapture(int frames) { occlTrial_ = frames; occludeAsk_ = true; startOcclusion(true, false); }
    void occlusionApplyForCapture(int frames) { occludeAsk_ = true; startOcclusion(false, false, frames); }
    bool selfCheckStep(int step);                    // scripted logic checks for the hidden smoke run
    bool stageCheckStep(int step);                   // the same for split and export; needs a fully confirmed project
    bool missingScriptStep(int step);                // trials of the missing-ball pass, their assembly and the apply of one
    bool batchSplitStep(int step, const std::string& spec);   // assigns, confirms and exports from the command line
    const std::string& error() const { return errorDemo_ ? empty_ : error_; }   // a pictured error must not end a hidden run
    bool canClose() const;
    void preserveRecovery();
    void droppedFiles(std::vector<std::filesystem::path> files);
    void openProject(const std::filesystem::path& folderOrFile);
    void importPaths(std::vector<std::filesystem::path> files);
    void commitPlannedImport();
    bool importing() const { return importProgress_ && !importProgress_->finished; }
    void cancelImport() { if (importing()) importProgress_->cancelled = true; }
    bool exporting() const { return !exportDemo_ && exportStatus_.value("status", "") == "running"; }
    void stopRunningExport() { if (engine_) engine_->nativeAction("stop-export", Json::object()); }   // the close dialog's 중단하고 종료
    bool importDraftingForTest() const {             // an import has copied and started its drafts (hidden cancel check)
        if (!importProgress_) return false;
        std::lock_guard guard(importProgress_->lock);
        return std::find(importProgress_->states.begin(), importProgress_->states.end(), 2) != importProgress_->states.end();
    }
    void projectUiCheck(int step, const std::filesystem::path& first, const std::filesystem::path& second);
    void showNewProjectForCapture(const std::filesystem::path& parent = {}, const std::string& name = {});
    void createProjectForCapture() { createProject(); }
    void closeProjectForCapture() { closeProject(); }
    ImVec2 creationPopupSizeForCapture() const { return creationPopupSize_; }
private:
    // The labeler's tabs come first; Export is the dataset export, whose page is not built yet. The rest are the trainer's.
    enum class Page { Data, Review, Export, Split, TrainExport, Train };
    bool batchTools_ = false;
    int batchScope_ = 0;
    Json batchPayload_ = {{"scope",{{"mode","all"}}}}, batchStats_ = Json::object();
    void batchToolsPopup();
    void openBatchTools();
    void refreshBatchScope(bool drawer = true);   // the scope's counts; the batch drawer opens unless 원형 검출 asked
    // 원형 검출 (design/labeler-v3 circles.png, 2026-10-14): a dialog with the project's settings, a search over the scope
    // that shows what it found, then 더하기. The search is the missing-ball pass with circles for its finder.
    bool circlesWanted_ = false, circleApply_ = false;
    bool circleShowResult_ = false;                  // the dialog shows the last search's result instead of the settings
    int circleMin_ = 6, circleMax_ = 45, circleThreshold_ = 22, circleEdge_ = 90, circleSpacing_ = 12, circleLimit_ = 20;
    bool circleContrast_ = true;
    std::string circleRun_;                          // the hidden capture: "find" or "add" once the dialog is open
    void openCircles();
    void circlesPopup();
    void startCircleSearch();
    void restoreCircleSettings(const Json& stored);
    Json circleSettings() const;
    Json batchScopePayload() const;
    // The batch drawer (design/labeler-v3 batch.png): scope, 초안 다시 만들기, 빠진 공 찾기, 가려진 공 표시 and the
    // automatic settings. Trials run on the whole scope, so what the drawer shows is what 적용 writes.
    Json redraftStatus_ = Json::object();
    void applyPreviousLabels();                      // 이전 라벨 가져오기: the nearest earlier confirmed frame's labels onto this scene, as edits
    bool missingAvailable_ = false;                  // the project finds missing balls (its YOLO model, or the 10-02 file)
    // The export tab: the captures and folders chosen for export, the formats and the folder (remembered per project),
    // and the dialogs of the export itself.
    std::set<std::string> exportChosen_;
    bool exportChosenInit_ = false, exportAsk_ = false, exportWatching_ = false, exportDemo_ = false;
    bool exportYolo_ = true, exportCocoDetection_ = true, exportCocoMask_ = false, exportCrops_ = false;
    char exportFolder_[4096] = "";
    std::chrono::steady_clock::time_point exportStarted_{}, exportPolled_{};
    void exportTab();
    void exportPopups();
    void beginExport();
    Json exportSettings() const;
    void restoreExportSettings(const Json& saved);
    void rememberExportSettings();
    std::string drawerRun_;                          // the tool the hidden capture starts once the drawer is open
    bool drawerApply_ = false;                       // the hidden capture also applies the trial ("missing-apply")
    int samplesFrom_ = 0;                            // the tool whose trial the sample crops show: 1 missing, 2 occlusion
    std::array<std::string, 3> scopeNames() const;   // 현재 장면·현재 촬영 NN·전체 자료, or 현재 이미지·현재 폴더·전체 자료
    void startRedraft();
    void applyMissing();
    void stopJob(const char* action, const char* label);
    void drawerSamples(ImVec2 at, float width);
    void startDrafts();
    void importData();
    // An import in its dialog: the check, then the copy of every item, then each item's drafts, filter and circle fit.
    struct ImportStatus {
        std::atomic<bool> cancelled = false;
        std::mutex lock;
        std::string phase;                           // the check's step, or the stage of the current item
        uint64_t done = 0, total = 0;                // of that step or stage
        int item = -1;                               // the plan item being copied or finished
        std::vector<int> states;                     // per item: 0 waiting, 1 copying, 4 copied, 2 drafting/filtering/fitting, 3 done, 5 failed
        std::vector<std::string> reasons;            // per item: why it failed
        Json items = Json::array();                  // the items being imported, as planned (fixed once the import starts)
        std::vector<std::string> ids, titles;        // their IDs (to place the reports) and names
        std::vector<uint64_t> frames;                // images per item, for the time left
        double remaining = -1;                       // seconds, estimated; negative while unknown
        std::chrono::steady_clock::time_point copyStart{}, pipelineStart{};
        uint64_t finishedFrames = 0;                 // images of the items done
        bool finished = false;                       // the import has ended; the dialog shows how until 확인
        std::string outcome, reason;                 // "done", "partial", "stopped" or "failed", and the failure when nothing came in
    };
    std::shared_ptr<ImportStatus> importProgress_;
    std::shared_ptr<ImportPlan> importPlan_;
    std::vector<char> importChosen_;                 // per plan item: imported when set (the dialog's checkboxes)
    std::filesystem::path workspace_;
    std::string workspaceName_;
    Json recentProjects_ = Json::array(), importSummary_;
    bool createProjectWanted_ = false, importPreviewWanted_ = false;
    char projectName_[256] = "";
    char projectLocation_[4096] = "";
    std::string createProjectError_;
    ImVec2 creationPopupSize_{};
    int importType_ = 0;
    float projectControls();                         // the project button and its menu; returns the width it took
    void welcomePage();
    void beginCreateProject();
    void closeProject();
    void chooseProjectLocation();
    void projectPopups();
    void chooseProject();
    void chooseImportFolder();
    void createProject();
    void rememberProject(const std::filesystem::path& path);
    void activateProject(JobResult result);
    void resetProjectViews();
    bool prepareProjectChange(std::function<void()> after);
    bool imageScene() const;
    int draftSeeds_ = 0;                             // the 초안 후보 field: 0 the YOLO model, 1 circles
    char draftModel_[4096] = "";
    bool settingsWanted_ = false;                    // the project settings dialog is open
    std::string settingsError_;
    void draftField(float width);                    // the 초안 후보 field shared by the new-project and settings dialogs
    bool deviceGpu_ = true;                          // the 실행 장치 field: drafts and SAM on the GPU
    void deviceField(float width);
    void loadDevice();                               // the 실행 장치 is kept for the PC (labeler-settings.json), not the project
    void saveDevice(bool gpu);
    // The hub (design/hub): this PC's worker name and invite code, the client that reports to it and the notice it sent.
    struct HubShared { std::mutex lock; hub::Registration registration; };   // what /register carries; read on the client's thread
    LabelerSettings settings_;
    std::shared_ptr<hub::Client> hub_;               // shared: the log sink holds it weakly, from any thread that logs
    std::shared_ptr<HubShared> hubShared_ = std::make_shared<HubShared>();
    std::chrono::steady_clock::time_point hubTicked_{}, hubStarted_{};
    Json hubProgress_;                               // hubProgress(): counted again when the labels' revision changes or every 10 s
    int hubProgressRevision_ = -1;
    std::chrono::steady_clock::time_point hubProgressAt_{};
    std::string exportLogged_;                       // the folder of the last export whose end went to the log
    std::chrono::seconds hubWait_{0};                // --hub-wait: a hidden run stays at least this long, so a check can send it commands
    bool workerDialog_ = false;
    std::string hubNotice_;
    char workerName_[128] = "", workerToken_[128] = "", hubAddress_[256] = "";
    std::array<ImGuiID, 3> workerFieldIds_{};       // the dialog's three fields as drawn last, for workerDialogCheckStep
    std::array<ImVec2, 3> workerFieldMin_{}, workerFieldMax_{};
    void startHub();                                 // the client from settings_, or its new settings; none without a worker name
    void hubTick();                                  // at the end of tick(): the status, the registration and the commands
    void openWorkerDialog();
    void workerPopup();
    void saveWorker();
    Json hubScene() const;                           // the scene on screen, for the status; null when none
    Json hubJob() const;                             // the running job, for the status; null when none
    std::string hubSceneName() const;                // "촬영 03 · 프레임 0142", for the event lines
    hub::Registration currentRegistration() const;
    void chooseDraftModel();
    void beginProjectSettings();
    void saveProjectSettings();
public:
    void recentForCapture(const std::filesystem::path& file);   // a recent-project list for the hidden start-screen capture
    void projectSettingsForCapture() { beginProjectSettings(); }
    void workerDialogForCapture() { openWorkerDialog(); }
    bool workerDialogCheckStep(int step);            // hidden check: a click on each later field keeps the keyboard there; false when done
    bool sceneBarIdCheckStep(int step);              // hidden check: no two items of the scene bar share an ImGui ID; false when done
    bool applyPreviousCheckStep();                   // hidden check: the button carries the nearest confirmed labels, then the ones just confirmed
    void hubForCheck(const std::string& address, const std::string& worker, const std::string& token, int waitSeconds = 0);   // --hub --worker --token: joins with these instead of the settings
    bool hubSettled() const;                         // no hub, or the hub has answered, or 5 s have passed, and --hub-wait is over: a hidden check may end
    void importStateForCapture(const std::string& state);       // the import dialog while it checks or copies, for a capture
private:
    enum class Tool { Select, Ai, Polygon, Brush, Box };
    enum class Scope { Scene, Camera };
    struct SceneRow {
        int scene = -1, recording = 0, index = 0, scenes = 0;
        float seconds = 0;
        bool header = false, warn = false, color = false;
        bool image = false;
        std::array<MemberState, 3> state{};
        std::array<int, 3> count{};
        int cameras = 0;
        std::string thumb;
    };
    struct Paint { bool fill = true, boxes = false, circles = false, outlines = true, faint = false; int selected = -1, skip = -1, only = -1; };   // only: marks of that ball alone; faint: dimmed
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
    bool propagateCarry_ = true;                                 // the dialog's "also the empty drafts after a confirmed frame"
    bool fitCircle_ = true;                                      // proposals take the circle fit; off keeps the outline as segmented (hand-held balls)
    std::vector<FrameView> missingSamples_;                      // crops of a trial's new balls, shown in the dialog
    int missingCameras_ = 0, missingTrial_ = 30;                 // 0 colour only, 1 all cameras; frames per trial
    float missingCoverage_ = .8f, missingMinRadius_ = 6, missingMaxRadius_ = 45;
    Json missingTrials_ = Json::array();                         // the saved trials, as the engine lists them
    char trialName_[33] = "A";                                   // the name the next saved trial gets
    int combineA_ = 0, combineOp_ = 0, combineB_ = 0;            // the assembly row: trial, operator, trial
    std::vector<FrameView> frames_;
    std::future<JobResult> task_, ahead_;
    std::future<JobResult> pollTask_;                            // the status check of the background jobs: its own thread, never in the way (2026-10-19)
    std::function<void(JobResult)> pollDone_;
    void poll();
    void refreshCatalog(bool reloadScene);
    int runningJobs() const;
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
         nextAsk_ = false,                                       // the scene is confirmed; the prompt asks whether to move on
         missingAsk_ = false,                                    // the missing-ball dialog is open
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
    std::string loggedError_, loggedMessage_;      // what the log (labeler_log.hpp) last got of the two
    std::chrono::steady_clock::time_point changed_, poll_;
    void launch(std::string label, std::function<JobResult()> work, std::function<void(JobResult)> completed);
    void connect(std::string note = {});            // note replaces the status line once the data is open
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
    void checkpoint(FrameView& frame);
    void changed(bool repaint = true);          // a box nudge leaves the mask overlay as it is
    void edited(FrameView& frame, bool repaint);   // changed() for any frame of the scene, without the message
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
    Json chosenProposal() const;                     // the proposal as it will be applied: the fit's version, or the raw outline with the fit off
    void undo();
    void removeSelected();
    void removeBall(int slot, int ball);
    void setOccluded(int slot, int ball, bool value);   // from the checkbox on a crop; that camera becomes current first
    void confirmNext();                              // the prompt behind nextAsk_: Space moves on, Esc stays
    void confirmMissing();                           // the dialog behind missingAsk_: options, count, trial, samples, apply
    void confirmOcclusion();                         // the dialog behind occludeAsk_: thresholds, count, trial, samples, apply
    void startOcclusion(bool dryRun, bool countOnly, int limit = -1);
    void sampleGrid(const char* heading);            // the sample crops of a trial, with the note or the measured circle
    void startPropagation();                         // carries confirmed labels to repeated frames
    void startMissing(bool dryRun, bool countOnly, int limit = -1);   // runs the pass with the dialog's options; limit overrides the frame count
    void loadMissingSamples(Json samples = Json());  // crops of new balls: the last trial's, or the given ones
    void refreshMissingTrials();                     // the saved trials, from the engine
    void missingAction(const std::string& action, Json payload, std::string done);   // a trial action, then the list again
    void segment(cv::Point2d point, bool negative);
    void segmentBox(cv::Rect2d box);
    void polygonProposal();
    void keys();
    void topBar();
    void shortcutsPopup(ImVec2 below);
    bool collapsedStrip(const char* id, bool& open, bool pointsRight, float height);
    void errorOverlay();                             // the error as a floating notice below the top bar; it never moves the page
    std::chrono::steady_clock::time_point errorShown_{};
    std::string errorShownText_;
    void statusBar();
    void rebuildRows();
    void stepRow(int step);
    GLuint thumb(const std::string& key, bool color);
    void pumpThumbs();
    void queue();
    void reviewPane();
    void sceneBar(ImVec2 pos, float width);
    ImVec2 sceneBarMin_{}, sceneBarMax_{};             // the scene bar as drawn last, for sceneBarIdCheckStep
    void overviewTile(int slot, ImVec2 pos, ImVec2 size);
    void smallTile(int slot, ImVec2 pos, ImVec2 size);
    void focusTile(int slot, ImVec2 pos, ImVec2 size);
    void tileHeader(int slot, ImVec2 pos, float width, bool compact, const std::string& meta);
    void cropGrid(int slot, ImVec2 pos, ImVec2 size, bool compact, int columns = 2);
    void canvas(FrameView& frame, ImVec2 pos, ImVec2 size);
    void zoomBy(FrameView& frame, float factor);     // zooms around the middle of the canvas, for the +/- keys and rail buttons
    ImVec2 canvasSize_{0, 0};                        // the canvas drawn last, so a key can zoom around its middle
    struct ViewMemory { float scale = 0, offsetX = 0, offsetY = 0; };
    std::vector<ViewMemory> lastView_;               // per camera: the zoom and position last used in its large view
    void rememberView(const FrameView& frame);       // from the canvas, every frame
    void addBallHere(int slot);                      // "공 추가" on a crop: the large view at the last zoom, the AI tool ready
    bool errorDemo_ = false; const std::string empty_;
    bool adding_ = false;                            // "공 추가" in progress: the existing balls' marks are faint and their badges hidden
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
