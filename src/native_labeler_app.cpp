#include "native_labeler_app.hpp"
#include "labeler_log.hpp"
#include "native_labeler_theme.hpp"
#include "labeler_assist.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>
#include <iomanip>
#include <sstream>
#include <windows.h>

namespace boccia::labeler {
namespace {
using Clock = std::chrono::steady_clock;
std::string title(const Json& scene) { return "촬영 " + std::to_string(scene.at("recording_number").get<int>()) + " · 프레임 " + std::to_string(scene.at("index").get<int>()); }
}
Application::Application(Configuration config): config_(std::move(config)), poll_(Clock::now()) {
    const auto legacyProject=config_.project;const bool startScreen=config_.startScreen;
    page_=config_.training?Page::Split:Page::Data;
    if(config_.startScreen){config_.source.clear();config_.project.clear();message_.clear();}
    else {
        std::string recovery;
        if(!config_.project.empty()&&std::filesystem::is_regular_file(config_.source.parent_path()/"boccia-project.json")){
            workspace_=config_.source.parent_path();workspaceName_=readWorkspace(workspace_).value("name", "프로젝트");
            // An import a crash interrupted is undone before the labels open, as when a project is opened from the menu.
            try{if(!pendingImports(workspace_).empty()||std::filesystem::exists(workspace_/"pending")){if(!config_.lease)config_.lease=acquireLabelLease(config_.project);rollbackImports(workspace_);}}
            catch(const std::exception& error){recovery=error.what();}
        }
        connect();if(!recovery.empty())error_=recovery;
    }
    if(config_.rememberUi){try{wchar_t location[32768]{};GetEnvironmentVariableW(L"LOCALAPPDATA",location,32768);std::ifstream input(std::filesystem::path(location)/"Boccia/labeler-projects.json",std::ios::binary);const auto recent=Json::parse(input);if(recent.is_array())recentProjects_=recent;}catch(...){}}
    if(config_.rememberUi){loadDevice();if(!settings_.worker.empty())startHub();else openWorkerDialog();}   // the hidden checks keep the CPU and join no hub
    if(startScreen&&std::filesystem::is_regular_file(legacyProject)){const auto path=utf8(legacyProject);bool found=false;for(const auto& item:recentProjects_)found|=item.value("path","")==path;if(!found)recentProjects_.push_back({{"path",path},{"name","기존 라벨 작업"},{"legacy",true}});}
}
Application::~Application() {
    logLine("종료");
    setLogSink(nullptr);hub_.reset();   // the hub client stops first: its threads touch nothing of the app after this
    if(task_.valid()) task_.wait(); if(thumbTask_.valid()) thumbTask_.wait(); if(ahead_.valid()) ahead_.wait();
    if(config_.rememberUi){try{rememberPosition();}catch(...){}try{if(projectOpened())rememberProject(workspace_.empty()?config_.project:workspace_);}catch(...){}} releaseFrames();deleteDoomed();
    for(auto& [key,id]:thumbs_)if(id)glDeleteTextures(1,&id);
}
std::filesystem::path Application::preferencesPath() const { return config_.project.parent_path()/(config_.project.stem().wstring()+L"-native-ui.json"); }
void Application::rememberPosition() const {
    if(frames_.empty())return;
    const auto& scene=catalog_["scenes"][scene_];
    Json state={{"recording",scene["recording"]},{"index",scene["index"]},{"camera",frames_[camera_].member["camera"]},{"editor",editor_},{"page",int(page_)},{"brightness",brightness_},{"export",exportSettings()},{"fit",fitCircle_},{"representatives",representatives_},{"filter",filter_},{"masks",masks_},{"boxes",boxes_},{"circles",circles_},{"outlines",outlines_},{"view",view_},{"fit_circle",fitCircle_},{"panels",{listOpen_,sideOpen_,summaryOpen_}}};
    auto path=preferencesPath();std::filesystem::create_directories(path.parent_path());
    auto temporary=path;temporary+=L"."+std::to_wstring(Clock::now().time_since_epoch().count())+L".tmp";std::ofstream stream(temporary,std::ios::binary);stream<<state.dump(2);stream.close();
    if(!stream)throw std::runtime_error("작업 위치 저장 실패");
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("작업 위치 반영 실패");
}
void Application::restorePosition() {
    int index=0,camera=0;bool editor=false;
    if(config_.rememberUi){try {
        std::ifstream input(preferencesPath(),std::ios::binary);auto state=Json::parse(input);
        const int savedPage=state.value("page",int(page_));if(!config_.training&&(savedPage==0||savedPage==1))page_=Page(savedPage);
        brightness_=std::clamp(state.value("brightness",1.65f),.5f,3.f);restoreExportSettings(state.value("export",Json()));fitCircle_=state.value("fit",true);representatives_=state.value("representatives",true);filter_=std::clamp(state.value("filter",0),0,3);masks_=state.value("masks",true);boxes_=state.value("boxes",true);circles_=state.value("circles",true);view_=std::clamp(state.value("view",0),0,viewCount-1);outlines_=state.value("outlines",true);fitCircle_=state.value("fit_circle",true);
        if(state.contains("panels")&&state["panels"].is_array()&&state["panels"].size()==3){listOpen_=state["panels"][0];sideOpen_=state["panels"][1];summaryOpen_=state["panels"][2];}
        for(int i=0;i<int(catalog_["scenes"].size());++i){auto& scene=catalog_["scenes"][i];if(scene["recording"]==state.at("recording")&&scene["index"]==state.at("index")){
            index=i;editor=state.value("editor",false);for(int c=0;c<int(scene["members"].size());++c)if(scene["members"][c]["camera"]==state.at("camera"))camera=c;break;
        }}
    }catch(...) {}}
    if(!catalog_["scenes"].empty())loadScene(index,editor,camera);
}
bool Application::idle() const {return settled()&&std::none_of(frames_.begin(),frames_.end(),[](const FrameView& frame){return frame.dirty;});}
bool Application::canClose() const {return !busy()&&!pending()&&!exporting()&&std::none_of(frames_.begin(),frames_.end(),[](const FrameView& frame){return frame.dirty;});}
void Application::preserveRecovery(){
    const auto directory=config_.project.parent_path()/"recovery";std::filesystem::create_directories(directory);   // beside the labels it belongs to
    for(auto& frame:frames_)if(frame.dirty||(&frame==&current()&&pending())){
        auto file=directory/("pending-"+std::to_string(Clock::now().time_since_epoch().count())+".json");
        Json snapshot={{"frame",frame.annotation},{"proposal",proposal_},{"polygon",Json::array()},{"points",points_},{"project",utf8(config_.project)}};
        for(auto point:polygon_)snapshot["polygon"].push_back({point.x,point.y});
        std::ofstream stream(file,std::ios::binary);stream<<snapshot.dump(2);stream.flush();if(!stream)throw std::runtime_error("변경 보관 파일을 저장하지 못했습니다.");
    }
}
FrameView& Application::current() { return frames_.at(camera_); }
void Application::launch(std::string label, std::function<JobResult()> work, std::function<void(JobResult)> completed) {
    if (busy()) return;
    busyLabel_=std::move(label); completion_=std::move(completed); error_.clear();
    task_=std::async(std::launch::async,std::move(work));
}
// The engine lives in this process: the recordings are scanned and the labels opened on a worker thread, so the
// window appears at once and an error (another source, a changed recording folder) shows in the usual place.
void Application::connect(std::string note) {
    const auto config=config_;
    launch("로컬 데이터 연결", [config] {
        auto engine=std::make_shared<Engine>(EnginePaths{config.source,config.project,config.exports,config.models,config.root});
        JobResult result{{{"catalog",engine->nativeCatalog(false)},{"auto",engine->autoStatus()},{"assist",engine->assistStatus()}}, {}};
        result.engine=std::move(engine);
        return result;
    }, [this,note](JobResult result){
        engine_=std::move(result.engine); updateCatalog(result.data.at("catalog")); autoStatus_=result.data.at("auto"); message_=note.empty()?"자료와 라벨을 열었습니다.":note;
        circleMargin_=float(catalog_.value("circle_margin",.5));
        restorePosition();
    });
}
void Application::updateCatalog(Json value) {
    catalog_=std::move(value);rowsDirty_=true;aheadDirty_=true;targetScenes_=-1;
    for(const auto& recording:catalog_["recordings"]) {
        auto id=recording.at("id").get<std::string>();
        if(!groups_.contains(id))groups_[id]=id;
        if(!splits_.contains(id))splits_[id]="unassigned";
        // Until the plan is edited on the split page it follows what the engine has stored.
        if(!planEdited_){splits_[id]=catalog_["summary"]["splits"].value(id,"unassigned");if(catalog_["groups"].contains(id))groups_[id]=catalog_["groups"][id];}
    }
}
void Application::updateSummary(const Json& summary) {
    catalog_["summary"]=summary; catalog_["split_confirmed"]=false;rowsDirty_=true;aheadDirty_=true;targetScenes_=-1;
    int target=0,done=0;
    for(auto& scene:catalog_["scenes"])for(auto& member:scene["members"]) {
        const auto key=member.at("key").get<std::string>();
        if(summary["frames"].contains(key)) {const auto& item=summary["frames"][key];member["selected"]=item.value("selected",true);member["reviewed"]=item.value("reviewed",false);member["status"]=item.value("review_status","draft");member["count"]=item.value("count",0);member["attention"]=item.value("needs_attention",false);}
        if(member.value("selected",true)){++target;done+=member.value("reviewed",false);}
    }
    catalog_["target_images"]=target;catalog_["reviewed_target_images"]=done;
    for(auto& frame:frames_) {
        auto key=frame.annotation.value("key","");
        if(summary["frames"].contains(key)) {const auto& state=summary["frames"][key];frame.member["selected"]=state.value("selected",true);frame.member["reviewed"]=state.value("reviewed",false);frame.member["status"]=state.value("review_status","draft");}
    }
}
// The textures are not deleted here: a scene change from a key or button runs after this frame's draw list was built, and
// that list still shows the old frames. Deleting at once left one frame sampling a freed id (the font atlas, as it happened).
void Application::releaseFrames(){for(auto& frame:frames_){if(frame.texture)doomed_.push_back(frame.texture);if(frame.overlay)doomed_.push_back(frame.overlay);}frames_.clear();}
void Application::deleteDoomed(){if(!doomed_.empty()){glDeleteTextures(GLsizei(doomed_.size()),doomed_.data());doomed_.clear();}}
namespace {
// The cameras of one scene are read at the same time, each on its own thread.
JobResult fetchScene(const std::shared_ptr<Engine>& engine,const Json& scene) {
    const auto recording=scene.at("recording").get<std::string>();
    std::vector<std::future<FrameView>> cameras;
    for(const auto& member:scene.at("members"))cameras.push_back(std::async(std::launch::async,[engine,member,recording]{
        const auto camera=member.at("camera").get<std::string>();const int index=member.at("index").get<int>();
        FrameView frame;frame.member=member;frame.annotation=engine->frame(recording,camera,index);frame.image=engine->image(recording,camera,index);return frame;}));
    JobResult result;result.data=Json::object();for(auto& camera:cameras)result.frames.push_back(camera.get());
    return result;
}
}
void Application::loadScene(int index,bool editor,int camera,std::string note) {
    if(index<0||index>=int(catalog_["scenes"].size())||busy())return;
    // Never replace a frame that still has unsaved edits: save it first and come back.
    if(!frames_.empty()&&dirtySlot()>=0){if(pending())message_="미확정 윤곽을 반영하거나 취소하세요.";else save([this,index,editor,camera,note]{loadScene(index,editor,camera,note);},dirtySlot());return;}
    const auto token=++generation_;
    auto show=[this,index,editor,camera,token,note](JobResult result){
        if(token!=generation_)return;
        // The catalog may have moved on since the images were requested, above all for a scene fetched ahead.
        const auto& members=catalog_["scenes"][index]["members"];
        for(size_t i=0;i<result.frames.size()&&i<members.size();++i){result.frames[i].member=members[i];result.frames[i].annotation["review_status"]=members[i].value("status","draft");}
        if(backing_)backing_=false;else if(!frames_.empty()&&scene_!=index){history_.push_back(scene_);if(history_.size()>50)history_.erase(history_.begin());}
        releaseFrames();frames_=std::move(result.frames);scene_=index;camera_=std::clamp(camera,0,int(frames_.size())-1);selected_=-1;editor_=editor||(frames_.size()==1&&frames_[0].member.value("camera","")=="images");adding_=false;stale_=false;aheadDirty_=true;scrollToCurrent_=true;cancel();
        message_=note.empty()?"원본과 저장 라벨 표시 중":note;};
    // A scene fetched ahead is used only if no automatic draft has been written since.
    if(index==aheadScene_&&ahead_.valid()&&aheadAuto_==catalog_["summary"].value("auto_revision",0)){
        const auto fetched=ahead_.share();aheadScene_=-1;++aheadHits_;
        if(fetched.wait_for(std::chrono::seconds(0))!=std::future_status::ready){launch("카메라 이미지 불러오기",[fetched]{return fetched.get();},show);return;}
        try{show(fetched.get());return;}catch(...){}   // fetch again below, so a failure is reported the usual way
    }
    const auto scene=catalog_["scenes"][index];const auto engine=engine_;
    launch("카메라 이미지 불러오기",[engine,scene]{return fetchScene(engine,scene);},show);
}
// Fetches the next unreviewed scene in the background, so confirming a scene does not wait for the images.
// Hidden check of 이전 라벨 가져오기 (2026-10-20: a button in place of the on/off): pressed on the first draft it carries
// the labels of the nearest earlier confirmed frame as unsaved edits, or says the draft already agrees; that scene
// confirmed and the next one opened, F1 pressed there while a save runs carries the labels just confirmed once the save
// is done, and 되돌리기 takes them back.
bool Application::applyPreviousCheckStep() {
    static int first = -1, phase = 0, waited = 0;
    static bool keyDown = false;
    static std::vector<Json> before;
    auto& io = ImGui::GetIO();
    if (keyDown) { io.AddKeyEvent(ImGuiKey_F1, false); keyDown = false; }   // F1 is let go the frame after it went down
    if (busy()) return true;
    const auto source = [this] {   // the frame the bottom bar names, "이전 라벨 · 프레임 0030", or -1
        const std::string head = "이전 라벨 · 프레임 ";
        return message_.rfind(head, 0) == 0 ? std::atoi(message_.c_str() + head.size()) : -1;
    };
    const auto edited = [this] { int count = 0; for (const auto& frame : frames_) count += frame.dirty; return count; };
    switch (phase) {
    case 0:
        first = nextUnreviewedIndex();
        if (first < 0) throw std::runtime_error("apply-previous check: no draft scene");
        loadScene(first); phase = 1; return true;
    case 1:
        if (scene_ != first) { if (++waited > 600) throw std::runtime_error("apply-previous check: the draft did not open"); return true; }
        applyPreviousLabels(); phase = 2; return true;
    case 2:
        if ((edited() == 0 || source() < 0) && message_ != "이전 라벨과 같음") throw std::runtime_error("apply-previous check: the first draft got " + message_);
        review(Scope::Scene, false); phase = 3; return true;
    case 3:   // confirmed: the prompt, where Space moves on
        if (!nextAsk_) return true;
        nextAsk_ = false; advance(); phase = 4; waited = 0; return true;
    case 4:
        if (scene_ == first) { if (++waited > 600) throw std::runtime_error("apply-previous check: did not move on"); return true; }
        before.clear(); for (const auto& frame : frames_) before.push_back(frame.annotation["boxes"]);
        changed(); save({}, dirtySlot());   // F1 (the user's key, 2026-10-20) while a save runs: it carries once the save is done
        if (!busy()) throw std::runtime_error("apply-previous check: the save did not start");
        io.AddKeyEvent(ImGuiKey_F1, true); keyDown = true; phase = 5; return true;
    case 5: {
        const int expected = catalog_["scenes"][first].value("index", -1);
        if (source() != expected) throw std::runtime_error("apply-previous check: the next scene got " + message_ + ", not the labels of the scene just confirmed (" + std::to_string(expected) + ")");
        const int slot = dirtySlot();
        if (slot < 0) throw std::runtime_error("apply-previous check: no frame of the next scene changed");
        camera_ = slot; undo();
        if (frames_[size_t(slot)].annotation["boxes"] != before[size_t(slot)]) throw std::runtime_error("apply-previous check: 되돌리기 did not take the carried labels back");
        return false;
    }
    default: return false;
    }
}
void Application::prefetch() {
    if(!aheadDirty_||!ready()||busy()||page_!=Page::Review)return;
    const int next=advanceIndex(),revision=catalog_["summary"].value("auto_revision",0);
    if(ahead_.valid()){
        if(aheadScene_==next&&aheadAuto_==revision){aheadDirty_=false;return;}
        if(ahead_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;   // dropping a running fetch would block until it ends
        try{ahead_.get();}catch(...){}
    }
    aheadDirty_=false;aheadScene_=-1;
    if(next<0||next==scene_)return;
    const auto engine=engine_;const auto scene=catalog_["scenes"][next];
    aheadScene_=next;aheadAuto_=revision;
    ahead_=std::async(std::launch::async,[engine,scene]{return fetchScene(engine,scene);});
}
bool Application::pending() const { return !proposal_.is_null()||!polygon_.empty()||!points_.empty()||dragging_; }
bool Application::guard(){if(busy()){message_="처리가 끝난 뒤 이동하세요. 화면 확대·이동은 계속할 수 있습니다.";return false;}if(pending()){message_="미확정 윤곽을 반영하거나 취소하세요.";return false;}return true;}
void Application::navigate(int index){if(!guard())return;if(!frames_.empty()&&current().dirty){save([this,index]{loadScene(index);});return;}loadScene(index);}
void Application::showEditor(int camera){focus(camera);}
void Application::switchCamera(int camera){focus(camera);}
void Application::openPage(Page page){
    if(!projectOpened()){page_=Page::Data;return;}
    if(config_.training&&(page==Page::Data||page==Page::Review||page==Page::Export))return;
    if(!config_.training&&(page==Page::Split||page==Page::TrainExport||page==Page::Train))return;
    if(!guard())return;
    if(!frames_.empty()&&current().dirty){save([this,page]{openPage(page);});return;}
    page_=page;
    if(!engine_)return;
    if(page==Page::Split||page==Page::TrainExport||page==Page::Train)refreshStage();else if(stale_&&page==Page::Review)loadScene(scene_,editor_,camera_);
}
void Application::showPage(int page){openPage(Page(std::clamp(page,0,5)));}
// Enlarge one camera in place without selecting a ball.
void Application::focus(int camera) {
    if(frames_.empty()||camera<0||camera>=int(frames_.size())||!guard())return;
    if(current().dirty&&camera!=camera_){save([this,camera]{focus(camera);});return;}
    current().repaint=true;camera_=camera;editor_=true;selected_=-1;current().scale=0;current().repaint=true;cancel();
}
// The large view of a camera keeps the zoom and position last used there: the next ball is usually near the last one,
// so "공 추가" opens the view as it was left instead of fitting the whole image (2026-10-09 user request).
void Application::rememberView(const FrameView& frame){
    if(!editor_||camera_<0||frame.scale<=0)return;
    if(int(lastView_.size())<=camera_)lastView_.resize(size_t(camera_)+1);
    lastView_[size_t(camera_)]={frame.scale,frame.offsetX,frame.offsetY};
}
void Application::addBallHere(int slot){
    focus(slot);
    if(!editor_||camera_!=slot)return;
    tool_=Tool::Ai;adding_=true;
    if(slot<int(lastView_.size())&&lastView_[size_t(slot)].scale>0){auto& f=frames_[size_t(slot)];f.scale=lastView_[size_t(slot)].scale;f.offsetX=lastView_[size_t(slot)].offsetX;f.offsetY=lastView_[size_t(slot)].offsetY;}
}
// Enlarge the camera, select the ball and zoom to it.
void Application::select(int camera,int ball) {
    if(frames_.empty()||camera<0||camera>=int(frames_.size())||!guard())return;
    adding_=false;
    if(current().dirty&&camera!=camera_){save([this,camera,ball]{select(camera,ball);});return;}
    if(camera!=camera_){current().repaint=true;cancel();}
    camera_=camera;editor_=true;selected_=ball<int(current().annotation["boxes"].size())?ball:-1;current().scale=0;current().repaint=true;
}
void Application::cycle(int step) {
    if(frames_.empty()||!guard())return;
    std::vector<Json> annotations;for(auto& frame:frames_)annotations.push_back(frame.annotation);
    const auto next=nextBall(ballOrder(annotations),editor_?BallRef{camera_,selected_}:BallRef{},step);
    if(next.camera>=0)select(next.camera,next.index);
}
// edge: 0 top, 1 bottom, 2 left, 3 right. The mask is never touched.
void Application::nudge(int edge,bool expand) {
    if(frames_.empty()||!editor_||selected_<0||pending()||busy())return;
    auto& frame=current();auto& box=frame.annotation["boxes"][selected_];auto trial=box;
    if(!nudgeEdge(trial,edge,expand,frame.image.cols,frame.image.rows)){message_=expand?"이미지 밖으로 넓힐 수 없습니다.":"박스는 1픽셀보다 작아질 수 없습니다.";return;}
    // The edited box defines the ball: the circle inscribed in it, and the mask as that disc (2026-10-08 user request).
    // A partly hidden ball's box is its visible part, so no circle fits in it; its mask stays and an old circle goes.
    if(trial.value("occluded",false))trial.erase("circle");else inscribeCircle(trial,frame.image.cols,frame.image.rows);
    // A correction that ends up equal to the mask bounds is no correction.
    if(trial.at("detection_bbox")==trial.at("bbox"))trial.erase("detection_bbox");
    checkpoint();box=trial;boxes_=true;changed(masks_||outlines_);   // the overlay is rebuilt only when the mask is shown: doing it always made every key press lag
}
void Application::checkpoint(){checkpoint(current());}
void Application::checkpoint(FrameView& f){f.history.push_back(f.annotation);if(f.history.size()>40)f.history.erase(f.history.begin());}
void Application::changed(bool repaint){edited(current(),repaint);message_="수정됨 · 자동 저장 대기";}
void Application::edited(FrameView& f,bool repaint){f.dirty=true;if(repaint)f.repaint=true;f.annotation["reviewed"]=false;f.annotation["review_status"]="editing";f.member["reviewed"]=false;f.member["status"]="editing";catalog_["split_confirmed"]=false;rowsDirty_=true;changed_=Clock::now();}
int Application::dirtySlot() const {
    if(!frames_.empty()&&camera_>=0&&camera_<int(frames_.size())&&frames_[size_t(camera_)].dirty)return camera_;
    for(int slot=0;slot<int(frames_.size());++slot)if(frames_[size_t(slot)].dirty)return slot;
    return -1;
}
void Application::save(std::function<void()> next,int slot) {
    if(frames_.empty()||pending()||busy())return;
    if(slot<0)slot=camera_; const auto& frame=frames_.at(size_t(slot)); const auto key=frame.annotation.at("key").get<std::string>();
    const auto payload=annotationPayload(frame.annotation,catalog_["summary"].at("revision").get<int>()); const auto engine=engine_;
    launch("라벨 저장",[engine,payload]{return JobResult{engine->saveAnnotation(payload),{}};},[this,slot,key,next](JobResult result){updateSummary(result.data.at("summary"));if(slot<int(frames_.size())&&frames_[slot].annotation.value("key","")==key)frames_[slot].dirty=false;message_="자동 저장 완료";if(next)next();});
}
void Application::cancel(){++generation_;proposal_=nullptr;brushMask_.release();polygon_.clear();points_=Json::array();replacement_=-1;dragging_=false;if(!frames_.empty())current().repaint=true;}
// A ball added during review is measured like the others: Canny + RANSAC circle, box from the circle (2026-10-08 user
// request). A box drawn without a mask starts from the disc inscribed in it, which stays when the fit fails.
// The proposal as it will be applied: with the circle fit off, the outline as the model or the person gave it, no circle.
Json Application::chosenProposal() const {
    Json box=proposal_;if(box.is_null())return box;
    if(!fitCircle_&&box.contains("raw")){const Json raw=box["raw"];box["bbox"]=raw["bbox"];box["segmentation"]=raw["segmentation"];if(raw.contains("area")&&!raw["area"].is_null())box["area"]=raw["area"];box.erase("circle");box.erase("detection_bbox");box.erase("contours");}
    box.erase("raw");return box;
}
bool Application::measure(Json& box){
    const auto& image=current().image;
    if(!box.contains("segmentation"))inscribeCircle(box,image.cols,image.rows);
    else if(!fitCircle_)return box.contains("circle");   // the fit is off: the outline stays as segmented (2026-10-08 user request)
    if(box.contains("circle")&&box.value("circle_source","")!="box")return true;   // measured already, by the engine
    if(!refineBallBox(box,image,circleMargin_))return false;
    box.erase("circle_source");return true;
}
void Application::apply() {
    if(busy())return;
    if(proposal_.is_null()){if(polygon_.size()>=3)polygonProposal();return;}
    try {
        auto box=chosenProposal();
        if(!brushMask_.empty())box=maskAnnotation(brushMask_,box,"manual_brush");
        const bool measured=measure(box);
        char note[112];
        if(!fitCircle_&&!measured)std::snprintf(note,sizeof note,"원 맞춤 없이 분할 윤곽 그대로 반영했습니다");
        else if(measured)std::snprintf(note,sizeof note,"박스는 Canny+RANSAC 원(r %.1f)에서 잡았습니다",box["circle"][2].get<double>());
        else std::snprintf(note,sizeof note,"원 맞춤이 안 되어 %s로 박스를 잡았습니다",box.contains("circle")?"내접원":"마스크 경계");
        checkpoint();auto& existing=current().annotation["boxes"];
        const bool added=!(replacement_>=0&&replacement_<int(existing.size()));
        if(!added) {
            box["occluded"]=existing[replacement_].value("occluded",false);
            if(existing[replacement_].contains("color"))box["color"]=existing[replacement_]["color"];
            existing[replacement_]=box;selected_=replacement_;
        } else {existing.push_back(box);selected_=int(existing.size())-1;}
        logLine((added?"공 추가 · ":"공 수정 · ")+cameraLabel(current().member)+(added?std::string():" · 공 "+std::to_string(selected_+1))+" · "+(!brushMask_.empty()?"브러시":tool_==Tool::Polygon?"폴리곤":tool_==Tool::Box?"박스":"클릭 분할"));
        cancel();changed();message_=std::string("윤곽 반영 · ")+note;
        // A found ball, once applied, takes the user back to the three-tile view (2026-10-08 user request).
        adding_=false;
        if(added){editor_=false;selected_=-1;message_=std::string("새 공 반영 · ")+note+" · 세 장 보기로 돌아왔습니다.";}
    } catch(const std::exception& error){error_=error.what();}
}
void Application::removeSelected(){if(editor_)removeBall(camera_,selected_);}
// Deletes a ball of any camera of the scene. Edits belong to the current camera, so that camera becomes current first.
void Application::removeBall(int slot,int ball){
    if(frames_.empty()||slot<0||slot>=int(frames_.size())||pending()||busy())return;
    if(slot!=camera_&&current().dirty){save([this,slot,ball]{removeBall(slot,ball);});return;}
    camera_=slot;auto& objects=current().annotation["boxes"];if(ball<0||ball>=int(objects.size()))return;
    checkpoint();objects.erase(objects.begin()+ball);selected_=-1;current().scale=0;current().repaint=true;changed();
    logLine("공 삭제 · "+cameraLabel(current().member)+" · 공 "+std::to_string(ball+1));
    message_="공을 지웠습니다 · Ctrl+Z로 되돌릴 수 있습니다";   // no dialog: deletion is immediate (2026-10-08 user request)
}
// Marks a ball of any camera of the scene as partly hidden, or not, from the checkbox on its crop (2026-10-08 user
// request: one click instead of selecting the ball and pressing O). Edits belong to the current camera, as for a deletion.
void Application::setOccluded(int slot,int ball,bool value){
    if(frames_.empty()||slot<0||slot>=int(frames_.size())||pending()||busy())return;
    if(slot!=camera_&&current().dirty){save([this,slot,ball,value]{setOccluded(slot,ball,value);});return;}
    camera_=slot;auto& objects=current().annotation["boxes"];if(ball<0||ball>=int(objects.size()))return;
    if(objects[ball].value("occluded",false)==value)return;
    checkpoint();setOccludedFlag(objects[ball],value);changed(false);message_=value?"일부 가림으로 표시했습니다":"일부 가림 표시를 지웠습니다";
}
// After Space confirmed the scene: the prompt in the middle, where a second Space moves on and Esc stays.
void Application::confirmNext(){
    if(!nextAsk_)return;
    if(!ImGui::IsPopupOpen("##next-scene"))ImGui::OpenPopup("##next-scene");
    const auto viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x+viewport->WorkSize.x/2,viewport->WorkPos.y+viewport->WorkSize.y/2},ImGuiCond_Appearing,{.5f,.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{26,22});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8,10});
    if(ImGui::BeginPopupModal("##next-scene",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoTitleBar)){
        char title[48];std::snprintf(title,sizeof title,"프레임 %04d 확인 완료",frames_.empty()?0:catalog_["scenes"][scene_].value("index",0));
        ImGui::PushFont(theme::bold,theme::px(theme::title));ImGui::TextUnformatted(title);ImGui::PopFont();
        const int next=advanceIndex();
        if(next<0||next==scene_)ImGui::TextUnformatted("더 넘어갈 장면이 없습니다.");
        else{
            bool reviewed=true;for(const auto& member:catalog_["scenes"][next]["members"])if(member.value("selected",true)&&!member.value("reviewed",false))reviewed=false;
            char line[96];std::snprintf(line,sizeof line,"다음 장면은 프레임 %04d입니다.%s",catalog_["scenes"][next].value("index",0),reviewed?"  (이미 확인한 장면)":"");ImGui::TextUnformatted(line);}
        ImGui::Dummy({0,4});
        bool close=false;
        ImGui::PushFont(theme::bold,theme::px(theme::base));
        if(theme::primaryButton("다음 장면  Space",{160,36})||ImGui::IsKeyPressed(ImGuiKey_Space,false)||ImGui::IsKeyPressed(ImGuiKey_Enter,false)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter,false)){later_=[this]{advance();};close=true;}
        ImGui::PopFont();ImGui::SameLine();
        if(ImGui::Button("머물기  Esc",{110,36})||ImGui::IsKeyPressed(ImGuiKey_Escape,false))close=true;
        if(close){nextAsk_=false;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}
