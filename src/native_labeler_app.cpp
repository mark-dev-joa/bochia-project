#include "native_labeler_app.hpp"
#include "native_labeler_theme.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <fstream>
#include <thread>
#include <iomanip>
#include <sstream>
#include <windows.h>

namespace boccia::labeler {
namespace {
using Clock = std::chrono::steady_clock;
std::string title(const Json& scene) { return "촬영 " + std::to_string(scene.at("recording_number").get<int>()) + " · 프레임 " + std::to_string(scene.at("index").get<int>()); }
}
Application::Application(Configuration config): config_(std::move(config)), poll_(Clock::now()) { connect(); }
Application::~Application() {
    if(task_.valid()) task_.wait(); if(thumbTask_.valid()) thumbTask_.wait(); if(ahead_.valid()) ahead_.wait();
    if(config_.rememberUi){try{rememberPosition();}catch(...){}} releaseFrames();
    for(auto& [key,id]:thumbs_)if(id)glDeleteTextures(1,&id);
}
std::filesystem::path Application::preferencesPath() const { return config_.project.parent_path()/(config_.project.stem().wstring()+L"-native-ui.json"); }
void Application::rememberPosition() const {
    if(frames_.empty())return;
    const auto& scene=catalog_["scenes"][scene_];
    Json state={{"recording",scene["recording"]},{"index",scene["index"]},{"camera",frames_[camera_].member["camera"]},{"editor",editor_},{"brightness",brightness_},{"representatives",representatives_},{"filter",filter_},{"masks",masks_},{"boxes",boxes_},{"circles",circles_},{"outlines",outlines_},{"view",view_},{"panels",{listOpen_,sideOpen_,summaryOpen_}}};
    auto path=preferencesPath();std::filesystem::create_directories(path.parent_path());
    auto temporary=path;temporary+=L"."+std::to_wstring(Clock::now().time_since_epoch().count())+L".tmp";std::ofstream stream(temporary,std::ios::binary);stream<<state.dump(2);stream.close();
    if(!stream)throw std::runtime_error("작업 위치 저장 실패");
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("작업 위치 반영 실패");
}
void Application::restorePosition() {
    int index=0,camera=0;bool editor=false;
    if(config_.rememberUi){try {
        std::ifstream input(preferencesPath(),std::ios::binary);auto state=Json::parse(input);
        brightness_=std::clamp(state.value("brightness",1.65f),.5f,3.f);representatives_=state.value("representatives",true);filter_=std::clamp(state.value("filter",0),0,3);masks_=state.value("masks",true);boxes_=state.value("boxes",true);circles_=state.value("circles",true);view_=std::clamp(state.value("view",0),0,viewCount-1);outlines_=state.value("outlines",true);
        if(state.contains("panels")&&state["panels"].is_array()&&state["panels"].size()==3){listOpen_=state["panels"][0];sideOpen_=state["panels"][1];summaryOpen_=state["panels"][2];}
        for(int i=0;i<int(catalog_["scenes"].size());++i){auto& scene=catalog_["scenes"][i];if(scene["recording"]==state.at("recording")&&scene["index"]==state.at("index")){
            index=i;editor=state.value("editor",false);for(int c=0;c<int(scene["members"].size());++c)if(scene["members"][c]["camera"]==state.at("camera"))camera=c;break;
        }}
    }catch(...) {}}
    if(!catalog_["scenes"].empty())loadScene(index,editor,camera);
}
bool Application::idle() const {return settled()&&std::none_of(frames_.begin(),frames_.end(),[](const FrameView& frame){return frame.dirty;});}
bool Application::canClose() const {return !busy()&&!pending()&&std::none_of(frames_.begin(),frames_.end(),[](const FrameView& frame){return frame.dirty;});}
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
void Application::connect() {
    const auto config=config_;
    launch("로컬 데이터 연결", [config] {
        auto engine=std::make_shared<Engine>(EnginePaths{config.source,config.project,config.exports,config.models,config.root});
        JobResult result{{{"catalog",engine->nativeCatalog()},{"auto",engine->autoStatus()},{"assist",engine->assistStatus()}}, {}};
        result.engine=std::move(engine);
        return result;
    }, [this](JobResult result){
        engine_=std::move(result.engine); updateCatalog(result.data.at("catalog")); autoStatus_=result.data.at("auto"); message_="기존 촬영과 라벨 연결 완료";
        circleMargin_=float(catalog_.value("circle_margin",.5));
        // The colour camera's balls get their colours by themselves: drafts and older labels without one are filled in the background.
        const int colorable=catalog_.value("colorable",0);
        if(colorable>0){try{colorStatus_=engine_->nativeAction("fill-colors",Json::object());message_="컬러 공 "+grouped(colorable)+"개의 색상을 채우는 중";}catch(const std::exception& error){message_=error.what();}}
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
void Application::releaseFrames(){for(auto& frame:frames_){if(frame.texture)glDeleteTextures(1,&frame.texture);if(frame.overlay)glDeleteTextures(1,&frame.overlay);}frames_.clear();}
namespace {
// The cameras of one scene are read at the same time, each on its own thread.
JobResult fetchScene(const std::shared_ptr<Engine>& engine,const Json& scene) {
    const auto recording=scene.at("recording").get<std::string>();
    std::vector<std::future<FrameView>> cameras;
    for(const auto& member:scene.at("members"))cameras.push_back(std::async(std::launch::async,[engine,member,recording]{
        const auto camera=member.at("camera").get<std::string>();const int index=member.at("index").get<int>();
        FrameView frame;frame.member=member;frame.annotation=engine->frame(recording,camera,index);frame.image=engine->image(recording,camera,index);return frame;}));
    JobResult result;for(auto& camera:cameras)result.frames.push_back(camera.get());
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
        releaseFrames();frames_=std::move(result.frames);scene_=index;camera_=std::clamp(camera,0,int(frames_.size())-1);selected_=-1;editor_=editor;stale_=false;aheadDirty_=true;scrollToCurrent_=true;cancel();message_=note.empty()?"원본과 저장 라벨 표시 중":note;};
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
void Application::prefetch() {
    if(!aheadDirty_||!ready()||busy()||page_!=Page::Review)return;
    const int next=nextUnreviewedIndex(),revision=catalog_["summary"].value("auto_revision",0);
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
    if(!guard())return;
    if(!frames_.empty()&&current().dirty){save([this,page]{openPage(page);});return;}
    page_=page;
    if(page!=Page::Review)refreshStage();else if(stale_)loadScene(scene_,editor_,camera_);
}
void Application::showPage(int page){openPage(Page(std::clamp(page,0,3)));}
// Enlarge one camera in place without selecting a ball.
void Application::focus(int camera) {
    if(frames_.empty()||camera<0||camera>=int(frames_.size())||!guard())return;
    if(current().dirty&&camera!=camera_){save([this,camera]{focus(camera);});return;}
    current().repaint=true;camera_=camera;editor_=true;selected_=-1;current().scale=0;current().repaint=true;cancel();
}
// Enlarge the camera, select the ball and zoom to it.
void Application::select(int camera,int ball) {
    if(frames_.empty()||camera<0||camera>=int(frames_.size())||!guard())return;
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
    // A correction that ends up equal to the mask bounds is no correction.
    if(trial.at("detection_bbox")==trial.at("bbox"))trial.erase("detection_bbox");
    checkpoint();box=trial;boxes_=true;changed(false);   // only the box moved; rebuilding the overlay textures here made every key press lag
}
void Application::checkpoint(){auto& f=current();f.history.push_back(f.annotation);if(f.history.size()>40)f.history.erase(f.history.begin());}
void Application::changed(bool repaint){auto& f=current();f.dirty=true;if(repaint)f.repaint=true;f.annotation["reviewed"]=false;f.annotation["review_status"]="editing";f.member["reviewed"]=false;f.member["status"]="editing";catalog_["split_confirmed"]=false;rowsDirty_=true;changed_=Clock::now();message_="수정됨 · 자동 저장 대기";}
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
void Application::apply() {
    if(busy())return;
    if(proposal_.is_null()){if(polygon_.size()>=3)polygonProposal();return;}
    try {
        auto box=proposal_;
        if(!brushMask_.empty())box=maskAnnotation(brushMask_,box,"manual_brush");
        checkpoint();auto& existing=current().annotation["boxes"];
        if(replacement_>=0&&replacement_<int(existing.size())) {
            box["occluded"]=existing[replacement_].value("occluded",false);
            if(existing[replacement_].contains("color"))box["color"]=existing[replacement_]["color"];
            existing[replacement_]=box;selected_=replacement_;
        } else {existing.push_back(box);selected_=int(existing.size())-1;}
        cancel();changed();message_="윤곽 반영 · 박스는 마스크에서 다시 계산했습니다.";
    } catch(const std::exception& error){error_=error.what();}
}
void Application::removeSelected(){if(editor_)removeBall(camera_,selected_);}
// Deletes a ball of any camera of the scene. Edits belong to the current camera, so that camera becomes current first.
void Application::removeBall(int slot,int ball){
    if(frames_.empty()||slot<0||slot>=int(frames_.size())||pending()||busy())return;
    if(slot!=camera_&&current().dirty){save([this,slot,ball]{removeBall(slot,ball);});return;}
    camera_=slot;auto& objects=current().annotation["boxes"];if(ball<0||ball>=int(objects.size()))return;
    checkpoint();objects.erase(objects.begin()+ball);selected_=-1;current().scale=0;current().repaint=true;changed();
}
// Grows every draft mask outward. The dialog says how many images and balls that touches; a person's work is left alone.
void Application::confirmGrowth(){
    if(!growAsk_)return;
    if(!ImGui::IsPopupOpen("##grow-masks"))ImGui::OpenPopup("##grow-masks");
    const auto viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x+viewport->WorkSize.x/2,viewport->WorkPos.y+viewport->WorkSize.y/2},ImGuiCond_Appearing,{.5f,.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{22,20});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8,10});
    if(ImGui::BeginPopupModal("##grow-masks",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoTitleBar)){
        const auto growable=catalog_.value("growable",Json::object());
        ImGui::PushFont(theme::bold,theme::px(theme::title));ImGui::TextUnformatted("자동 초안 윤곽 1픽셀 넓히기");ImGui::PopFont();
        ImGui::Text("사람이 손대지 않은 이미지 %s장의 공 %s개 마스크를 바깥으로 1픽셀 넓힙니다.",grouped(growable.value("frames",0)).c_str(),grouped(growable.value("balls",0)).c_str());
        ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"박스도 1픽셀씩 커집니다. 되돌릴 수 없고, 1분쯤 걸립니다.");ImGui::PopFont();
        ImGui::Dummy({0,4});
        bool close=false;
        ImGui::PushFont(theme::bold,theme::px(theme::base));
        if(theme::primaryButton("넓히기  Enter",{140,34})||ImGui::IsKeyPressed(ImGuiKey_Enter,false)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter,false)){later_=[this]{growMasks(1);};close=true;}
        ImGui::PopFont();ImGui::SameLine();
        if(ImGui::Button("취소  Esc",{110,34})||ImGui::IsKeyPressed(ImGuiKey_Escape,false))close=true;
        if(close){growAsk_=false;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}
void Application::growMasks(int pixels){
    if(!guard())return;
    if(!frames_.empty()&&dirtySlot()>=0){save([this,pixels]{growMasks(pixels);},dirtySlot());return;}
    const auto engine=engine_;const Json payload={{"pixels",pixels},{"expected_revision",catalog_["summary"]["revision"]}};
    launch("윤곽 넓히는 중",[engine,payload]{return JobResult{engine->nativeAction("grow-masks",payload),{}};},[this](JobResult result){
        updateSummary(result.data.at("summary"));catalog_["mask_growth"]=result.data.value("mask_growth",Json::object());catalog_["growable"]=result.data.value("growable",Json::object());
        message_="공 "+grouped(result.data.value("balls",0))+"개의 윤곽을 넓혔습니다.";
        if(!frames_.empty())loadScene(scene_,editor_,camera_,message_);
    });
}
// Fills the colour of every colour-camera ball that has none. The engine reads the images in the background.
void Application::confirmColors(){
    if(!colorAsk_)return;
    if(!ImGui::IsPopupOpen("##fill-colors"))ImGui::OpenPopup("##fill-colors");
    const auto viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x+viewport->WorkSize.x/2,viewport->WorkPos.y+viewport->WorkSize.y/2},ImGuiCond_Appearing,{.5f,.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{22,20});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8,10});
    if(ImGui::BeginPopupModal("##fill-colors",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoTitleBar)){
        ImGui::PushFont(theme::bold,theme::px(theme::title));ImGui::TextUnformatted("컬러 공 색상 채우기");ImGui::PopFont();
        ImGui::Text("컬러 카메라의 공 %s개에 빨강·파랑·흰색·검정을 자동으로 넣습니다.",grouped(catalog_.value("colorable",0)).c_str());
        ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"앱을 시작할 때도 자동으로 채웁니다. 이미 색상이 있는 공은 그대로 두고, 그동안 계속 작업할 수 있습니다.");ImGui::PopFont();
        ImGui::Dummy({0,4});
        bool close=false;
        ImGui::PushFont(theme::bold,theme::px(theme::base));
        if(theme::primaryButton("채우기  Enter",{140,34})||ImGui::IsKeyPressed(ImGuiKey_Enter,false)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter,false)){later_=[this]{fillColors();};close=true;}
        ImGui::PopFont();ImGui::SameLine();
        if(ImGui::Button("취소  Esc",{110,34})||ImGui::IsKeyPressed(ImGuiKey_Escape,false))close=true;
        if(close){colorAsk_=false;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}