// The missing-ball pass (2026-10-08 user request): the detector's balls the draft lacks, with options, a count, a trial
// on a few frames whose new balls are shown as crops, and the full apply. Only images nobody has touched are changed.
void Application::startMissing(bool dryRun,bool countOnly,int limit){
    if(busy())return;
    const auto engine=engine_;
    Json payload={{"cameras",missingCameras_==0?"colour":"all"},{"coverage",double(missingCoverage_)},{"min_radius",double(missingMinRadius_)},{"max_radius",double(missingMaxRadius_)},
                        {"margin",double(circleMargin_)},{"dry_run",dryRun},{"count_only",countOnly},{"limit_frames",limit>=0?limit:dryRun?missingTrial_:0},{"sample_limit",8}};
    payload.update(batchPayload_);
    launch(countOnly?"누락 공 후보 세기":dryRun?"누락 공 시험 실행":"누락 공 채우기",[engine,payload]{return JobResult{engine->nativeAction("fill-missing",payload),{}};},
           [this,countOnly](JobResult result){missingStatus_=result.data;if(countOnly)message_="후보 "+grouped(missingStatus_.value("candidates",0))+"개 · "+grouped(missingStatus_.value("frames_with_candidates",0))+"장";else message_="누락 공 작업을 시작했습니다. 그동안 계속 작업할 수 있습니다.";});
}
// Each sample: the image around one new ball, the box and circle moved into the crop, shown by the usual tile painter.
void Application::loadMissingSamples(Json samples){
    if(samples.is_null())samples=missingStatus_.value("samples",Json::array());
    if(samples.empty()||busy()){for(auto& s:missingSamples_){if(s.texture)doomed_.push_back(s.texture);if(s.overlay)doomed_.push_back(s.overlay);}missingSamples_.clear();return;}
    const auto engine=engine_;
    launch("샘플 조각 읽기",[engine,samples]{
        JobResult result;
        for(const auto& sample:samples){
            const std::string key=sample.value("key","");const auto a=key.find('/'),b=key.rfind('/');if(a==std::string::npos||b==a)continue;
            const std::string recording=key.substr(0,a),camera=key.substr(a+1,b-a-1);const int index=std::stoi(key.substr(b+1));
            const cv::Mat image=engine->image(recording,camera,index);if(image.empty())continue;
            Json box=sample["box"];const auto r=detectionBox(box);const auto region=cropRegion(r,image.cols,image.rows,2.6);
            const int x0=std::max(0,int(std::floor(region.x))),y0=std::max(0,int(std::floor(region.y)));
            const int x1=std::min(image.cols,int(std::ceil(region.x+region.width))),y1=std::min(image.rows,int(std::ceil(region.y+region.height)));
            if(x1-x0<4||y1-y0<4)continue;
            auto shift=[&](const char* name){if(box.contains(name)){box[name][0]=box[name][0].get<double>()-x0;box[name][1]=box[name][1].get<double>()-y0;}};
            shift("bbox");shift("detection_bbox");shift("circle");box.erase("segmentation");box.erase("contours");
            FrameView view;view.image=image(cv::Rect(x0,y0,x1-x0,y1-y0)).clone();
            view.annotation={{"key",key},{"boxes",Json::array({box})},{"index",index}};
            view.member={{"index",index},{"kind",box.contains("color")?"컬러":"모노"},{"count",sample.value("count",1)},{"camera",camera},{"note",sample.value("note","")}};
            result.frames.push_back(std::move(view));
        }
        return result;},
        [this](JobResult result){for(auto& s:missingSamples_){if(s.texture)doomed_.push_back(s.texture);if(s.overlay)doomed_.push_back(s.overlay);}missingSamples_=std::move(result.frames);});
}
void Application::refreshMissingTrials(){
    if(busy())return;const auto engine=engine_;
    launch("시험 목록 읽기",[engine]{return JobResult{engine->nativeAction("missing-trials",Json::object()),{}};},[this](JobResult result){missingTrials_=result.data.value("trials",Json::array());
        const int n=int(missingTrials_.size());combineA_=std::clamp(combineA_,0,std::max(0,n-1));combineB_=std::clamp(combineB_,0,std::max(0,n-1));});
}
void Application::missingAction(const std::string& action,Json payload,std::string done){
    if(busy())return;const auto engine=engine_;
    launch(done,[engine,action,payload]{return JobResult{engine->nativeAction(action,payload),{}};},[this,action,done](JobResult result){
        if(result.data.contains("samples")){loadMissingSamples(result.data["samples"]);message_=done+" · 공 "+grouped(int(result.data["samples"].size()))+"개 조각";return;}
        if(action=="apply-missing-trial"){message_=done+" · "+grouped(result.data.value("applied_frames",0))+"장에 공 "+grouped(result.data.value("applied_balls",0))+"개 추가, 건너뜀 "+grouped(result.data.value("skipped",0))+"장";refreshStage();return;}
        missingTrials_=result.data.value("trials",Json::array());message_=done;
        const int n=int(missingTrials_.size());combineA_=std::clamp(combineA_,0,std::max(0,n-1));combineB_=std::clamp(combineB_,0,std::max(0,n-1));
        // The next trial gets the next letter, so A, B, C come without typing.
        if(action=="save-missing-trial"&&trialName_[0]&&!trialName_[1]&&trialName_[0]>='A'&&trialName_[0]<'Z')++trialName_[0];});
}
// The hidden run's check of the trial path: two trials on the same frames, their assembly and the apply of one.
bool Application::missingScriptStep(int step){
    auto expect=[](bool condition,const char* what){if(!condition)throw std::runtime_error(std::string("missing-script: ")+what);};
    auto trial=[&](const char* name)->Json{for(const auto& t:missingTrials_)if(t.value("name","")==name)return t;return Json();};
    switch(step){
    case 0:missingTrial_=6;missingCoverage_=.8f;startMissing(true,false);return true;
    case 1:{expect(missingStatus_.value("status","")=="complete"&&missingStatus_.value("dry_run",false),"the first trial did not complete");
        engine_->nativeAction("save-missing-trial",{{"name","A"}});missingCoverage_=.65f;startMissing(true,false);return true;}
    case 2:{expect(missingStatus_.value("status","")=="complete","the second trial did not complete");
        engine_->nativeAction("save-missing-trial",{{"name","B"}});
        engine_->nativeAction("combine-missing-trials",{{"name","A or B"},{"op","or"},{"a","A"},{"b","B"}});
        engine_->nativeAction("combine-missing-trials",{{"name","A and B"},{"op","and"},{"a","A"},{"b","B"}});
        engine_->nativeAction("combine-missing-trials",{{"name","B not A"},{"op","and_not"},{"a","B"},{"b","A"}});
        missingTrials_=engine_->nativeAction("missing-trials",Json::object()).value("trials",Json::array());
        const Json a=trial("A"),b=trial("B"),both=trial("A and B"),either=trial("A or B"),rest=trial("B not A");
        expect(!a.is_null()&&!b.is_null()&&!both.is_null()&&!either.is_null()&&!rest.is_null(),"a trial is missing from the list");
        expect(a.value("balls",0)>0&&b.value("balls",0)>=a.value("balls",0),"a looser coverage must find at least as many balls on the same frames");
        expect(both.value("balls",0)==a.value("balls",0)&&either.value("balls",0)==b.value("balls",0)&&rest.value("balls",0)==b.value("balls",0)-a.value("balls",0),"the assembly does not follow set algebra");
        const Json applied=engine_->nativeAction("apply-missing-trial",{{"name","A and B"}});
        expect(applied.value("applied_balls",0)==a.value("balls",0)&&applied.value("skipped",0)==0,"applying the assembled trial did not add its balls");
        const Json again=engine_->nativeAction("apply-missing-trial",{{"name","A and B"}});
        expect(again.value("applied_balls",0)==0,"applying twice must add nothing, since the drafts changed");
        engine_->nativeAction("drop-missing-trial",{{"name","B not A"}});
        missingTrials_=engine_->nativeAction("missing-trials",Json::object()).value("trials",Json::array());
        expect(trial("B not A").is_null()&&missingTrials_.size()==4,"dropping a trial failed");
        missingAsk_=true;refreshStage();return false;}
    }
    return false;
}
// The sample crops of a trial: four to a row, each with its camera, frame and either the trial's note or the measured circle.
void Application::sampleGrid(const char* heading){
    ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"%s",heading);ImGui::PopFont();
    auto draw=ImGui::GetWindowDrawList();const float side=132,gap=8;const ImVec2 start=ImGui::GetCursorScreenPos();
    const int columns=4;const int rows=(int(missingSamples_.size())+columns-1)/columns;
    ImGui::Dummy({columns*(side+gap)-gap,rows*(side+gap+18)-gap});
    for(size_t i=0;i<missingSamples_.size();++i){
        auto& sample=missingSamples_[i];if(sample.repaint)repaint(sample);
        const ImVec2 p0{start.x+float(i%columns)*(side+gap),start.y+float(i/columns)*(side+gap+18)},p1{p0.x+side,p0.y+side};
        paintImage(sample,p0,p1,{0,0,double(sample.image.cols),double(sample.image.rows)},{.fill=false,.boxes=true,.circles=true,.outlines=false,.selected=-1,.skip=-1});
        draw->AddRect(p0,p1,theme::line,4);
        const auto& box=sample.annotation["boxes"][0];char caption[112];
        const std::string camera=sample.member.value("camera",""),note=sample.member.value("note","");
        const char* cam=camera.empty()?"?":camera.substr(camera.size()-1).c_str();
        if(!note.empty())std::snprintf(caption,sizeof caption,"%s·%04d %s",cam,sample.member.value("index",0),note.c_str());
        else std::snprintf(caption,sizeof caption,"%s·%04d r%.1f %s",cam,sample.member.value("index",0),box.contains("circle")?box["circle"][2].get<double>():0.0,box.value("source","")=="auto_sam"?"윤곽":"상자");
        theme::write(draw,{p0.x,p1.y+3},theme::text2,caption,11,theme::mono);
    }
}
void Application::confirmMissing(){
    if(!missingAsk_)return;
    if(!ImGui::IsPopupOpen("##fill-missing"))ImGui::OpenPopup("##fill-missing");
    const auto viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x+viewport->WorkSize.x/2,viewport->WorkPos.y+viewport->WorkSize.y/2},ImGuiCond_Appearing,{.5f,.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{22,20});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8,10});
    if(ImGui::BeginPopupModal("##fill-missing",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoTitleBar)){
        const std::string status=missingStatus_.value("status","idle");const bool running=status=="running";
        ImGui::PushFont(theme::bold,theme::px(theme::title));ImGui::TextUnformatted("검출기가 찾았지만 초안에 없는 공 채우기");ImGui::PopFont();
        ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+720);ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"초안의 씨앗은 스테레오 매칭을 거친 공뿐이라 컬러 카메라의 먼 공이 빠졌습니다. 검출기 결과(detections.jsonl)에서 조건에 맞고 초안에 없는 공을 SAM으로 따고 원 맞춤과 색상까지 붙여 더합니다. 사람이 손댄 이미지는 건드리지 않습니다.");ImGui::PopTextWrapPos();ImGui::PopFont();
        ImGui::Dummy({0,2});
        ImGui::BeginDisabled(running);
        ImGui::TextUnformatted("카메라");ImGui::SameLine();ImGui::RadioButton("컬러만",&missingCameras_,0);ImGui::SameLine();ImGui::RadioButton("모든 카메라",&missingCameras_,1);
        ImGui::TextUnformatted("커버리지 기준");ImGui::SameLine();ImGui::SetNextItemWidth(96);ImGui::DragFloat("##coverage",&missingCoverage_,.01f,.5f,1.f,"%.2f");ImGui::SameLine();
        ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"검출기가 본 테두리 비율. 낮출수록 공이 늘고 오검출도 늘어납니다");ImGui::PopFont();
        ImGui::TextUnformatted("반지름 범위");ImGui::SameLine();ImGui::SetNextItemWidth(72);ImGui::DragFloat("##rmin",&missingMinRadius_,.5f,2.f,100.f,"%.0f px");ImGui::SameLine();ImGui::TextUnformatted("~");ImGui::SameLine();ImGui::SetNextItemWidth(72);ImGui::DragFloat("##rmax",&missingMaxRadius_,.5f,2.f,300.f,"%.0f px");
        if(missingMaxRadius_<missingMinRadius_)missingMaxRadius_=missingMinRadius_;
        ImGui::TextUnformatted("시험 장수");ImGui::SameLine();ImGui::SetNextItemWidth(96);ImGui::DragInt("##trial",&missingTrial_,1,5,500,"%d장");ImGui::SameLine();
        ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"고르게 뽑은 프레임에서만 돌려 보고 저장하지 않음 · 둘레 여유는 공 둘레 설정(%+.2f px)",circleMargin_);ImGui::PopFont();
        ImGui::EndDisabled();
        ImGui::Dummy({0,2});
        // What the last run (count, trial or apply) found.
        if(running){char line[96];std::snprintf(line,sizeof line,"%s · %s / %s장",missingStatus_.value("dry_run",false)?"시험 실행 중":"채우는 중",grouped(missingStatus_.value("done",0)).c_str(),grouped(missingStatus_.value("total",0)).c_str());ImGui::TextUnformatted(line);}
        else if(status=="complete"){
            char line[200];
            if(missingStatus_.value("count_only",false))std::snprintf(line,sizeof line,"후보 %s개 · %s장 (손대지 않은 %s장 중)",grouped(missingStatus_.value("candidates",0)).c_str(),grouped(missingStatus_.value("frames_with_candidates",0)).c_str(),grouped(missingStatus_.value("scanned",0)).c_str());
            else if(missingStatus_.value("dry_run",false))std::snprintf(line,sizeof line,"시험 %s장 · 공 %s개 추가 가능 (SAM %s · 기하 %s · 원 맞춤 %s) · 전체 후보 %s개 / %s장",grouped(missingStatus_.value("total",0)).c_str(),grouped(missingStatus_.value("balls",0)).c_str(),grouped(missingStatus_.value("sam",0)).c_str(),grouped(missingStatus_.value("geometry",0)).c_str(),grouped(missingStatus_.value("circles",0)).c_str(),grouped(missingStatus_.value("candidates",0)).c_str(),grouped(missingStatus_.value("frames_with_candidates",0)).c_str());
            else std::snprintf(line,sizeof line,"적용 완료 · %s장에 공 %s개 추가 (SAM %s · 기하 %s · 원 맞춤 %s) · 건너뜀 %s장",grouped(missingStatus_.value("applied_frames",0)).c_str(),grouped(missingStatus_.value("applied_balls",0)).c_str(),grouped(missingStatus_.value("sam",0)).c_str(),grouped(missingStatus_.value("geometry",0)).c_str(),grouped(missingStatus_.value("circles",0)).c_str(),grouped(missingStatus_.value("skipped",0)).c_str());
            ImGui::TextUnformatted(line);
        } else if(status=="error")ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::warn),"%s",missingStatus_.value("error","").c_str());
        else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"먼저 후보를 세거나 시험 실행으로 몇 장을 확인하세요.");
        if(!missingSamples_.empty()&&status=="complete")sampleGrid("시험에서 더해질 공 (각 프레임의 첫 공)");
        // Saving the trial under a name, the saved trials, and the assembly of two of them (2026-10-08 user request).
        if(status=="complete"&&missingStatus_.value("dry_run",false)&&!missingStatus_.value("count_only",false)&&missingStatus_.contains("results")){
            ImGui::BeginDisabled(busy());
            ImGui::TextUnformatted("이 시험을 저장");ImGui::SameLine();ImGui::SetNextItemWidth(110);ImGui::InputText("##trial-name",trialName_,sizeof trialName_);ImGui::SameLine();
            if(ImGui::Button("시험 저장",{96,26}))later_=[this]{missingAction("save-missing-trial",{{"name",std::string(trialName_)}},"시험을 저장했습니다");};
            ImGui::EndDisabled();
        }
        if(!missingTrials_.empty()){
            ImGui::Dummy({0,2});
            ImGui::PushFont(theme::bold,theme::px(theme::base));ImGui::TextUnformatted("저장한 시험");ImGui::PopFont();
            std::string names;for(const auto& t:missingTrials_){names+=t.value("name","");names+='\0';}names+='\0';
            for(size_t i=0;i<missingTrials_.size();++i){
                const auto& t=missingTrials_[i];const std::string name=t.value("name","");ImGui::PushID(int(i));
                char line[160];
                if(t["derived"].is_string())std::snprintf(line,sizeof line,"%s  =  %s  ·  %d장 · 공 %d개",name.c_str(),t["derived"].get<std::string>().c_str(),t.value("frames",0),t.value("balls",0));
                else{const auto& o=t["options"];std::snprintf(line,sizeof line,"%s  ·  %s · 커버리지 %.2f · r %.0f~%.0f · %s  ·  %d장 · 공 %d개",name.c_str(),o.value("cameras","")=="all"?"모든 카메라":"컬러",o.value("coverage",0.0),o.value("min_radius",0.0),o.value("max_radius",0.0),o.value("limit_frames",0)>0?(std::to_string(o.value("limit_frames",0))+"장 시험").c_str():"전체",t.value("frames",0),t.value("balls",0));}
                ImGui::TextUnformatted(line);ImGui::SameLine();
                ImGui::BeginDisabled(busy()||running);
                if(ImGui::SmallButton("샘플"))later_=[this,name]{missingAction("missing-trial-samples",{{"name",name},{"limit",8}},"시험 "+name);};ImGui::SameLine();
                if(ImGui::SmallButton("적용"))later_=[this,name]{missingAction("apply-missing-trial",{{"name",name}},"시험 "+name+" 적용");};ImGui::SameLine();
                if(ImGui::SmallButton("지우기"))later_=[this,name]{missingAction("drop-missing-trial",{{"name",name}},"시험 "+name+"을 지웠습니다");};
                ImGui::EndDisabled();ImGui::PopID();
            }
            if(missingTrials_.size()>=2){
                ImGui::TextUnformatted("조립");ImGui::SameLine();ImGui::SetNextItemWidth(120);ImGui::Combo("##ca",&combineA_,names.c_str());ImGui::SameLine();
                ImGui::SetNextItemWidth(110);ImGui::Combo("##cop",&combineOp_,"or (합집합)\0and (교집합)\0and not (차집합)\0\0");ImGui::SameLine();
                ImGui::SetNextItemWidth(120);ImGui::Combo("##cb",&combineB_,names.c_str());ImGui::SameLine();
                ImGui::BeginDisabled(busy()||running||combineA_==combineB_);
                if(ImGui::Button("조립",{70,26}))later_=[this]{
                    const std::string a=missingTrials_[combineA_].value("name",""),b=missingTrials_[combineB_].value("name","");const char* op=combineOp_==0?"or":combineOp_==1?"and":"and_not";
                    const std::string name=a+(combineOp_==0?" or ":combineOp_==1?" and ":" and not ")+b;
                    missingAction("combine-missing-trials",{{"name",name},{"op",op},{"a",a},{"b",b}},"조립했습니다: "+name);};
                ImGui::EndDisabled();
                ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"같은 프레임에서 중심이 반지름 안에 들면 같은 공으로 봅니다. 조립한 결과도 시험으로 저장되어 다시 조립하거나 적용할 수 있습니다.");ImGui::PopFont();
            }
        }
        ImGui::Dummy({0,4});
        bool close=false;
        ImGui::BeginDisabled(running||busy());
        if(ImGui::Button("후보 세기",{110,34}))later_=[this]{startMissing(false,true);};
        ImGui::SameLine();
        {char label[48];std::snprintf(label,sizeof label,"시험 실행 %d장",missingTrial_);if(ImGui::Button(label,{150,34}))later_=[this]{startMissing(true,false);};}
        ImGui::SameLine();
        ImGui::PushFont(theme::bold,theme::px(theme::base));
        if(theme::primaryButton("전체 적용",{130,34}))later_=[this]{startMissing(false,false);};
        ImGui::PopFont();
        ImGui::EndDisabled();ImGui::SameLine();
        if(running){if(ImGui::Button("중단",{90,34}))later_=[this]{const auto engine=engine_;launch("누락 공 작업 중단",[engine]{return JobResult{engine->nativeAction("stop-missing",Json::object()),{}};},[this](JobResult result){missingStatus_=result.data;});};ImGui::SameLine();}
        if(ImGui::Button("닫기  Esc",{100,34})||ImGui::IsKeyPressed(ImGuiKey_Escape,false))close=true;
        if(close){missingAsk_=false;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}
// 이전 라벨 가져오기 (2026-10-20 user request: a button, in place of the on/off that showed them on every draft): the labels
// of the nearest earlier confirmed frame of each camera, carried onto the frames of this scene now, as an edit of each frame
// they change (되돌리기 per camera, saved like any edit). Unsaved edits are saved first: the carry starts from the saved frames.
void Application::applyPreviousLabels(){
    if(frames_.empty()||imageScene()||!engine_)return;
    if(busy()&&!pending()){carryWanted_=true;message_=busyLabel_+" 중 · 끝나면 이전 라벨을 가져옵니다";return;}   // F1 during a load or a save
    if(!guard())return;
    if(const int slot=dirtySlot();slot>=0){save([this]{applyPreviousLabels();},slot);return;}
    Json keys=Json::array();for(const auto& frame:frames_)keys.push_back(frame.annotation.at("key"));   // the repeated cameras too
    const auto engine=engine_;const int scene=scene_;
    launch("이전 라벨 가져오기",[engine,keys]{return JobResult{engine->nativeAction("carry-preview",{{"keys",keys},{"touched",true}}),{}};},
           [this,scene](JobResult result){
        if(scene_!=scene)return;   // another scene opened meanwhile
        std::string from;int changes=0;
        for(auto& frame:frames_){const std::string key=frame.annotation.at("key");if(!result.data["frames"].contains(key))continue;
            const Json& carried=result.data["frames"][key];
            checkpoint(frame);frame.annotation["boxes"]=carried["boxes"];edited(frame,true);from=carried.value("from","");++changes;}
        if(!changes){message_=result.data.value("same",Json::array()).empty()?"이전 확인본 없음":"이전 라벨과 같음";return;}
        selected_=-1;
        char number[16];std::snprintf(number,sizeof number,"%04d",std::atoi(from.substr(from.rfind('/')+1).c_str()));
        message_=std::string("이전 라벨 · 프레임 ")+number;
        logLine("이전 라벨 가져오기 · "+hubSceneName()+" ← "+number);});
}
// Confirmed labels carried to the repeated frames the filter left out of the review (2026-10-08 user request), and into the
// drafts that follow a confirmed frame, in the engine's background thread (전파 → on the scene bar until 2026-10-20).
void Application::startPropagation(){
    if(busy())return;const auto engine=engine_;Json payload=batchPayload_;payload.update({{"force",propagateForce_},{"carry",propagateCarry_}});
    launch("라벨 전파 시작",[engine,payload]{return JobResult{engine->nativeAction("propagate-reviewed",payload),{}};},[this](JobResult result){propagationStatus_=result.data;message_="반복 프레임에 라벨을 전파하는 중입니다. 그동안 계속 작업할 수 있습니다.";});
}
// Balls partly hidden by a hand, a person or the frame edge, marked by themselves (2026-10-08 user request). The rim
// share, the mask's circularity and the box's aspect decide; a rim hidden by a neighbouring ball on the floor is exempt.
void Application::startOcclusion(bool dryRun,bool countOnly,int limit){
    if(busy())return;const auto engine=engine_;
    Json payload={{"coverage",double(occlCoverage_)},{"circularity",double(occlCircularity_)},{"aspect",double(occlAspect_)},{"include_reviewed",occlReviewed_},
                        {"dry_run",dryRun},{"count_only",countOnly},{"limit_frames",limit>=0?limit:dryRun?occlTrial_:0},{"sample_limit",8}};
    payload.update(batchPayload_);
    launch(countOnly?"가림 후보 세기":dryRun?"가림 시험 실행":"가림 표시",[engine,payload]{return JobResult{engine->nativeAction("mark-occluded",payload),{}};},
           [this,countOnly](JobResult result){occlusionStatus_=result.data;message_=countOnly?"아직 표시 없는 공 "+grouped(occlusionStatus_.value("balls_total",0))+"개 · "+grouped(occlusionStatus_.value("frames_total",0))+"장":"가림 판정을 시작했습니다. 그동안 계속 작업할 수 있습니다.";});
}
void Application::confirmOcclusion(){
    if(!occludeAsk_)return;
    if(!ImGui::IsPopupOpen("##mark-occluded"))ImGui::OpenPopup("##mark-occluded");
    const auto viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x+viewport->WorkSize.x/2,viewport->WorkPos.y+viewport->WorkSize.y/2},ImGuiCond_Appearing,{.5f,.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{22,20});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8,10});
    if(ImGui::BeginPopupModal("##mark-occluded",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoTitleBar)){
        const std::string status=occlusionStatus_.value("status","idle");const bool running=status=="running";
        ImGui::PushFont(theme::bold,theme::px(theme::title));ImGui::TextUnformatted("가려진 공 자동 표시");ImGui::PopFont();
        ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+720);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"손·사람·화면 가장자리에 가려진 공을 찾아 일부 가림으로 표시합니다. 근거는 Canny 원 맞춤의 테두리 지지율, 마스크 원형도, 박스 가로세로 비율입니다. 빠진 테두리 쪽에 다른 공이 닿아 있으면 바닥에서 공끼리 겹친 것이라 표시하지 않습니다(공 전체 규칙). 표시가 켜진 공과 사람이 확인한 이미지는 건드리지 않습니다.");
        ImGui::PopTextWrapPos();ImGui::PopFont();
        ImGui::Dummy({0,2});
        ImGui::BeginDisabled(running);
        ImGui::TextUnformatted("테두리 지지율");ImGui::SameLine();ImGui::SetNextItemWidth(90);ImGui::DragFloat("##ocov",&occlCoverage_,.01f,.3f,1.f,"%.2f");ImGui::SameLine();
        ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"이보다 적은 테두리 구간에서만 에지가 잡히면 가림. 흔들린 온전한 공도 0.6 근처라 0.5가 안전");ImGui::PopFont();
        ImGui::TextUnformatted("마스크 원형도");ImGui::SameLine();ImGui::SetNextItemWidth(90);ImGui::DragFloat("##ocirc",&occlCircularity_,.01f,.3f,1.f,"%.2f");ImGui::SameLine();
        ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"마스크 넓이 ÷ 박스에 맞는 원 넓이");ImGui::PopFont();
        ImGui::TextUnformatted("박스 비율");ImGui::SameLine();ImGui::SetNextItemWidth(90);ImGui::DragFloat("##oasp",&occlAspect_,.01f,.3f,1.f,"%.2f");ImGui::SameLine();
        ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"짧은 변 ÷ 긴 변");ImGui::PopFont();
        theme::checkbox("확인한 이미지의 공도 판정 (표시를 켜기만 함)",&occlReviewed_);
        ImGui::TextUnformatted("시험 장수");ImGui::SameLine();ImGui::SetNextItemWidth(96);ImGui::DragInt("##otrial",&occlTrial_,1,5,500,"%d장");
        ImGui::EndDisabled();
        ImGui::Dummy({0,2});
        if(running){char line[96];std::snprintf(line,sizeof line,"%s · %s / %s장",occlusionStatus_.value("dry_run",false)?"시험 실행 중":"표시하는 중",grouped(occlusionStatus_.value("done",0)).c_str(),grouped(occlusionStatus_.value("total",0)).c_str());ImGui::TextUnformatted(line);}
        else if(status=="complete"){
            char line[220];
            if(occlusionStatus_.value("count_only",false))std::snprintf(line,sizeof line,"아직 표시 없는 공 %s개 · %s장",grouped(occlusionStatus_.value("balls_total",0)).c_str(),grouped(occlusionStatus_.value("frames_total",0)).c_str());
            else if(occlusionStatus_.value("dry_run",false))std::snprintf(line,sizeof line,"시험 %s장 · 가림으로 판정 %s개 (%s장) · 옆 공에 가려 제외 %s개",grouped(occlusionStatus_.value("total",0)).c_str(),grouped(occlusionStatus_.value("balls",0)).c_str(),grouped(occlusionStatus_.value("frames",0)).c_str(),grouped(occlusionStatus_.value("neighbours",0)).c_str());
            else std::snprintf(line,sizeof line,"적용 완료 · 공 %s개를 일부 가림으로 표시 (%s장) · 옆 공에 가려 제외 %s개 · 건너뜀 %s장",grouped(occlusionStatus_.value("applied_balls",0)).c_str(),grouped(occlusionStatus_.value("applied_frames",0)).c_str(),grouped(occlusionStatus_.value("neighbours",0)).c_str(),grouped(occlusionStatus_.value("skipped",0)).c_str());
            ImGui::TextUnformatted(line);
        } else if(status=="error")ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::warn),"%s",occlusionStatus_.value("error","").c_str());
        else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"먼저 후보를 세거나 시험 실행으로 몇 장을 확인하세요.");
        if(!missingSamples_.empty()&&status=="complete"&&occlusionStatus_.value("dry_run",false)&&!occlusionStatus_.value("count_only",false))sampleGrid("가림으로 판정된 공 (각 프레임의 첫 공, 근거)");
        ImGui::Dummy({0,4});
        bool close=false;
        ImGui::BeginDisabled(running||busy());
        if(ImGui::Button("후보 세기",{110,34}))later_=[this]{startOcclusion(false,true);};
        ImGui::SameLine();
        {char label[48];std::snprintf(label,sizeof label,"시험 실행 %d장",occlTrial_);if(ImGui::Button(label,{150,34}))later_=[this]{startOcclusion(true,false);};}
        ImGui::SameLine();
        ImGui::PushFont(theme::bold,theme::px(theme::base));
        if(theme::primaryButton("전체 적용",{130,34}))later_=[this]{startOcclusion(false,false);};
        ImGui::PopFont();
        ImGui::EndDisabled();ImGui::SameLine();
        if(running){if(ImGui::Button("중단",{90,34}))later_=[this]{const auto engine=engine_;launch("가림 판정 중단",[engine]{return JobResult{engine->nativeAction("stop-occluded",Json::object()),{}};},[this](JobResult result){occlusionStatus_=result.data;});};ImGui::SameLine();}
        if(ImGui::Button("닫기  Esc",{100,34})||ImGui::IsKeyPressed(ImGuiKey_Escape,false))close=true;
        if(close){occludeAsk_=false;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}