// Measures every masked ball's outline again from the image. The dialog says how many balls that is.
void Application::confirmCircles(){
    if(!circleAsk_)return;
    if(!ImGui::IsPopupOpen("##refine-circles"))ImGui::OpenPopup("##refine-circles");
    const auto viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x+viewport->WorkSize.x/2,viewport->WorkPos.y+viewport->WorkSize.y/2},ImGuiCond_Appearing,{.5f,.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{22,20});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8,10});
    if(ImGui::BeginPopupModal("##refine-circles",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoTitleBar)){
        ImGui::PushFont(theme::bold,theme::px(theme::title));ImGui::TextUnformatted("공 둘레를 영상에서 다시 재기");ImGui::PopFont();
        const int count=catalog_.value(remeasure_?"circleable_remeasure":includeReviewed_?"circleable_all":"circleable",0);
        ImGui::Text("공 %s개의 가장자리를 Canny로 찾아 원을 맞추고, 그 원으로 마스크와 검출 박스를 정합니다.",grouped(count).c_str());
        ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"확인 상태는 그대로이고 공을 지우거나 더하지 않습니다. 가려진 공, 손으로 그린 윤곽, 직접 보정한 박스, 원이 맞지 않는 공은 건너뜁니다. 그동안 계속 작업할 수 있습니다.");ImGui::PopFont();
        ImGui::Dummy({0,2});
        theme::checkbox("확인한 이미지의 공도 포함",&includeReviewed_);
        theme::checkbox("이미 잰 공도 다시 재기 (둘레 여유를 바꾼 뒤)",&remeasure_);
        ImGui::TextUnformatted("둘레 여유");ImGui::SameLine();ImGui::SetNextItemWidth(96);ImGui::DragFloat("##margin",&circleMargin_,.05f,-1.f,2.f,"%+.2f px");ImGui::SameLine();
        ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"잰 반지름에 더하는 픽셀. 0은 에지의 그래디언트 최대점, 양수는 그보다 바깥");ImGui::PopFont();
        ImGui::Dummy({0,4});
        bool close=false;
        ImGui::PushFont(theme::bold,theme::px(theme::base));
        if(theme::primaryButton("다듬기  Enter",{140,34})||ImGui::IsKeyPressed(ImGuiKey_Enter,false)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter,false)){later_=[this]{refineCircles();};close=true;}
        ImGui::PopFont();ImGui::SameLine();
        if(ImGui::Button("취소  Esc",{110,34})||ImGui::IsKeyPressed(ImGuiKey_Escape,false))close=true;
        if(close){circleAsk_=false;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}