void Application::undo(){if(busy())return;if(pending()){cancel();return;}auto& f=current();if(f.history.empty())return;logLine("되돌리기 · "+cameraLabel(f.member));f.annotation=f.history.back();f.history.pop_back();selected_=f.annotation["boxes"].empty()?-1:std::min(selected_,int(f.annotation["boxes"].size())-1);changed();}
void Application::segment(cv::Point2d point,bool negative) {
    if(busy())return;
    if(!proposal_.is_null()&&proposal_.value("source","")!="efficientsam"){message_="현재 윤곽을 먼저 반영하거나 취소하세요.";return;}
    if(points_.empty()) {if(negative){message_="첫 점은 공 내부에 클릭하세요.";return;}replacement_=target(current(),point);if(replacement_>=0)selected_=replacement_;}
    if(points_.size()>=32){message_="점은 최대 32개입니다. 현재 결과를 반영하거나 취소하세요.";return;}
    points_.push_back({{"x",point.x},{"y",point.y},{"label",negative?0:1}});
    auto payload=annotationPayload(current().annotation,catalog_["summary"]["revision"].get<int>());payload["points"]=points_;payload["replacement"]=replacement_;payload["colour"]=fitCircle_;payload["radius"]=radius_;
    const auto key=current().annotation.at("key").get<std::string>();const auto engine=engine_;const auto token=generation_;
    launch("EfficientSAM으로 공 윤곽 분리",[engine,payload]{return JobResult{engine->assist("segment",payload),{}};},[this,key,token](JobResult result){if(token!=generation_||current().annotation.value("key","")!=key)return;proposal_=result.data.at("proposals").at(0);current().repaint=true;message_=proposal_.contains("circle")?"제안 미확정 · 원 맞춤 완료 · Space 반영, Esc 취소":"제안 미확정 · 원 맞춤 실패, 마스크 경계 박스 · Space 반영, Esc 취소";});
}
// A dragged box is sent together with its center as the positive point.
void Application::segmentBox(cv::Rect2d box) {
    if(busy()||!points_.empty()||box.width<2||box.height<2)return;
    const cv::Point2d center{box.x+box.width/2,box.y+box.height/2};
    replacement_=target(current(),center);if(replacement_>=0)selected_=replacement_;
    points_.push_back({{"x",center.x},{"y",center.y},{"label",1}});
    auto payload=annotationPayload(current().annotation,catalog_["summary"]["revision"].get<int>());payload["points"]=points_;payload["replacement"]=replacement_;payload["colour"]=fitCircle_;payload["radius"]=std::max(box.width,box.height)/2;payload["box"]={box.x,box.y,box.width,box.height};
    const auto key=current().annotation.at("key").get<std::string>();const auto engine=engine_;const auto token=generation_;
    launch("EfficientSAM으로 공 윤곽 분리",[engine,payload]{return JobResult{engine->assist("segment",payload),{}};},[this,key,token](JobResult result){if(token!=generation_||current().annotation.value("key","")!=key)return;proposal_=result.data.at("proposals").at(0);current().repaint=true;message_=proposal_.contains("circle")?"제안 미확정 · 원 맞춤 완료 · Space 반영, Esc 취소":"제안 미확정 · 원 맞춤 실패, 마스크 경계 박스 · Space 반영, Esc 취소";});
}
void Application::polygonProposal(){
    if(busy()||polygon_.size()<3)return;Json points=Json::array();for(auto point:polygon_)points.push_back({point.x,point.y});
    // An outline follows the edge of a ball, so its middle says which ball it is; its first point may lie in a neighbour's box.
    replacement_=target(current(),boundsCenter(polygon_));if(replacement_>=0)selected_=replacement_;
    auto payload=annotationPayload(current().annotation,catalog_["summary"]["revision"].get<int>());payload["points"]=points;payload["colour"]=fitCircle_;const auto engine=engine_;const auto token=generation_;
    launch("수동 폴리곤 마스크 만들기",[engine,payload]{return JobResult{engine->assist("polygon",payload),{}};},[this,token](JobResult result){if(token!=generation_)return;proposal_=result.data.at("proposals").at(0);polygon_.clear();current().repaint=true;message_=proposal_.contains("circle")?"폴리곤 미리보기 · 원 맞춤 완료 · Space로 반영":"폴리곤 미리보기 · 원 맞춤 실패, 마스크 경계 박스 · Space로 반영";});
}
void Application::review(Scope scope,bool hold,int member) {
    if(frames_.empty())return;
    // Space during the autosave or another job is kept, not dropped (2026-10-08 user report). If the job was this scene's
    // own confirmation, the second press means "move on", as it would from the prompt.
    if(busy()&&!pending()){
        queued_=[this,scope,hold,member,scene=scene_]{if(scene_!=scene)return;if(nextAsk_){nextAsk_=false;advance();}else review(scope,hold,member);};
        message_=busyLabel_+" 중 · 끝나면 바로 "+(hold?"보류합니다":"확인합니다");return;}
    if(!guard())return;
    Json keys=Json::array();
    if(scope==Scope::Camera)keys.push_back(frames_.at(member>=0?member:camera_).annotation.at("key"));
    else for(auto& frame:frames_)if(frame.member.value("selected",true)&&(!hold||!frame.member.value("reviewed",false)))keys.push_back(frame.annotation.at("key"));
    if(keys.empty())return;
    // The screen's unsaved edits go in the same write as the confirmation (2026-10-19: a save and then a confirmation wrote
    // the label file twice). An edited frame outside it is saved first, as before.
    Json edits=Json::object();std::vector<size_t> carried;
    for(size_t i=0;i<frames_.size();++i){auto& frame=frames_[i];const std::string key=frame.annotation.at("key");
        const bool inKeys=std::find(keys.begin(),keys.end(),Json(key))!=keys.end();
        if(frame.dirty&&inKeys){Json boxes=frame.annotation["boxes"];for(auto& box:boxes)box.erase("contours");edits[key]=std::move(boxes);carried.push_back(i);}
        else if(frame.dirty){save([this,scope,hold,member]{review(scope,hold,member);},int(i));return;}}
    const bool moveOn=scope==Scope::Scene||hold;
    // The engine refuses when it holds a different number of balls than this screen shows, as after an automatic draft.
    Json seen=Json::object();for(const auto& frame:frames_)seen[frame.annotation.at("key").get<std::string>()]=frame.annotation["boxes"].size();
    const auto engine=engine_;Json payload={{"keys",keys},{"seen",seen},{"edits",edits},{"status",hold?"hold":"reviewed"},{"expected_revision",catalog_["summary"]["revision"]},{"worker",settings_.worker}};
    launch(hold?"보류 상태 저장":"확인 저장",[engine,payload]{
        try{return JobResult{engine->nativeAction("review",payload),{}};}
        catch(const Conflict&){return JobResult{{{"conflict",true},{"catalog",engine->nativeCatalog(false)}},{}};}
    },[this,moveOn,hold,keys,scope,carried](JobResult result){
        if(result.data.contains("conflict")){updateCatalog(result.data["catalog"]);loadScene(scene_,editor_,camera_,"저장된 라벨이 화면과 달라 다시 불러왔습니다. 확인한 뒤 다시 누르세요.");return;}
        updateSummary(result.data.at("summary"));
        for(auto& f:frames_)if(std::find(keys.begin(),keys.end(),f.annotation["key"])!=keys.end()){f.annotation["reviewed"]=!hold;f.annotation["review_status"]=hold?"hold":"reviewed";}
        for(const size_t i:carried)if(i<frames_.size())frames_[i].dirty=false;
        {int balls=0;for(const auto& frame:frames_)balls+=int(frame.annotation.value("boxes",Json::array()).size());logLine((hold?"보류 · ":"장면 확인 · ")+hubSceneName()+(hold?std::string():" · 공 "+std::to_string(balls)+"개"));}
        message_=hold?"보류로 표시했습니다":"확인했습니다";
        // A hold moves on at once; a confirmation stops at a prompt, and a second Space moves on (2026-10-08 user request).
        if(hold&&moveOn)advance();else if(scope==Scope::Scene)nextAsk_=true;});
}
// Held images are skipped; they are revisited through the list filter.
int Application::nextUnreviewedIndex() const {
    const int count=int(catalog_["scenes"].size());
    for(int offset=1;offset<=count;++offset){const int index=(scene_+offset)%count;for(const auto& member:catalog_["scenes"][index]["members"])if(member.value("selected",true)&&!member.value("reviewed",false)&&member.value("status","")!="hold")return index;}
    return -1;
}
// Returns to the scene shown before this one, which E and N may have skipped far away from.
void Application::back(){
    if(history_.empty()||!guard())return;
    const int previous=history_.back();history_.pop_back();backing_=true;navigate(previous);
}
// The scene to move on to after a confirm or hold: the very next scene that holds a review target, reviewed or not. After
// a correction in the middle this goes on to the next frame, not past the last confirmed one (2026-10-08 user request).
// Past the end, the next unreviewed scene anywhere.
int Application::advanceIndex() const {
    // With a list tab other than 전체 chosen, the flow follows that list: on the 미검수 tab the next unreviewed scene, on
    // 보류 the next held one, on 확인됨 the next confirmed one (2026-10-09 user request). The rows are in scene order.
    if(filter_!=0&&!rows_.empty()){
        for(const auto& row:rows_)if(!row.header&&row.scene>scene_)return row.scene;
        for(const auto& row:rows_)if(!row.header&&row.scene>=0&&row.scene!=scene_)return row.scene;   // past the end: around to the first
        return -1;
    }
    const int count=int(catalog_["scenes"].size());
    for(int index=scene_+1;index<count;++index)for(const auto& member:catalog_["scenes"][index]["members"])if(member.value("selected",true))return index;
    return nextUnreviewedIndex();
}
void Application::advance(){
    if(busy()){queued_=[this,scene=scene_]{if(scene_==scene)advance();};message_=busyLabel_+" 중 · 끝나면 다음 장면으로 넘어갑니다";return;}
    const int index=advanceIndex();
    if(index<0){editor_=false;message_="검수 대상을 모두 확인했습니다. 분할 단계로 넘어가세요.";return;}
    if(index==scene_){message_="남은 미검수 이미지는 이 장면뿐입니다.";return;}
    loadScene(index);
}
void Application::nextUnreviewed(){
    if(busy()){queued_=[this,scene=scene_]{if(scene_==scene)nextUnreviewed();};message_=busyLabel_+" 중 · 끝나면 다음 미검수 장면으로 넘어갑니다";return;}
    const int index=nextUnreviewedIndex();
    if(index<0){editor_=false;message_="검수 대상을 모두 확인했습니다. 분할 단계로 넘어가세요.";return;}
    if(index==scene_){message_="남은 미검수 이미지는 이 장면뿐입니다.";return;}
    loadScene(index);
}
void Application::selectSceneForCapture(int recordingNumber,int index){if(!ready()||busy())return;for(int i=0;i<int(catalog_["scenes"].size());++i)if(catalog_["scenes"][i].value("recording_number",0)==recordingNumber&&catalog_["scenes"][i].value("index",-1)==index){loadScene(i);return;}}
int Application::runningJobs() const {
    int count=0;
    for(const Json* status:{&exportStatus_,&colorStatus_,&circleStatus_,&missingStatus_,&propagationStatus_,&occlusionStatus_,&redraftStatus_})count+=status->value("status","")=="running";
    return count+(autoStatus_.value("job",Json::object()).value("status","")=="running");
}
// The catalog read again on the status thread, and the scene shown again when the labels changed under it.
void Application::refreshCatalog(bool reloadScene){
    const auto engine=engine_;
    pollTask_=std::async(std::launch::async,[engine]{return JobResult{{{"catalog",engine->nativeCatalog(false)}},{}};});
    pollDone_=[this,reloadScene](JobResult result){updateCatalog(result.data["catalog"]);if(reloadScene){if(page_==Page::Review)loadScene(scene_,editor_,camera_);else stale_=true;}};
}
void Application::tick(){
    if(pollTask_.valid()&&pollTask_.wait_for(std::chrono::seconds(0))==std::future_status::ready){
        auto done=std::move(pollDone_);pollDone_=nullptr;
        try{auto result=pollTask_.get();if(done)done(std::move(result));}catch(const std::exception&){}   // a failed check: the next one tries again
    }
    // Every notice and error the screen shows goes to the log as well (labeler_log.hpp).
    if(error_!=loggedError_){if(!error_.empty())logLine("오류 · "+error_);loggedError_=error_;}
    // An export's end, once, whoever saw it end (the export tab polls its export itself, so the status poll may not).
    if(!exportDemo_&&exportStatus_.is_object()){const std::string state=exportStatus_.value("status",""),path=exportStatus_.value("path","");
        if(!path.empty()&&!state.empty()&&state!="running"&&path!=exportLogged_){exportLogged_=path;
            std::string formats;for(const auto& [on,name]:{std::pair{exportYolo_,"YOLO"},{exportCocoDetection_,"COCO 검출"},{exportCocoMask_,"COCO 마스크"},{exportCrops_,"크롭"}})if(on)formats+=std::string(formats.empty()?"":" · ")+name;
            if(exportStatus_.contains("error"))logLine("오류 · 내보내기 · "+(exportStatus_["error"].is_string()?exportStatus_["error"].get<std::string>():exportStatus_["error"].dump()));
            else if(state=="stopped")logLine("내보내기 중단");
            else logLine("내보내기 · "+formats+" · "+grouped(exportStatus_.value("total",0))+"장 · "+utf8(fromUtf8(path).make_preferred()));}}
    if(message_!=loggedMessage_){if(!message_.empty())logLine("알림 · "+message_);loggedMessage_=message_;}
    if(task_.valid()&&task_.wait_for(std::chrono::seconds(0))==std::future_status::ready){
        auto complete=std::move(completion_);
        try {auto result=task_.get();busyLabel_.clear();complete(std::move(result));}
        catch(const std::exception& error){error_=error.what();busyLabel_.clear();importProgress_.reset();message_="처리에 실패했습니다. 변경과 현재 위치를 유지합니다.";
            if(!frames_.empty()&&current().dirty){try{preserveRecovery();message_="저장 실패 · 변경은 메모리와 recovery 폴더에 보관했습니다.";}catch(...) {}}
        }
    }
    // The key pressed during the job: done now, or dropped when the job failed. F1 first: F1 then Space confirms what it carried.
    if(carryWanted_){if(!error_.empty())carryWanted_=false;else if(!busy()){carryWanted_=false;applyPreviousLabels();}}
    if(queued_){if(!error_.empty())queued_=nullptr;else if(!busy()){auto action=std::move(queued_);queued_=nullptr;action();}}
    pumpThumbs();prefetch();hubTick();
    if(ready()&&!busy()&&!pending()&&dirtySlot()>=0&&Clock::now()-changed_>std::chrono::milliseconds(450)&&error_.empty())save({},dirtySlot());
    const bool activeJob=autoStatus_.value("job",Json::object()).value("status","")=="running"||exportStatus_.value("status","")=="running"||colorStatus_.value("status","")=="running"||circleStatus_.value("status","")=="running"||missingStatus_.value("status","")=="running"||propagationStatus_.value("status","")=="running"||occlusionStatus_.value("status","")=="running"||redraftStatus_.value("status","")=="running";
    const auto pollEvery=propagationStatus_.value("status","")=="running"?std::chrono::milliseconds(400):std::chrono::milliseconds(3000);   // a live carry is short; its result should show quickly
    if(activeJob&&ready()&&!pollTask_.valid()&&Clock::now()-poll_>pollEvery&&error_.empty()&&engine_)poll();
}
// The status check of the background jobs (2026-10-19: it took the slot of the person's own actions, so Space waited
// for it): statuses and the label revisions only; the catalog, a pass over every frame, only when something changed.
void Application::poll(){
    {
        poll_=Clock::now();const auto engine=engine_;
        pollTask_=std::async(std::launch::async,[engine]{return JobResult{{{"auto",engine->autoStatus()},{"export",engine->exportStatus()},{"color",engine->colorFillStatus()},{"circle",engine->circleRefineStatus()},{"missing",engine->missingFillStatus()},{"propagation",engine->propagationStatus()},{"occlusion",engine->occlusionStatus()},{"redraft",engine->redraftStatus()},{"revisions",engine->revisions()}}, {}};});
        pollDone_=[this](JobResult result){const int runningBefore=runningJobs();
            const auto oldRevision=catalog_["summary"].value("revision",0),oldAuto=catalog_["summary"].value("auto_revision",0);const bool exporting=exportStatus_.value("status","")=="running";
            const bool coloring=colorStatus_.value("status","")=="running",refining=circleStatus_.value("status","")=="running",filling=missingStatus_.value("status","")=="running",propagating=propagationStatus_.value("status","")=="running";
            propagationStatus_=result.data["propagation"];
            const bool marking=occlusionStatus_.value("status","")=="running";occlusionStatus_=result.data["occlusion"];
            if(marking&&occlusionStatus_.value("status","")!="running"){
                if(occlusionStatus_.value("status","")!="complete"){message_="가림 판정이 실패했습니다: "+occlusionStatus_.value("error","");logLine(jobEndProblem("가려진 공 표시",occlusionStatus_));}
                else if(occlusionStatus_.value("dry_run",false)){logLine("가려진 공 표시 · 시험 · 공 "+grouped(occlusionStatus_.value("balls",0))+"개 · "+grouped(occlusionStatus_.value("frames",0))+"장");message_="가림 시험 완료 · "+grouped(occlusionStatus_.value("frames",0))+"장의 공 "+grouped(occlusionStatus_.value("balls",0))+"개가 가려진 것으로 판정 (저장 안 함)";loadMissingSamples(occlusionStatus_.value("samples",Json::array()));}
                else{logLine("가려진 공 표시 · 공 "+grouped(occlusionStatus_.value("applied_balls",0))+"개 · "+grouped(occlusionStatus_.value("applied_frames",0))+"장");message_="공 "+grouped(occlusionStatus_.value("applied_balls",0))+"개를 일부 가림으로 표시했습니다 ("+grouped(occlusionStatus_.value("applied_frames",0))+"장).";if(page_==Page::Review)loadScene(scene_,editor_,camera_,message_);else stale_=true;}}
            if(propagating&&propagationStatus_.value("status","")!="running"){
                if(propagationStatus_.value("live",false)){if(propagationStatus_.value("carried",0)+propagationStatus_.value("frames",0)>0)message_="확인한 라벨을 다음 초안 "+grouped(propagationStatus_.value("carried",0))+"장과 반복 "+grouped(propagationStatus_.value("frames",0))+"장에 맞췄습니다.";}
                else{if(propagationStatus_.value("status","")=="complete")logLine("전파 → · 반복 "+grouped(propagationStatus_.value("frames",0))+"장 · 초안 "+grouped(propagationStatus_.value("carried",0))+"장");
                    if(propagationStatus_.value("status","")!="complete")logLine(jobEndProblem("전파",propagationStatus_));
                    message_=propagationStatus_.value("status","")=="complete"?"반복 프레임 "+grouped(propagationStatus_.value("frames",0))+"장에 라벨을 전파하고 초안 "+grouped(propagationStatus_.value("carried",0))+"장을 채웠습니다 (공 "+grouped(propagationStatus_.value("balls",0))+"개).":"라벨 전파가 실패했습니다: "+propagationStatus_.value("error","");if(page_==Page::Review)loadScene(scene_,editor_,camera_,message_);else stale_=true;}
                }
            {   // 초안 다시 만들기 finished: say so and show the new drafts
                const bool redrafting=redraftStatus_.value("status","")=="running";redraftStatus_=result.data["redraft"];
                if(redrafting&&redraftStatus_.value("status","")!="running"){
                    const std::string state=redraftStatus_.value("status","");
                    message_=state=="complete"?"초안을 다시 만들었습니다 · "+grouped(redraftStatus_.value("drafted",0))+"장":state=="stopped"?"초안 다시 만들기를 중단했습니다.":"초안 다시 만들기가 실패했습니다: "+redraftStatus_.value("error","");
                    logLine(state=="complete"?"초안 다시 만들기 · "+grouped(redraftStatus_.value("drafted",0))+"장":jobEndProblem("초안 다시 만들기",redraftStatus_));
                    if(page_==Page::Review)loadScene(scene_,editor_,camera_,message_);else stale_=true;
                    if(batchTools_)queued_=[this]{refreshBatchScope();};}
            }
            autoStatus_=result.data["auto"];exportStatus_=result.data["export"];colorStatus_=result.data["color"];circleStatus_=result.data["circle"];missingStatus_=result.data["missing"];
            if(filling&&missingStatus_.value("status","")!="running"){
                const bool trial=missingStatus_.value("dry_run",false);
                const bool circles=missingStatus_.value("method","")=="circles";const std::string tool=circles?"원형 검출":"빠진 공 찾기";
                if(missingStatus_.value("status","")=="complete"&&trial)logLine(tool+(circles?" · 찾기":" · 시험")+" · 공 "+grouped(missingStatus_.value("balls",0))+"개 · "+grouped(missingStatus_.value("frames",0))+"장");
                else if(missingStatus_.value("status","")=="complete")logLine(tool+(circles?" · 더하기":"")+" · 공 "+grouped(missingStatus_.value("applied_balls",0))+"개 · "+grouped(missingStatus_.value("applied_frames",0))+"장");
                else logLine(jobEndProblem(tool,missingStatus_));
                if(missingStatus_.value("status","")!="complete")message_="누락 공 채우기가 실패했습니다: "+missingStatus_.value("error","");
                else if(trial&&missingStatus_.value("method","")=="circles"){message_="원형 검출 · 새 공 "+grouped(missingStatus_.value("balls",0))+"개 · "+grouped(missingStatus_.value("frames",0))+"장";loadMissingSamples();}
                else if(trial){message_="시험 실행 완료 · "+grouped(missingStatus_.value("frames",0))+"장에 공 "+grouped(missingStatus_.value("balls",0))+"개 (저장 안 함)";loadMissingSamples();}
                else{message_="누락 공 "+grouped(missingStatus_.value("applied_balls",0))+"개를 "+grouped(missingStatus_.value("applied_frames",0))+"장에 추가했습니다.";if(page_==Page::Review)loadScene(scene_,editor_,camera_,message_);else stale_=true;}}
            if(refining&&circleStatus_.value("status","")!="running"){logLine(circleStatus_.value("status","")=="complete"?"둘레 다듬기 · 공 "+grouped(circleStatus_.value("balls",0))+"개":jobEndProblem("둘레 다듬기",circleStatus_));message_=circleStatus_.value("status","")=="complete"?"공 "+grouped(circleStatus_.value("balls",0))+"개의 둘레를 다시 쟀습니다. 건너뛴 공 "+grouped(circleStatus_.value("skipped",0))+"개.":"둘레 다듬기가 실패했습니다.";if(page_==Page::Review)loadScene(scene_,editor_,camera_,message_);else stale_=true;}
            if(coloring&&colorStatus_.value("status","")!="running"){logLine(colorStatus_.value("status","")=="complete"?"색 채우기 · 공 "+grouped(colorStatus_.value("filled",0))+"개":jobEndProblem("색 채우기",colorStatus_));message_=colorStatus_.value("status","")=="complete"?"공 "+grouped(colorStatus_.value("filled",0))+"개의 색상을 채웠습니다.":"색상 채우기가 실패했습니다.";if(page_==Page::Review)loadScene(scene_,editor_,camera_,message_);else stale_=true;}
            const Json revisions=result.data.value("revisions",Json::object());
            const bool changed=oldRevision!=revisions.value("revision",0)||oldAuto!=revisions.value("auto_revision",0);
            if(changed||runningJobs()<runningBefore)refreshCatalog(changed);   // the catalog, then the scene again when the labels moved under it
            if(exporting&&exportStatus_.value("status","")!="running"){message_=exportStatus_.contains("error")?"내보내기가 실패했습니다. 내보내기 화면에서 원인을 확인하세요.":"내보내기를 마쳤습니다.";refreshStage();}
        };
    }
}
void Application::uploadTexture(GLuint& texture,const cv::Mat& pixels,bool rgba){
    if(!texture)glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glTexImage2D(GL_TEXTURE_2D,0,rgba?GL_RGBA:GL_RGB,pixels.cols,pixels.rows,0,rgba?GL_RGBA:GL_BGR,GL_UNSIGNED_BYTE,pixels.data);
}
// Rebuilds the display image, the mask fill overlay and the cached contours of one frame.
const char* const Application::viewNames[Application::viewCount]={"원본","대비","흑백","에지","그래디언트","에지 겹침"};
namespace {
// The edges the circle refinement works from: a 3x3 blur and Canny 20/60.
cv::Mat cannyEdges(const cv::Mat& bgr){cv::Mat gray,edges;cv::cvtColor(bgr,gray,cv::COLOR_BGR2GRAY);cv::GaussianBlur(gray,gray,{3,3},0);cv::Canny(gray,edges,20,60,3,true);return edges;}
}
// The frame as the chosen view mode shows it. The brightness gain applies to the original and the gray view.
cv::Mat Application::displayImage(const FrameView& f) const {
    const double gain=f.member.value("kind","")=="컬러"?brightness_:1.12;
    cv::Mat display;
    switch(view_){
    case 1:{cv::Mat lab;cv::cvtColor(f.image,lab,cv::COLOR_BGR2Lab);std::vector<cv::Mat> channels;cv::split(lab,channels);cv::createCLAHE(3,{8,8})->apply(channels[0],channels[0]);cv::merge(channels,lab);cv::cvtColor(lab,display,cv::COLOR_Lab2BGR);break;}   // contrast
    case 2:{cv::Mat gray;cv::cvtColor(f.image,gray,cv::COLOR_BGR2GRAY);gray.convertTo(gray,CV_8UC1,gain);cv::cvtColor(gray,display,cv::COLOR_GRAY2BGR);break;}   // gray
    case 3:cv::cvtColor(cannyEdges(f.image),display,cv::COLOR_GRAY2BGR);break;   // edges
    case 4:{cv::Mat gray,gx,gy,magnitude;cv::cvtColor(f.image,gray,cv::COLOR_BGR2GRAY);cv::GaussianBlur(gray,gray,{3,3},0);cv::Sobel(gray,gx,CV_32F,1,0,3);cv::Sobel(gray,gy,CV_32F,0,1,3);cv::magnitude(gx,gy,magnitude);cv::normalize(magnitude,magnitude,0,255,cv::NORM_MINMAX);magnitude.convertTo(gray,CV_8UC1);cv::cvtColor(gray,display,cv::COLOR_GRAY2BGR);break;}   // gradient
    case 5:{f.image.convertTo(display,CV_8UC3,gain);display.setTo(cv::Scalar(255,0,255),cannyEdges(f.image));break;}   // original with the edges on top
    default:f.image.convertTo(display,CV_8UC3,gain);break;
    }
    return display;
}
void Application::repaint(FrameView& f){
    cv::Mat display=displayImage(f);uploadTexture(f.texture,display);
    cv::Mat overlay(f.image.size(),CV_8UC4,cv::Scalar(0,0,0,0));
    const bool active=!frames_.empty()&&&f==&current();
    const auto& boxes=f.annotation["boxes"];f.contours.assign(boxes.size(),{});f.circles.assign(boxes.size(),{});
    for(int i=0;i<int(boxes.size());++i){
        if(!boxes[i].contains("segmentation")){const auto r=detectionBox(boxes[i]);f.circles[i]={r.x+r.width/2,r.y+r.height/2,(r.width+r.height)/4};continue;}   // box only: the inscribed circle
        auto mask=decodeMask(boxes[i]["segmentation"],f.image.cols,f.image.rows);
        cv::findContours(mask.clone(),f.contours[i],cv::RETR_EXTERNAL,cv::CHAIN_APPROX_SIMPLE);
        std::vector<cv::Point> edge;for(const auto& contour:f.contours[i])edge.insert(edge.end(),contour.begin(),contour.end());
        // The circle shown is the one measured from the image (Canny + RANSAC): the stored one for a refined ball, measured
        // on the spot for the others, and only when the image gives no trustworthy circle a fit to the mask's outline.
        if(boxes[i].contains("circle")){const auto& c=boxes[i]["circle"];f.circles[i]={c[0].get<double>()-.5,c[1].get<double>()-.5,c[2].get<double>()};}
        else{const CircleFit fit=refineCircle(f.image,mask);f.circles[i]=fit.ok?Circle{fit.x-.5,fit.y-.5,fit.r+circleMargin_}:boxes[i].value("occluded",false)?Circle{}:fitCircle(edge);}   // a hidden ball shows no guessed circle
        if(active&&replacement_==i&&!proposal_.is_null())continue;
        overlay.setTo(active&&editor_&&i==selected_?cv::Scalar(255,216,74,13):cv::Scalar(46,230,208,51),mask);
    }
    if(active){
        proposalContours_.clear();
        // Only a brush stroke is filled, so painting gives feedback; AI and polygon proposals show their outline only.
        if(!brushMask_.empty()){overlay.setTo(cv::Scalar(255,90,217,70),brushMask_);cv::findContours(brushMask_.clone(),proposalContours_,cv::RETR_EXTERNAL,cv::CHAIN_APPROX_SIMPLE);}
        else if(!proposal_.is_null()&&proposal_.contains("segmentation")){const Json shown=chosenProposal();auto mask=decodeMask(shown["segmentation"],f.image.cols,f.image.rows);cv::findContours(mask,proposalContours_,cv::RETR_EXTERNAL,cv::CHAIN_APPROX_SIMPLE);}
    }
    uploadTexture(f.overlay,overlay,true);f.repaint=false;
}
// The ball a new outline at this point changes, or -1 when it adds a ball.
// The ball whose circle holds the point, the nearest centre first: with two balls overlapping, a click on the one behind
// must not pick the one in front just because its box is wider (2026-10-08 user report).
int Application::target(const FrameView& frame,cv::Point2d point)const {
    const auto& boxes=frame.annotation["boxes"];int best=-1;double bestDistance=0;
    for(int i=0;i<int(boxes.size());++i){
        const auto r=detectionBox(boxes[i]);double cx=r.x+r.width/2,cy=r.y+r.height/2,radius=std::min(r.width,r.height)/2;
        if(boxes[i].contains("circle")){cx=boxes[i]["circle"][0].get<double>();cy=boxes[i]["circle"][1].get<double>();radius=boxes[i]["circle"][2].get<double>();}
        const double distance=std::hypot(point.x-cx,point.y-cy);
        if(distance<=radius&&(best<0||distance<bestDistance)){best=i;bestDistance=distance;}
    }
    return best;
}
int Application::hit(const FrameView& frame,cv::Point2d point)const {for(int i=int(frame.annotation["boxes"].size())-1;i>=0;--i)if(detectionBox(frame.annotation["boxes"][i]).contains(point))return i;return -1;}
// scale 0 means automatic: zoom to the selected ball, otherwise fit the whole image.
// Zooms around the middle of the canvas, keeping the image point there in place.
void Application::zoomBy(FrameView& f,float factor){
    if(f.scale<=0||canvasSize_.x<=0)return;
    const float updated=std::clamp(f.scale*factor,.1f,24.f);const ImVec2 pivot{canvasSize_.x/2,canvasSize_.y/2};
    const float px=(pivot.x-f.offsetX)/f.scale,py=(pivot.y-f.offsetY)/f.scale;
    f.offsetX=pivot.x-px*updated;f.offsetY=pivot.y-py*updated;f.scale=updated;
}
void Application::fit(FrameView& f,ImVec2 size,bool toBall){
    const auto& boxes=f.annotation["boxes"];
    const View view=toBall&&selected_>=0&&selected_<int(boxes.size())?ballView(detectionBox(boxes[selected_]),f.image.cols,f.image.rows,size.x,size.y):fitView(f.image.cols,f.image.rows,size.x,size.y);
    f.scale=view.scale;f.offsetX=view.x;f.offsetY=view.y;
}
void Application::proposeForCapture(){
    if(frames_.empty()||!editor_||selected_<0||selected_>=int(current().annotation["boxes"].size()))return;
    const auto r=detectionBox(current().annotation["boxes"][selected_]);
    polygon_={{r.x,r.y},{r.x+r.width,r.y},{r.x+r.width,r.y+r.height},{r.x,r.y+r.height}};tool_=Tool::Polygon;polygonProposal();
}
void Application::selectForCapture(int camera,int rank){if(frames_.empty()||camera<0||camera>=int(frames_.size()))return;const auto order=sorted(frames_[camera]);if(rank>=0&&rank<int(order.size()))select(camera,order[rank]);}
// One step per call while the app is idle. Returns false when every step has passed; throws on a failed expectation.
bool Application::selfCheckStep(int step) {
    auto expect=[](bool condition,const char* what){if(!condition)throw std::runtime_error(std::string("self-test: ")+what);};
    static int checkedScene=-1,advanced=-1;static Json before;static BallRef was;
    auto& io=ImGui::GetIO();
    const auto first=[this]{return sorted(frames_.at(0)).front();};
    const auto moved=[this]{return camera_!=was.camera||selected_!=was.index;};
    switch(step){
    case 0:{expect(!sorted(frames_.at(0)).empty(),"scene has no ball in the first camera");checkedScene=scene_;select(0,first());return true;}
    case 1:{expect(editor_&&camera_==0&&selected_>=0,"ball was not selected");expect(current().scale>2,"selected ball was not zoomed");before=current().annotation["boxes"][selected_];
        // Leaving the scene right after an edit, before the autosave delay has passed, must save the edit first.
        nudge(0,true);if(guard())nextUnreviewed();return true;}
    case 2:{expect(scene_!=checkedScene,"N did not move to the next unreviewed scene");
        expect(catalog_["scenes"][checkedScene]["members"][0].value("status","")=="editing","an edit made just before moving on was not saved");
        navigate(checkedScene);return true;}
    case 3:select(0,first());return true;
    case 4:{const auto& box=current().annotation["boxes"][selected_];const auto old=detectionBox(before),now=detectionBox(box);
        expect(now.y==old.y-1&&now.height==old.height+1&&now.x==old.x&&now.width==old.width,"top edge did not grow by one pixel");
        // The edited box defines the ball: the circle inscribed in it, and the mask as that disc, whose bounds are the bbox.
        const auto c=box.value("circle",Json::array({0,0,0}));const auto inside=decodeMask(box.at("segmentation"),current().image.cols,current().image.rows);const auto bounds=cv::boundingRect(inside);
        expect(box.value("circle_source","")=="box"&&std::abs(c[0].get<double>()-(now.x+now.width/2))<1e-3&&std::abs(c[1].get<double>()-(now.y+now.height/2))<1e-3&&std::abs(c[2].get<double>()-std::min(now.width,now.height)/2)<1e-3,"box correction did not inscribe the circle");
        expect(box.at("bbox")==Json({bounds.x,bounds.y,bounds.width,bounds.height})&&std::abs(cv::countNonZero(inside)-3.14159*c[2].get<double>()*c[2].get<double>())<=std::max(8.0,.1*cv::countNonZero(inside)),"box correction did not fill the inscribed disc");
        expect(current().member.value("status","")=="editing","an edited image must leave the confirmed state");nudge(0,false);return true;}
    case 5:{expect(detectionBox(current().annotation["boxes"][selected_])==detectionBox(before),"shrinking did not restore the box");
        const Json boxes=current().annotation["boxes"];
        removeSelected();expect(current().annotation["boxes"].size()+1==boxes.size()&&selected_<0,"delete did not remove the selected ball");
        undo();expect(current().annotation["boxes"]==boxes,"undo did not bring the deleted ball back");return true;}
    case 6:{const Json other=frames_.at(1).annotation["boxes"];const int target=sorted(frames_.at(1)).front();
        // A crop of another camera can delete its ball; the edit then belongs to that camera and can be undone.
        removeBall(1,target);expect(camera_==1&&current().annotation["boxes"].size()+1==other.size(),"deleting from another camera did not remove its ball");
        undo();expect(current().annotation["boxes"]==other,"undo did not bring back the ball of the other camera");return true;}
    case 7:{select(0,first());const auto r=detectionBox(current().annotation["boxes"][selected_]);
        // An outline traced around the ball starts outside its box; it must still change that ball.
        tool_=Tool::Polygon;polygon_={{r.x-3,r.y-3},{r.x+r.width+3,r.y-3},{r.x+r.width+3,r.y+r.height+3},{r.x-3,r.y+r.height+3}};apply();return true;}
    case 8:{expect(!proposal_.is_null()&&replacement_==first(),"an outline traced around a ball must change that ball");cancel();
        // An outline of something beside the ball may start inside the ball's box; it must add a ball, not overwrite this one.
        const auto r=detectionBox(current().annotation["boxes"][first()]);
        polygon_={{r.x+1,r.y+1},{r.x-60,r.y+1},{r.x-60,r.y-40},{r.x+1,r.y-40}};apply();return true;}
    case 9:expect(!proposal_.is_null()&&replacement_<0,"an outline that only starts inside a ball's box must add a ball");cancel();tool_=Tool::Select;return true;
    case 10:{select(0,first());was={camera_,selected_};cycle(1);expect(moved(),"cycling did not move to another ball");
        // A click gives the review pane the keyboard, as it has in real use, before the real Tab key is pressed twice.
        const float width=ImGui::GetMainViewport()->WorkSize.x;io.AddMousePosEvent((width<1500?232.f:300.f)+150,70);io.AddMouseButtonEvent(0,true);return true;}
    case 11:io.AddMouseButtonEvent(0,false);return true;
    case 12:was={camera_,selected_};io.AddKeyEvent(ImGuiKey_Tab,true);return true;
    case 13:io.AddKeyEvent(ImGuiKey_Tab,false);expect(moved(),"the Tab key did not move to the next ball");return true;
    case 14:was={camera_,selected_};io.AddKeyEvent(ImGuiKey_Tab,true);return true;
    case 15:io.AddKeyEvent(ImGuiKey_Tab,false);expect(moved()&&!io.WantTextInput,"Tab put the keyboard into a widget, which turns the shortcuts off");return true;
    case 16:review(Scope::Scene,false);return true;
    case 17:{expect(nextAsk_&&scene_==checkedScene,"confirm did not stop at the move-on prompt");nextAsk_=false;advanced=advanceIndex();advance();return true;}   // the second Space
    case 18:{for(const auto& member:catalog_["scenes"][checkedScene]["members"])expect(!member.value("selected",true)||member.value("reviewed",false),"scene confirm left an image unconfirmed");
        expect(advanced>checkedScene&&scene_==advanced&&!editor_,"the second Space did not advance to the very next scene");
        expect(aheadHits_>0,"the next unreviewed scene was not fetched ahead");return true;}
    case 19:{changed();save({},dirtySlot());review(Scope::Scene,false);expect(busy()&&queued_!=nullptr,"a confirm during a save was dropped");return true;}   // Space while saving
    case 20:{expect(!queued_&&nextAsk_,"the queued confirm did not run after the save");nextAsk_=false;return false;}
    }
    return false;
}
// The split and export from the command line: "8:test,9:test,7:val" assigns those recordings by number, the rest train;
// then the confirmation, the export without crops, and the export folder on stdout. Returns false when done.
bool Application::batchSplitStep(int step,const std::string& spec){
    auto fail=[](const std::string& what){throw std::runtime_error("batch-split: "+what);};
    switch(step){
    case 0:{
        std::map<int,std::string> wanted;
        for(size_t at=0;at<spec.size();){size_t comma=spec.find(',',at);if(comma==std::string::npos)comma=spec.size();const std::string item=spec.substr(at,comma-at);at=comma+1;
            const size_t colon=item.find(':');if(colon==std::string::npos)fail("use number:split, as in 8:test");
            const std::string split=item.substr(colon+1);if(split!="train"&&split!="val"&&split!="test")fail("split must be train, val or test: "+split);
            wanted[std::stoi(item.substr(0,colon))]=split;}
        for(auto& [id,group]:groups_.items())group=id;   // by recording, no batches: the assignment is explicit
        for(const auto& recording:catalog_["recordings"]){const auto id=recording.at("id").get<std::string>();const int number=catalog_["stats"].value(id,Json::object()).value("number",0);
            const auto found=wanted.find(number);assignSplit(id,found==wanted.end()?"train":found->second);}
        std::cout<<"batch-split: assigned";for(const auto& [id,split]:splits_.items())std::cout<<' '<<catalog_["stats"].value(id,Json::object()).value("number",0)<<':'<<split.get<std::string>();std::cout<<'\n';
        confirmSplit();return true;}   // not through the split page: opening it would start the engine's own plan and leave the app busy
    case 1:{if(!splitConfirmed()||dataVersion().size()!=64)fail("the split was not confirmed: "+message_);
        std::cout<<"batch-split: confirmed, data version "<<dataVersion()<<'\n';crops_=false;startExport();return true;}
    default:{
        const auto status=exportStatus_.value("status","");
        if(status=="running")return true;
        if(status!="complete")fail("the export did not finish: "+exportStatus_.dump());
        if(exports_.empty()||exports_[0].value("fingerprint",Json()).is_null()||exports_[0]["fingerprint"]!=dataVersion())return true;   // the history refreshes after the poll
        std::cout<<"batch-split: exported to "<<exports_[0].value("path","")<<" · images "<<exports_[0].value("images",exports_[0].value("frames",0))<<'\n';
        return false;}
    }
}
// The same kind of scripted run for the split and export pages. Returns false when every step has passed.
bool Application::stageCheckStep(int step) {
    auto expect=[](bool condition,const char* what){if(!condition)throw std::runtime_error(std::string("stage-test: ")+what);};
    const auto planned=[this]{return !splits_.empty()&&std::none_of(splits_.begin(),splits_.end(),[](const Json& split){return split.get<std::string>()=="unassigned";});};
    switch(step){
    case 0:openPage(Page::Split);return true;
    case 1:{expect(page_==Page::Split&&planned(),"the split page did not open with a plan");expect(splits_.size()>=3,"the stage test needs three recordings");
        auto entry=splits_.begin();const std::string first=entry.key(),second=(++entry).key();
        picked_={second,first};mergePicked();
        expect(groups_[first]==groups_[second]&&splits_[first]==splits_[second],"merged recordings must share a group and a split");
        leaveGroup(groups_,second);expect(groups_[first]!=groups_[second],"a recording that left its group is still grouped");
        proposeSplit();return true;}
    case 2:expect(planned(),"no plan after regrouping");confirmSplit();return true;
    case 3:expect(page_==Page::TrainExport&&dataVersion().size()==64,"confirming did not open the export page with a data version");crops_=false;startExport();return true;
    default:{
        const auto status=exportStatus_.value("status","");
        if(status=="running")return true;
        expect(status=="complete","the export did not finish");
        expect(!exports_.empty()&&exports_[0].value("fingerprint",Json()).is_string()&&exports_[0]["fingerprint"]==dataVersion(),"the export history does not show the finished export");
        expect(std::filesystem::exists(fromUtf8(exports_[0].value("path",""))/"data.yaml"),"the exported folder has no data.yaml");
        // A plan changed after the confirm must not be exported as if it were the confirmed one.
        const std::string first=splits_.begin().key();assignSplit(first,splits_[first]=="train"?"val":"train");
        expect(!splitConfirmed()&&dataVersion().empty(),"a changed plan still counts as confirmed");
        startExport();expect(page_==Page::Split&&exportStatus_.value("status","")=="complete","a changed plan could be exported without confirming it");
        return false;}
    }
}
// The stage pages show per-recording counts that only the full catalog carries.
void Application::refreshStage(){
    if(busy())return;const auto engine=engine_;
    launch("현황 새로 읽기",[engine]{return JobResult{{{"catalog",engine->nativeCatalog(false)},{"exports",engine->exports()},{"export",engine->exportStatus()},{"auto",engine->autoStatus()},{"color",engine->colorFillStatus()},{"circle",engine->circleRefineStatus()},{"missing",engine->missingFillStatus()},{"propagation",engine->propagationStatus()},{"occlusion",engine->occlusionStatus()}},{}};},
        [this](JobResult result){
            const auto revision=catalog_["summary"].value("revision",0),automatic=catalog_["summary"].value("auto_revision",0);
            missingStatus_=result.data["missing"];propagationStatus_=result.data["propagation"];occlusionStatus_=result.data["occlusion"];
            updateCatalog(result.data["catalog"]);
            if(revision!=catalog_["summary"].value("revision",0)||automatic!=catalog_["summary"].value("auto_revision",0))stale_=true;
            exports_=result.data["exports"].value("exports",Json::array());exportStatus_=result.data["export"];autoStatus_=result.data["auto"];colorStatus_=result.data["color"];circleStatus_=result.data["circle"];
            // Nothing assigned yet: show the plan the engine would make, so the split page never opens empty.
            const bool unassigned=std::all_of(splits_.begin(),splits_.end(),[](const Json& split){return split.get<std::string>()=="unassigned";});
            if(page_==Page::Split&&unassigned&&!splits_.empty())proposeSplit();
        });
}
void Application::keys() {
    auto& io=ImGui::GetIO();
    if(page_!=Page::Review||io.WantTextInput||frames_.empty()||createProjectWanted_||importPreviewWanted_||settingsWanted_||nextAsk_||missingAsk_||occludeAsk_)return;   // an open dialog owns the keys
    auto pressed=[](ImGuiKey key){return ImGui::IsKeyPressed(key,false);};
    if(pressed(ImGuiKey_Escape)){
        if(pending())cancel();
        else if(editor_&&selected_>=0){selected_=-1;current().scale=0;current().repaint=true;}
        else if(editor_&&guard()){if(current().dirty)save([this]{editor_=false;});else editor_=false;}
    }
    if(pressed(ImGuiKey_Enter)||pressed(ImGuiKey_KeypadEnter))apply();
    if(io.KeyCtrl&&pressed(ImGuiKey_Z))undo();
    if(io.KeyCtrl&&pressed(ImGuiKey_S))save();
    if(io.KeyCtrl||io.KeyAlt)return;
    // Space applies a pending proposal, otherwise confirms the scene (2026-10-08 user request); E confirms only.
    if(pressed(ImGuiKey_Space)){if(pending())apply();else review(io.KeyShift&&editor_?Scope::Camera:Scope::Scene,false);}
    if(pressed(ImGuiKey_E))review(io.KeyShift&&editor_?Scope::Camera:Scope::Scene,false);
    if(pressed(ImGuiKey_Q))review(editor_?Scope::Camera:Scope::Scene,true);
    if(pressed(ImGuiKey_N)&&guard())nextUnreviewed();
    if(pressed(ImGuiKey_F1))applyPreviousLabels();   // 이전 라벨 가져오기 (2026-10-20 user choice: F1)
    if(pressed(ImGuiKey_Backspace))back();
    if(pressed(ImGuiKey_A))stepRow(-1);
    if(pressed(ImGuiKey_D))stepRow(1);
    if(pressed(ImGuiKey_1))focus(0);
    if(pressed(ImGuiKey_2))focus(1);
    if(pressed(ImGuiKey_3))focus(2);
    if(pressed(ImGuiKey_Tab))cycle(io.KeyShift?-1:1);
    static const ImGuiKey arrows[]={ImGuiKey_UpArrow,ImGuiKey_DownArrow,ImGuiKey_LeftArrow,ImGuiKey_RightArrow};
    for(int edge=0;edge<4;++edge)if(ImGui::IsKeyPressed(arrows[edge],true))nudge(edge,!io.KeyShift);
    if(pressed(ImGuiKey_M)){masks_=!masks_;}
    if(pressed(ImGuiKey_X))boxes_=!boxes_;
    if(pressed(ImGuiKey_C))circles_=!circles_;
    if(pressed(ImGuiKey_G))outlines_=!outlines_;
    if(pressed(ImGuiKey_I)){view_=(view_+1)%viewCount;for(auto& view:frames_)view.repaint=true;}
    if(!editor_||pending()||busy())return;
    if(pressed(ImGuiKey_V))tool_=Tool::Select;
    if(pressed(ImGuiKey_S))tool_=Tool::Ai;
    if(pressed(ImGuiKey_B))tool_=Tool::Brush;
    if(pressed(ImGuiKey_P))tool_=Tool::Polygon;
    if(pressed(ImGuiKey_R))tool_=Tool::Box;
    if(pressed(ImGuiKey_F)){selected_=-1;current().scale=0;current().repaint=true;}
    if(pressed(ImGuiKey_Z))current().scale=0;
    if(editor_&&(pressed(ImGuiKey_Equal)||pressed(ImGuiKey_KeypadAdd)))zoomBy(current(),1.25f);      // + and - zoom the large view (2026-10-08 user request)
    if(editor_&&(pressed(ImGuiKey_Minus)||pressed(ImGuiKey_KeypadSubtract)))zoomBy(current(),.8f);
    if(pressed(ImGuiKey_LeftBracket))brushSize_=std::max(2.f,brushSize_-1);
    if(pressed(ImGuiKey_RightBracket))brushSize_=std::min(30.f,brushSize_+1);
    auto& objects=current().annotation["boxes"];
    if(selected_<0||selected_>=int(objects.size()))return;
    if(pressed(ImGuiKey_O)){checkpoint();setOccludedFlag(objects[selected_],!objects[selected_].value("occluded",false));changed();}
    if(pressed(ImGuiKey_Delete))removeSelected();
}
// The mark and name, the project button, the tabs, then progress, save state and the shortcut list (design/labeler-v3).
void Application::topBar() {
    const float width=ImGui::GetContentRegionAvail().x,height=48;const ImVec2 origin=ImGui::GetCursorScreenPos();
    ImGui::Dummy({width,height});const ImVec2 after=ImGui::GetCursorScreenPos();auto draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,{origin.x+width,origin.y+height},theme::bg1);
    draw->AddLine({origin.x,origin.y+height-.5f},{origin.x+width,origin.y+height-.5f},theme::line);
    const ImVec2 mark{origin.x+21,origin.y+24};   // a ring around a dot, as on the program icon
    draw->AddCircleFilled(mark,7,theme::accent);draw->AddCircle(mark,5.1f,theme::bg1,0,2.2f);
    const char* app=config_.training?"Boccia Trainer":"Boccia Labeler";
    theme::write(draw,{origin.x+35,origin.y+24-7},theme::text,app,14,theme::bold);
    float x=origin.x+35+theme::extent(app,14,theme::bold).x+14;
    if(!projectOpened()){ImGui::SetCursorScreenPos(after);return;}
    draw->AddLine({x+.5f,origin.y+13},{x+.5f,origin.y+35},theme::line2);x+=14;
    ImGui::SetCursorScreenPos({x,origin.y+9});x+=projectControls()+18;
    struct Tab{Page page;const char* name;};
    static const Tab labeler[]={{Page::Data,"자료"},{Page::Review,"검수"},{Page::Export,"내보내기"}};
    static const Tab trainer[]={{Page::Split,"데이터 배정"},{Page::TrainExport,"데이터 내보내기"},{Page::Train,"모델 학습"}};
    const Tab* tabs=config_.training?trainer:labeler;
    for(int i=0;i<3;++i){
        const Tab& tab=tabs[i];const bool enabled=true,on=page_==tab.page;
        const float w=theme::extent(tab.name,theme::base,on?theme::bold:nullptr).x+26;
        ImGui::SetCursorScreenPos({x,origin.y});ImGui::PushID(i);ImGui::BeginDisabled(!enabled);
        if(ImGui::InvisibleButton("##tab",{w,height})&&!on)openPage(tab.page);
        const bool hovered=enabled&&ImGui::IsItemHovered();ImGui::EndDisabled();ImGui::PopID();
        theme::write(draw,{x+13,origin.y+24-theme::base/2},!enabled?theme::line2:on||hovered?theme::text:theme::text2,tab.name,theme::base,on?theme::bold:nullptr);
        if(on)draw->AddRectFilled({x+9,origin.y+height-2},{x+w-9,origin.y+height},theme::accent);
        x+=w;
    }
    const int target=catalog_.is_object()?catalog_.value("target_images",0):0,done=catalog_.is_object()?catalog_.value("reviewed_target_images",0):0;
    float right=origin.x+width-14;
    const float help=theme::extent("단축키",theme::caption).x+40;right-=help;
    const ImVec2 helpAt{right,origin.y+10};
    ImGui::SetCursorScreenPos(helpAt);
    const bool helpClicked=ImGui::InvisibleButton("##shortcutsButton",{help,28}),helpHovered=ImGui::IsItemHovered();
    if(helpClicked||shortcutsWanted_){ImGui::OpenPopup("##shortcuts");shortcutsWanted_=false;}
    draw->AddRectFilled(helpAt,{helpAt.x+help,helpAt.y+28},helpHovered?theme::bg3:theme::bg1,6);draw->AddRect(helpAt,{helpAt.x+help,helpAt.y+28},theme::line,6);
    theme::icon(draw,theme::Icon::Help,{helpAt.x+16,helpAt.y+14},14,theme::text2);
    theme::write(draw,{helpAt.x+28,helpAt.y+14-theme::caption/2},theme::text2,"단축키",theme::caption);
    shortcutsPopup({right+help,origin.y+height});
    const bool dirty=!frames_.empty()&&current().dirty;
    const std::string saved=busy()?busyLabel_:dirty?"저장 대기":"저장됨";
    right-=20+theme::extent(saved,theme::caption).x;theme::write(draw,{right,origin.y+24-theme::caption/2},busy()||dirty?theme::hold:theme::text3,saved,theme::caption);
    draw->AddCircleFilled({right-9,origin.y+24},3,busy()||dirty?theme::hold:theme::ok);
    const std::string count=grouped(done)+" / "+grouped(target);
    right-=34+120;draw->AddRectFilled({right,origin.y+22},{right+120,origin.y+26},theme::bg4,2);
    if(target&&done)draw->AddRectFilled({right,origin.y+22},{right+std::max(2.f,120.f*done/target),origin.y+26},theme::ok,2);
    right-=10+theme::extent(count,theme::caption,theme::mono).x;theme::write(draw,{right,origin.y+24-theme::caption/2},theme::text,count,theme::caption,theme::mono);
    right-=8+theme::extent("검수",theme::caption).x;theme::write(draw,{right,origin.y+24-theme::caption/2},theme::text2,"검수",theme::caption);
    ImGui::SetCursorScreenPos(after);
}
// The key list lives here instead of in hint lines all over the screens.
void Application::shortcutsPopup(ImVec2 below) {
    static const char* const rows[][2]={{"Space / E","세 장 확인 완료 · 팝업에서 Space 한 번 더 누르면 다음 장면"},{"Shift+Space / Shift+E","크게 보는 카메라 한 장만 확인"},{"Q","보류"},{"N","다음 미검수 장면"},{"F1","이전 라벨 가져오기"},
        {"A / D","이전 / 다음 장면"},{"Backspace","이전에 보던 장면으로"},{"1 / 2 / 3","그 카메라를 크게"},{"Tab / Shift+Tab","다음 / 이전 공"},
        {"방향키","선택한 공의 그 방향 경계를 1픽셀 바깥으로"},{"Shift+방향키","1픽셀 안으로"},{"V / S / B / P / R","선택 / AI 분할 / 브러시 / 폴리곤 / 박스"},
        {"왼쪽 / 오른쪽 클릭","더하기 / 빼기"},{"[ / ]","브러시 굵기"},{"Space / Esc","윤곽 반영 / 취소, 선택 해제, 세 장 보기"},{"G / M / X / C","윤곽 / 마스크 채움 / 박스 / 원 표시"},{"I","보기: 원본 / 대비 / 흑백 / 에지 / 그래디언트 / 에지 겹침"},
        {"F / Z","화면 맞춤 / 선택한 공으로"},{"+ / -","확대 / 축소 (휠도 됨)"},{"도구 막대 ○","원 맞춤 켜고 끄기 (끄면 분할 윤곽 그대로)"},{"O / Delete","일부 가림 / 공 삭제 (바로, Ctrl+Z로 되돌림)"},{"Ctrl+Z / Ctrl+S","되돌리기 / 저장"},{"휠 / 가운데 끌기","확대 / 이동"}};
    ImGui::SetNextWindowPos({below.x,below.y+4},ImGuiCond_Always,{1,0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{16,12});
    if(ImGui::BeginPopup("##shortcuts")){
        auto draw=ImGui::GetWindowDrawList();const ImVec2 origin=ImGui::GetCursorScreenPos();
        float keyWidth=0;for(const auto& row:rows)keyWidth=std::max(keyWidth,theme::extent(row[0],theme::caption,theme::mono).x);
        float y=origin.y;
        for(const auto& row:rows){
            theme::write(draw,{origin.x,y},theme::text,row[0],theme::caption,theme::mono);
            theme::write(draw,{origin.x+keyWidth+18,y},theme::text2,row[1],theme::caption);
            y+=22;
        }
        ImGui::Dummy({keyWidth+18+300,y-origin.y});
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}
// A folded panel: a thin strip whose button opens it again. Returns true when clicked.
bool Application::collapsedStrip(const char* id, bool& open, bool pointsRight, float height) {
    const ImVec2 origin=ImGui::GetCursorScreenPos();auto draw=ImGui::GetWindowDrawList();
    ImGui::Dummy({26,height});
    draw->AddLine({pointsRight?origin.x+25.5f:origin.x+.5f,origin.y},{pointsRight?origin.x+25.5f:origin.x+.5f,origin.y+height},theme::line);
    ImGui::SetCursorScreenPos({origin.x+2,origin.y+8});
    const bool clicked=ImGui::InvisibleButton(id,{22,26});
    if(ImGui::IsItemHovered())draw->AddRectFilled({origin.x+2,origin.y+8},{origin.x+24,origin.y+34},theme::bg3,4);
    theme::icon(draw,pointsRight?theme::Icon::Right:theme::Icon::Left,{origin.x+13,origin.y+21},11,theme::text2);
    if(clicked)open=true;
    return clicked;
}
// The error floats below the top bar instead of taking a row: a row pushed the whole page down and up again, so clicks
// landed on the wrong thing (2026-10-09 user report). Once the data is connected it goes by itself after eight seconds.
void Application::errorOverlay() {
    if(error_.empty()){errorShownText_.clear();return;}
    if(error_!=errorShownText_){errorShownText_=error_;errorShown_=Clock::now();}
    if(ready()&&Clock::now()-errorShown_>std::chrono::seconds(8)){error_.clear();errorShownText_.clear();return;}
    const auto viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x+viewport->WorkSize.x/2,viewport->WorkPos.y+48},ImGuiCond_Always,{.5f,0});
    ImGui::SetNextWindowSizeConstraints({0,0},{viewport->WorkSize.x-40,FLT_MAX});
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImGui::ColorConvertU32ToFloat4(theme::rgb(0x2a1d16,245)));ImGui::PushStyleColor(ImGuiCol_Border,ImGui::ColorConvertU32ToFloat4(theme::warn));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,8);ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,1);ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{14,6});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8,4});
    if(ImGui::Begin("##error",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNav)){
        const ImVec2 at=ImGui::GetCursorScreenPos();
        theme::warning(ImGui::GetWindowDrawList(),{at.x+6,at.y+13},13,theme::warn);
        ImGui::Dummy({18,26});ImGui::SameLine();
        ImGui::AlignTextToFramePadding();ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::warn),"%s",error_.c_str());ImGui::SameLine();
        if(ImGui::Button("닫기",{60,26}))error_.clear();
        if(!ready()&&!config_.source.empty()){ImGui::SameLine();if(ImGui::Button("다시 연결",{110,26})){if(!busy()&&(frames_.empty()||!current().dirty))connect();else message_="현재 변경을 저장하거나 recovery 파일을 보관한 뒤 다시 실행하세요.";}}
    }
    ImGui::End();
    ImGui::PopStyleVar(4);ImGui::PopStyleColor(2);
}
void Application::statusBar() {
    const float width=ImGui::GetContentRegionAvail().x;const ImVec2 origin=ImGui::GetCursorScreenPos();
    ImGui::Dummy({width,28});auto draw=ImGui::GetWindowDrawList();
    draw->AddLine(origin,{origin.x+width,origin.y},theme::line);
    // A notice the hub sent stays at the left, with × to close it, until the next one replaces it (design/hub).
    if(!hubNotice_.empty()){
        const float noticeWidth=theme::extent(hubNotice_,theme::caption).x;
        theme::write(draw,{origin.x+14,origin.y+14-theme::caption/2},theme::accent,hubNotice_,theme::caption);
        ImGui::SetCursorScreenPos({origin.x+14+noticeWidth+6,origin.y+4});
        if(ImGui::InvisibleButton("##hubNoticeClose",{22,20}))hubNotice_.clear();
        theme::write(draw,{origin.x+14+noticeWidth+12,origin.y+14-theme::caption/2},ImGui::IsItemHovered()?theme::text:theme::text3,"×",theme::caption);
    }
    else theme::write(draw,{origin.x+14,origin.y+14-theme::caption/2},busy()?theme::hold:theme::text3,busy()?busyLabel_:message_,theme::caption);
    // Where the project lives; a long path keeps its end, which names the project. Nothing before a project is open.
    std::string where=projectOpened()?utf8(workspace_.empty()?config_.project:workspace_):std::string();
    const float room=width*.45f;
    if(theme::extent(where,theme::caption).x>room)for(size_t start=1;start<where.size();++start){
        if((where[start]&0xC0)==0x80)continue;
        if(const auto tail="…"+where.substr(start);theme::extent(tail,theme::caption).x<=room){where=tail;break;}
    }
    const float textWidth=where.empty()?0:theme::extent(where,theme::caption).x,x=origin.x+width-14-textWidth;
    // The hub, left of the folder: a green dot while connected, an orange one while the invite code is refused, nothing
    // while off. A click opens the worker dialog.
    float hubWidth=0;
    if(hub_&&hub_->state()!=hub::Client::State::Offline){
        const bool joined=hub_->state()==hub::Client::State::Connected;const std::string word=joined?"허브":"초대 코드 확인";
        const float wordWidth=theme::extent(word,theme::caption).x,dotX=where.empty()?x-6:x-12-16;hubWidth=wordWidth+32;
        draw->AddCircleFilled({dotX,origin.y+14},4,joined?theme::ok:theme::warn);
        theme::write(draw,{dotX-8-wordWidth,origin.y+14-theme::caption/2},theme::text3,word,theme::caption);
        ImGui::SetCursorScreenPos({dotX-8-wordWidth,origin.y+4});
        if(ImGui::InvisibleButton("##hubState",{wordWidth+16,20}))later_=[this]{openWorkerDialog();};
        if(ImGui::IsItemHovered()&&!joined)ImGui::SetTooltip("%s",hub_->lastError().c_str());
    }
    if(projectOpened()){
        struct Running{const Json* state;std::string label;const char* stop;};
        const Running jobs[]={{&redraftStatus_,"초안 다시 만들기 · "+redraftStatus_.value("stage",""),"stop-redraft"},{&missingStatus_,"빠진 공 찾기","stop-missing"},{&occlusionStatus_,"가려진 공 표시","stop-occluded"}};
        for(const auto& job:jobs){
            if(job.state->value("status","")!="running")continue;
            const int done=job.state->value("done",0),total=job.state->value("total",0);
            const std::string text=job.label+(total?"  "+grouped(done)+" / "+grouped(total):"");
            float right=x-24-hubWidth;const float stopWidth=44,bar=90;
            ImGui::SetCursorScreenPos({right-stopWidth,origin.y+4});
            const bool stop=ImGui::InvisibleButton("##stopJob",{stopWidth,20}),hovered=ImGui::IsItemHovered();
            if(stop&&!busy())later_=[this,action=job.stop]{stopJob(action,"작업 중단");};
            draw->AddRectFilled({right-stopWidth,origin.y+4},{right,origin.y+24},hovered?theme::bg3:theme::bg2,5);draw->AddRect({right-stopWidth,origin.y+4},{right,origin.y+24},theme::line2,5);
            theme::write(draw,{right-stopWidth+(stopWidth-theme::extent("중단",theme::caption).x)/2,origin.y+14-theme::caption/2},theme::text,"중단",theme::caption);
            right-=stopWidth+10;
            draw->AddRectFilled({right-bar,origin.y+12},{right,origin.y+16},theme::bg4,2);
            if(total>0)draw->AddRectFilled({right-bar,origin.y+12},{right-bar+bar*std::min(1.f,float(done)/float(total)),origin.y+16},theme::accent,2);
            right-=bar+10;
            theme::write(draw,{right-theme::extent(text,theme::caption).x,origin.y+14-theme::caption/2},theme::text2,text,theme::caption);
            break;
        }
    }
    if(!where.empty()){theme::write(draw,{x,origin.y+14-theme::caption/2},theme::text3,where,theme::caption);theme::icon(draw,theme::Icon::Folder,{x-12,origin.y+14},11,theme::text3);}
}
void Application::draw(){
    deleteDoomed();   // the previous frame has been rendered; the textures it showed can go now
    const auto viewport=ImGui::GetMainViewport();ImGui::SetNextWindowPos(viewport->WorkPos);ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Boccia Labeler",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{0,0});
    topBar();
    const float body=ImGui::GetContentRegionAvail().y-28;
    if(!projectOpened()){
        ImGui::BeginChild("ProjectStart",{0,body},0,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);welcomePage();ImGui::EndChild();
    }
    else if(page_==Page::Data){
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{28,20});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8,8});
        ImGui::BeginChild("ImportPane",{0,body},ImGuiChildFlags_AlwaysUseWindowPadding);preparePage();ImGui::EndChild();ImGui::PopStyleVar(2);
    }
    else if(page_==Page::Export){
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{28,20});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8,8});
        ImGui::BeginChild("ExportPane",{0,body},ImGuiChildFlags_AlwaysUseWindowPadding);exportTab();ImGui::EndChild();ImGui::PopStyleVar(2);
    }
    else if(!ready()){
        ImGui::BeginChild("Loading",{0,body});
        theme::write(ImGui::GetWindowDrawList(),{ImGui::GetCursorScreenPos().x+28,ImGui::GetCursorScreenPos().y+24},theme::text2,message_);
        ImGui::EndChild();
    }
    else if(page_==Page::Review){
        const float width=ImGui::GetContentRegionAvail().x,list=listOpen_?(width<1500?232.f:300.f):26.f,side=sideOpen_?(width<1500?296.f:344.f):26.f;
        ImGui::BeginChild("QueuePane",{list,body});if(listOpen_)queue();else collapsedStrip("##openList",listOpen_,true,body);ImGui::EndChild();ImGui::SameLine(0,0);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,theme::bg0);
        ImGui::BeginChild("ReviewPane",{width-list-side,body},0,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);reviewPane();ImGui::EndChild();
        ImGui::PopStyleColor();ImGui::SameLine(0,0);
        ImGui::BeginChild("SidePane",{side,body});if(sideOpen_)sidePanel();else collapsedStrip("##openSide",sideOpen_,false,body);ImGui::EndChild();
        keys();
    }
    else{
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{28,20});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8,8});ImGui::PushStyleColor(ImGuiCol_ChildBg,theme::bg0);
        ImGui::BeginChild("StagePane",{0,body},ImGuiChildFlags_AlwaysUseWindowPadding);
        if(page_==Page::Split)splitPage();else if(page_==Page::TrainExport)exportPage();
        ImGui::EndChild();ImGui::PopStyleColor();ImGui::PopStyleVar(2);
    }
    statusBar();
    errorOverlay();
    projectPopups();workerPopup();
    exportPopups();
    batchToolsPopup();
    circlesPopup();
    confirmNext();
    confirmMissing();
    confirmOcclusion();
    ImGui::PopStyleVar();
    ImGui::End();
    if(later_){auto action=std::move(later_);later_=nullptr;action();}
}
}