void Application::refineCircles(){
    if(!guard())return;
    if(!frames_.empty()&&dirtySlot()>=0){save([this]{refineCircles();},dirtySlot());return;}
    const auto engine=engine_;const Json payload={{"include_reviewed",includeReviewed_},{"remeasure",remeasure_},{"margin",double(circleMargin_)}};
    launch("둘레 다듬기 시작",[engine,payload]{return JobResult{engine->nativeAction("refine-circles",payload),{}};},[this](JobResult result){circleStatus_=result.data;message_="공 둘레를 다시 재는 중입니다. 끝나면 알려 드립니다.";});
}
void Application::fillColors(){
    if(!guard())return;
    if(!frames_.empty()&&dirtySlot()>=0){save([this]{fillColors();},dirtySlot());return;}
    const auto engine=engine_;
    launch("색상 채우기 시작",[engine]{return JobResult{engine->nativeAction("fill-colors",Json::object()),{}};},[this](JobResult result){colorStatus_=result.data;message_="색상을 채우는 중입니다. 끝나면 알려 드립니다.";});
}
// Asked from a crop or the ball list, where a click must not delete by itself.
void Application::confirmRemoval(){
    if(pendingRemove_.camera<0)return;
    if(pendingRemove_.camera>=int(frames_.size())||pendingRemove_.index<0||pendingRemove_.index>=int(frames_[pendingRemove_.camera].annotation["boxes"].size())){pendingRemove_={};return;}
    if(!ImGui::IsPopupOpen("##remove-ball"))ImGui::OpenPopup("##remove-ball");
    const auto viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x+viewport->WorkSize.x/2,viewport->WorkPos.y+viewport->WorkSize.y/2},ImGuiCond_Appearing,{.5f,.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{22,20});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8,10});
    if(ImGui::BeginPopupModal("##remove-ball",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoTitleBar)){
        const auto& frame=frames_[pendingRemove_.camera];const auto order=sorted(frame);
        const int rank=int(std::find(order.begin(),order.end(),pendingRemove_.index)-order.begin())+1;
        ImGui::PushFont(theme::bold,theme::px(theme::title));ImGui::Text("%s · 공 %d",cameraLabel(frame.member).c_str(),rank);ImGui::PopFont();
        ImGui::TextUnformatted("이 공을 삭제할까요?");
        ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::text2),"윤곽과 박스를 지웁니다. Ctrl+Z로 되돌릴 수 있습니다.");ImGui::PopFont();
        ImGui::Dummy({0,4});
        bool close=false;const auto target=pendingRemove_;
        ImGui::PushStyleColor(ImGuiCol_Button,theme::rgb(0xc8453f));ImGui::PushStyleColor(ImGuiCol_ButtonHovered,theme::rgb(0xd9534f));ImGui::PushStyleColor(ImGuiCol_ButtonActive,theme::rgb(0xb33a35));ImGui::PushStyleColor(ImGuiCol_Text,theme::rgb(0xffffff));
        if(ImGui::Button("삭제  Enter",{124,34})||ImGui::IsKeyPressed(ImGuiKey_Enter,false)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter,false)){later_=[this,target]{removeBall(target.camera,target.index);};close=true;}
        ImGui::PopStyleColor(4);ImGui::SameLine();
        if(ImGui::Button("취소  Esc",{110,34})||ImGui::IsKeyPressed(ImGuiKey_Escape,false))close=true;
        if(close){pendingRemove_={};ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}
void Application::undo(){if(busy())return;if(pending()){cancel();return;}auto& f=current();if(f.history.empty())return;f.annotation=f.history.back();f.history.pop_back();selected_=f.annotation["boxes"].empty()?-1:std::min(selected_,int(f.annotation["boxes"].size())-1);changed();}
void Application::segment(cv::Point2d point,bool negative) {
    if(busy())return;
    if(!proposal_.is_null()&&proposal_.value("source","")!="efficientsam"){message_="현재 윤곽을 먼저 반영하거나 취소하세요.";return;}
    if(points_.empty()) {if(negative){message_="첫 점은 공 내부에 클릭하세요.";return;}replacement_=target(current(),point);if(replacement_>=0)selected_=replacement_;}
    if(points_.size()>=32){message_="점은 최대 32개입니다. 현재 결과를 반영하거나 취소하세요.";return;}
    points_.push_back({{"x",point.x},{"y",point.y},{"label",negative?0:1}});
    auto payload=annotationPayload(current().annotation,catalog_["summary"]["revision"].get<int>());payload["points"]=points_;payload["radius"]=radius_;
    const auto key=current().annotation.at("key").get<std::string>();const auto engine=engine_;const auto token=generation_;
    launch("EfficientSAM으로 공 윤곽 분리",[engine,payload]{return JobResult{engine->assist("segment",payload),{}};},[this,key,token](JobResult result){if(token!=generation_||current().annotation.value("key","")!=key)return;proposal_=result.data.at("proposals").at(0);current().repaint=true;message_="제안 미확정 · Enter 반영, Esc 취소";});
}
// A dragged box is sent together with its center as the positive point.
void Application::segmentBox(cv::Rect2d box) {
    if(busy()||!points_.empty()||box.width<2||box.height<2)return;
    const cv::Point2d center{box.x+box.width/2,box.y+box.height/2};
    replacement_=target(current(),center);if(replacement_>=0)selected_=replacement_;
    points_.push_back({{"x",center.x},{"y",center.y},{"label",1}});
    auto payload=annotationPayload(current().annotation,catalog_["summary"]["revision"].get<int>());payload["points"]=points_;payload["radius"]=std::max(box.width,box.height)/2;payload["box"]={box.x,box.y,box.width,box.height};
    const auto key=current().annotation.at("key").get<std::string>();const auto engine=engine_;const auto token=generation_;
    launch("EfficientSAM으로 공 윤곽 분리",[engine,payload]{return JobResult{engine->assist("segment",payload),{}};},[this,key,token](JobResult result){if(token!=generation_||current().annotation.value("key","")!=key)return;proposal_=result.data.at("proposals").at(0);current().repaint=true;message_="제안 미확정 · Enter 반영, Esc 취소";});
}
void Application::polygonProposal(){
    if(busy()||polygon_.size()<3)return;Json points=Json::array();for(auto point:polygon_)points.push_back({point.x,point.y});
    // An outline follows the edge of a ball, so its middle says which ball it is; its first point may lie in a neighbour's box.
    replacement_=target(current(),boundsCenter(polygon_));if(replacement_>=0)selected_=replacement_;
    auto payload=annotationPayload(current().annotation,catalog_["summary"]["revision"].get<int>());payload["points"]=points;const auto engine=engine_;const auto token=generation_;
    launch("수동 폴리곤 마스크 만들기",[engine,payload]{return JobResult{engine->assist("polygon",payload),{}};},[this,token](JobResult result){if(token!=generation_)return;proposal_=result.data.at("proposals").at(0);polygon_.clear();current().repaint=true;message_="폴리곤 미리보기 · Enter로 반영";});
}
void Application::review(Scope scope,bool hold,int member) {
    if(!guard()||frames_.empty())return;
    if(dirtySlot()>=0){save([this,scope,hold,member]{review(scope,hold,member);},dirtySlot());return;}
    Json keys=Json::array();
    if(scope==Scope::Camera)keys.push_back(frames_.at(member>=0?member:camera_).annotation.at("key"));
    else for(auto& frame:frames_)if(frame.member.value("selected",true)&&(!hold||!frame.member.value("reviewed",false)))keys.push_back(frame.annotation.at("key"));
    if(keys.empty())return;
    const bool advance=scope==Scope::Scene||hold;
    // The engine refuses when it holds a different number of balls than this screen shows, as after an automatic draft.
    Json seen=Json::object();for(const auto& frame:frames_)seen[frame.annotation.at("key").get<std::string>()]=frame.annotation["boxes"].size();
    const auto engine=engine_;Json payload={{"keys",keys},{"seen",seen},{"status",hold?"hold":"reviewed"},{"expected_revision",catalog_["summary"]["revision"]}};
    launch(hold?"보류 상태 저장":"확인 저장",[engine,payload]{
        try{return JobResult{engine->nativeAction("review",payload),{}};}
        catch(const Conflict&){return JobResult{{{"conflict",true},{"catalog",engine->nativeCatalog()}},{}};}
    },[this,advance,hold,keys](JobResult result){
        if(result.data.contains("conflict")){updateCatalog(result.data["catalog"]);loadScene(scene_,editor_,camera_,"저장된 라벨이 화면과 달라 다시 불러왔습니다. 확인한 뒤 다시 누르세요.");return;}
        updateSummary(result.data.at("summary"));
        for(auto& f:frames_)if(std::find(keys.begin(),keys.end(),f.annotation["key"])!=keys.end()){f.annotation["reviewed"]=!hold;f.annotation["review_status"]=hold?"hold":"reviewed";}
        message_=hold?"보류로 표시했습니다":"확인했습니다";if(advance)nextUnreviewed();});
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
void Application::nextUnreviewed(){
    const int index=nextUnreviewedIndex();
    if(index<0){editor_=false;message_="검수 대상을 모두 확인했습니다. 분할 단계로 넘어가세요.";return;}
    if(index==scene_){message_="남은 미검수 이미지는 이 장면뿐입니다.";return;}
    loadScene(index);
}
void Application::selectSceneForCapture(int recordingNumber,int index){if(!ready()||busy())return;for(int i=0;i<int(catalog_["scenes"].size());++i)if(catalog_["scenes"][i].value("recording_number",0)==recordingNumber&&catalog_["scenes"][i].value("index",-1)==index){loadScene(i);return;}}
void Application::tick(){
    if(task_.valid()&&task_.wait_for(std::chrono::seconds(0))==std::future_status::ready){
        auto complete=std::move(completion_);
        try {auto result=task_.get();busyLabel_.clear();complete(std::move(result));}
        catch(const std::exception& error){error_=error.what();busyLabel_.clear();message_="처리에 실패했습니다. 변경과 현재 위치를 유지합니다.";
            if(!frames_.empty()&&current().dirty){try{preserveRecovery();message_="저장 실패 · 변경은 메모리와 recovery 폴더에 보관했습니다.";}catch(...) {}}
        }
    }
    pumpThumbs();prefetch();
    if(ready()&&!busy()&&!pending()&&dirtySlot()>=0&&Clock::now()-changed_>std::chrono::milliseconds(450)&&error_.empty())save({},dirtySlot());
    const bool activeJob=autoStatus_.value("job",Json::object()).value("status","")=="running"||exportStatus_.value("status","")=="running"||colorStatus_.value("status","")=="running"||circleStatus_.value("status","")=="running";
    if(activeJob&&ready()&&!busy()&&Clock::now()-poll_>std::chrono::seconds(3)&&error_.empty()&&!current().dirty&&!pending()){
        poll_=Clock::now();const auto engine=engine_;
        launch("작업 상태 확인",[engine]{return JobResult{{{"auto",engine->autoStatus()},{"export",engine->exportStatus()},{"color",engine->colorFillStatus()},{"circle",engine->circleRefineStatus()},{"catalog",engine->nativeCatalog()}}, {}};},[this](JobResult result){
            const auto oldRevision=catalog_["summary"].value("revision",0),oldAuto=catalog_["summary"].value("auto_revision",0);const bool exporting=exportStatus_.value("status","")=="running";
            const bool coloring=colorStatus_.value("status","")=="running",refining=circleStatus_.value("status","")=="running";
            autoStatus_=result.data["auto"];exportStatus_=result.data["export"];colorStatus_=result.data["color"];circleStatus_=result.data["circle"];updateCatalog(result.data["catalog"]);
            if(refining&&circleStatus_.value("status","")!="running"){message_=circleStatus_.value("status","")=="complete"?"공 "+grouped(circleStatus_.value("balls",0))+"개의 둘레를 다시 쟀습니다. 건너뛴 공 "+grouped(circleStatus_.value("skipped",0))+"개.":"둘레 다듬기가 실패했습니다.";if(page_==Page::Review)loadScene(scene_,editor_,camera_,message_);else stale_=true;}
            if(coloring&&colorStatus_.value("status","")!="running"){message_=colorStatus_.value("status","")=="complete"?"공 "+grouped(colorStatus_.value("filled",0))+"개의 색상을 채웠습니다.":"색상 채우기가 실패했습니다.";if(page_==Page::Review)loadScene(scene_,editor_,camera_,message_);else stale_=true;}
            if(oldRevision!=catalog_["summary"].value("revision",0)||oldAuto!=catalog_["summary"].value("auto_revision",0)){if(page_==Page::Review)loadScene(scene_,editor_,camera_);else stale_=true;}
            if(exporting&&exportStatus_.value("status","")!="running"){message_=exportStatus_.contains("error")?"내보내기가 실패했습니다. 내보내기 화면에서 원인을 확인하세요.":"내보내기를 마쳤습니다.";refreshStage();}
        });
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
        else{const CircleFit fit=refineCircle(f.image,mask);f.circles[i]=fit.ok?Circle{fit.x-.5,fit.y-.5,fit.r+circleMargin_}:fitCircle(edge);}
        if(active&&replacement_==i&&!proposal_.is_null())continue;
        overlay.setTo(active&&editor_&&i==selected_?cv::Scalar(255,216,74,13):cv::Scalar(46,230,208,51),mask);
    }
    if(active){
        proposalContours_.clear();
        // Only a brush stroke is filled, so painting gives feedback; AI and polygon proposals show their outline only.
        if(!brushMask_.empty()){overlay.setTo(cv::Scalar(255,90,217,70),brushMask_);cv::findContours(brushMask_.clone(),proposalContours_,cv::RETR_EXTERNAL,cv::CHAIN_APPROX_SIMPLE);}
        else if(!proposal_.is_null()&&proposal_.contains("segmentation")){auto mask=decodeMask(proposal_["segmentation"],f.image.cols,f.image.rows);cv::findContours(mask,proposalContours_,cv::RETR_EXTERNAL,cv::CHAIN_APPROX_SIMPLE);}
    }
    uploadTexture(f.overlay,overlay,true);f.repaint=false;
}
// The ball a new outline at this point changes, or -1 when it adds a ball.
int Application::target(const FrameView& frame,cv::Point2d point)const {
    std::vector<cv::Rect2d> boxes;for(const auto& box:frame.annotation["boxes"])boxes.push_back(detectionBox(box));
    return replaceTarget(boxes,point,selected_);
}
int Application::hit(const FrameView& frame,cv::Point2d point)const {for(int i=int(frame.annotation["boxes"].size())-1;i>=0;--i)if(detectionBox(frame.annotation["boxes"][i]).contains(point))return i;return -1;}
// scale 0 means automatic: zoom to the selected ball, otherwise fit the whole image.
void Application::fit(FrameView& f,ImVec2 size,bool toBall){
    const auto& boxes=f.annotation["boxes"];
    const View view=toBall&&selected_>=0&&selected_<int(boxes.size())?ballView(detectionBox(boxes[selected_]),f.image.cols,f.image.rows,size.x,size.y):fitView(f.image.cols,f.image.rows,size.x,size.y);
    f.scale=view.scale;f.offsetX=view.x;f.offsetY=view.y;
}
void Application::askRemoveForCapture(int camera,int rank){if(camera<0||camera>=int(frames_.size()))return;const auto order=sorted(frames_[camera]);if(rank>=0&&rank<int(order.size()))pendingRemove_={camera,order[rank]};}
void Application::selectForCapture(int camera,int rank){if(frames_.empty()||camera<0||camera>=int(frames_.size()))return;const auto order=sorted(frames_[camera]);if(rank>=0&&rank<int(order.size()))select(camera,order[rank]);}
// One step per call while the app is idle. Returns false when every step has passed; throws on a failed expectation.
bool Application::selfCheckStep(int step) {
    auto expect=[](bool condition,const char* what){if(!condition)throw std::runtime_error(std::string("self-test: ")+what);};
    static int checkedScene=-1;static Json before;static BallRef was;
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
        expect(box.at("bbox")==before.at("bbox")&&box.value("segmentation",Json())==before.value("segmentation",Json()),"box correction changed the mask");
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
    case 17:{for(const auto& member:catalog_["scenes"][checkedScene]["members"])expect(!member.value("selected",true)||member.value("reviewed",false),"scene confirm left an image unconfirmed");
        expect(scene_!=checkedScene&&!editor_,"confirm did not advance to the next unreviewed scene");
        expect(aheadHits_>0,"the next unreviewed scene was not fetched ahead");return false;}
    }
    return false;
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
    case 3:expect(page_==Page::Export&&dataVersion().size()==64,"confirming did not open the export page with a data version");crops_=false;startExport();return true;
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
    launch("현황 새로 읽기",[engine]{return JobResult{{{"catalog",engine->nativeCatalog()},{"exports",engine->exports()},{"export",engine->exportStatus()},{"auto",engine->autoStatus()},{"color",engine->colorFillStatus()},{"circle",engine->circleRefineStatus()}},{}};},
        [this](JobResult result){
            const auto revision=catalog_["summary"].value("revision",0),automatic=catalog_["summary"].value("auto_revision",0);
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
    if(page_!=Page::Review||io.WantTextInput||frames_.empty()||pendingRemove_.camera>=0||growAsk_||colorAsk_||circleAsk_)return;   // an open dialog owns the keys
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
    if(pressed(ImGuiKey_E))review(io.KeyShift&&editor_?Scope::Camera:Scope::Scene,false);
    if(pressed(ImGuiKey_Q))review(editor_?Scope::Camera:Scope::Scene,true);
    if(pressed(ImGuiKey_N)&&guard())nextUnreviewed();
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
    if(pressed(ImGuiKey_LeftBracket))brushSize_=std::max(2.f,brushSize_-1);
    if(pressed(ImGuiKey_RightBracket))brushSize_=std::min(30.f,brushSize_+1);
    auto& objects=current().annotation["boxes"];
    if(selected_<0||selected_>=int(objects.size()))return;
    if(pressed(ImGuiKey_O)){checkpoint();objects[selected_]["occluded"]=!objects[selected_].value("occluded",false);changed();}
    if(pressed(ImGuiKey_Delete))removeSelected();
}
// Name, page tabs, progress, save state and the shortcut list. Plain tabs with an underline, like any desktop tool.
void Application::topBar() {
    const float width=ImGui::GetContentRegionAvail().x,height=44;const ImVec2 origin=ImGui::GetCursorScreenPos();
    ImGui::Dummy({width,height});const ImVec2 after=ImGui::GetCursorScreenPos();auto draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,{origin.x+width,origin.y+height},theme::bg1);
    draw->AddLine({origin.x,origin.y+height-.5f},{origin.x+width,origin.y+height-.5f},theme::line);
    theme::write(draw,{origin.x+16,origin.y+13},theme::text,"Boccia Labeler",14,theme::bold);
    float x=origin.x+16+theme::extent("Boccia Labeler",14,theme::bold).x+28;
    static const char* names[]={"사전 작업","검수","분할","내보내기"};
    for(int i=0;i<4;++i){
        const float tab=theme::extent(names[i]).x+24;const bool on=int(page_)==i;
        ImGui::SetCursorScreenPos({x,origin.y});ImGui::PushID(i);
        if(ImGui::InvisibleButton("##tab",{tab,height})&&!on)openPage(Page(i));
        const bool hovered=ImGui::IsItemHovered();ImGui::PopID();
        theme::write(draw,{x+12,origin.y+13},on||hovered?theme::text:theme::text2,names[i]);
        if(on)draw->AddRectFilled({x+8,origin.y+height-2},{x+tab-8,origin.y+height},theme::accent);
        x+=tab;
    }
    const int target=catalog_.is_object()?catalog_.value("target_images",0):0,done=catalog_.is_object()?catalog_.value("reviewed_target_images",0):0;
    float right=origin.x+width-14;
    ImGui::PushFont(nullptr,theme::px(theme::caption));ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{10,5});
    const float help=theme::extent("단축키",theme::caption).x+20;right-=help;
    ImGui::SetCursorScreenPos({right,origin.y+9});
    if(ImGui::Button("단축키",{help,26})||shortcutsWanted_){ImGui::OpenPopup("##shortcuts");shortcutsWanted_=false;}
    shortcutsPopup({right+help,origin.y+height});
    ImGui::PopStyleVar();ImGui::PopFont();
    const bool dirty=!frames_.empty()&&current().dirty;
    const std::string saved=busy()?busyLabel_:dirty?"저장 대기":"저장됨";
    right-=20+theme::extent(saved,theme::caption).x;theme::write(draw,{right,origin.y+14},busy()||dirty?theme::hold:theme::text3,saved,theme::caption);
    draw->AddCircleFilled({right-9,origin.y+22},3,busy()||dirty?theme::hold:theme::ok);
    const std::string count="검수 "+grouped(done)+" / "+grouped(target);
    right-=34+120;draw->AddRectFilled({right,origin.y+20},{right+120,origin.y+24},theme::bg4,2);
    if(target&&done)draw->AddRectFilled({right,origin.y+20},{right+std::max(2.f,120.f*done/target),origin.y+24},theme::ok,2);
    right-=10+theme::extent(count,theme::caption,theme::mono).x;theme::write(draw,{right,origin.y+14},theme::text,count,theme::caption,theme::mono);
    ImGui::SetCursorScreenPos(after);
}
// The key list lives here instead of in hint lines all over the screens.
void Application::shortcutsPopup(ImVec2 below) {
    static const char* const rows[][2]={{"E","세 장 확인하고 다음 미검수 장면"},{"Shift+E","크게 보는 카메라 한 장만 확인"},{"Q","보류"},{"N","다음 미검수 장면"},
        {"A / D","이전 / 다음 장면"},{"Backspace","이전에 보던 장면으로"},{"1 / 2 / 3","그 카메라를 크게"},{"Tab / Shift+Tab","다음 / 이전 공"},
        {"방향키","선택한 공의 그 방향 경계를 1픽셀 바깥으로"},{"Shift+방향키","1픽셀 안으로"},{"V / S / B / P / R","선택 / AI 분할 / 브러시 / 폴리곤 / 박스"},
        {"왼쪽 / 오른쪽 클릭","더하기 / 빼기"},{"[ / ]","브러시 굵기"},{"Enter / Esc","윤곽 반영 / 취소, 선택 해제, 세 장 보기"},{"G / M / X / C","윤곽 / 마스크 채움 / 박스 / 원 표시"},{"I","보기: 원본 / 대비 / 흑백 / 에지 / 그래디언트 / 에지 겹침"},
        {"F / Z","화면 맞춤 / 선택한 공으로"},{"O / Delete","일부 가림 / 공 삭제"},{"Ctrl+Z / Ctrl+S","되돌리기 / 저장"},{"휠 / 가운데 끌기","확대 / 이동"}};
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
void Application::errorBar() {
    const float width=ImGui::GetContentRegionAvail().x;const ImVec2 origin=ImGui::GetCursorScreenPos();
    ImGui::Dummy({width,36});const ImVec2 after=ImGui::GetCursorScreenPos();auto draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,{origin.x+width,origin.y+36},theme::rgb(0xff8a4c,30));
    theme::warning(draw,{origin.x+22,origin.y+18},13,theme::warn);
    theme::write(draw,{origin.x+38,origin.y+10},theme::warn,error_);
    ImGui::SetCursorScreenPos({origin.x+width-196,origin.y+5});
    if(ImGui::Button("닫기",{60,26}))error_.clear();
    ImGui::SameLine(0,8);
    if(ImGui::Button("다시 연결",{110,26})){if(!busy()&&(frames_.empty()||!current().dirty))connect();else message_="현재 변경을 저장하거나 recovery 파일을 보관한 뒤 다시 실행하세요.";}
    ImGui::SetCursorScreenPos(after);
}
void Application::statusBar() {
    const float width=ImGui::GetContentRegionAvail().x;const ImVec2 origin=ImGui::GetCursorScreenPos();
    ImGui::Dummy({width,28});auto draw=ImGui::GetWindowDrawList();
    draw->AddLine(origin,{origin.x+width,origin.y},theme::line);
    theme::write(draw,{origin.x+14,origin.y+7},busy()?theme::hold:theme::text3,busy()?busyLabel_:message_,theme::caption);
    const std::string source="원본 "+utf8(config_.source);
    theme::write(draw,{origin.x+width-14-theme::extent(source,theme::caption).x,origin.y+7},theme::text3,source,theme::caption);
}
void Application::draw(){
    const auto viewport=ImGui::GetMainViewport();ImGui::SetNextWindowPos(viewport->WorkPos);ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Boccia Labeler",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{0,0});
    topBar();
    if(!error_.empty())errorBar();
    const float body=ImGui::GetContentRegionAvail().y-28;
    if(!ready()){
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
        if(page_==Page::Prepare)preparePage();else if(page_==Page::Split)splitPage();else exportPage();
        ImGui::EndChild();ImGui::PopStyleColor();ImGui::PopStyleVar(2);
    }
    statusBar();
    confirmRemoval();
    confirmGrowth();
    confirmColors();
    confirmCircles();
    ImGui::PopStyleVar();
    ImGui::End();
    if(later_){auto action=std::move(later_);later_=nullptr;action();}
}
}
