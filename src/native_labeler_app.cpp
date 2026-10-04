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
ImVec4 green{.35f, .86f, .70f, 1}, muted{.57f, .67f, .73f, 1}, amber{.95f, .73f, .36f, 1};
std::string title(const Json& scene) { return "촬영 " + std::to_string(scene.at("recording_number").get<int>()) + " · 프레임 " + std::to_string(scene.at("index").get<int>()); }
std::string utf8(const std::filesystem::path& value) { auto text=value.u8string(); return {text.begin(),text.end()}; }
void hint(const char* text) { ImGui::TextColored(muted, "%s", text); }
bool primary(const char* label, ImVec2 size = {}) { ImGui::PushStyleColor(ImGuiCol_Button, {.13f,.38f,.32f,1}); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.19f,.49f,.40f,1}); bool result=ImGui::Button(label,size); ImGui::PopStyleColor(2); return result; }
}
Application::Application(Configuration config): config_(std::move(config)), client_(config_.port), poll_(Clock::now()) { connect(); }
Application::~Application() {
    if(task_.valid()) task_.wait(); if(thumbTask_.valid()) thumbTask_.wait(); if(ahead_.valid()) ahead_.wait();
    if(config_.rememberUi){try{rememberPosition();}catch(...){}} releaseFrames();
    for(auto& [key,id]:thumbs_)if(id)glDeleteTextures(1,&id);
}
std::filesystem::path Application::preferencesPath() const { return config_.project.parent_path()/(config_.project.stem().wstring()+L"-native-ui.json"); }
void Application::rememberPosition() const {
    if(frames_.empty())return;
    const auto& scene=catalog_["scenes"][scene_];
    Json state={{"recording",scene["recording"]},{"index",scene["index"]},{"camera",frames_[camera_].member["camera"]},{"editor",editor_},{"brightness",brightness_},{"representatives",representatives_},{"filter",filter_}};
    auto path=preferencesPath();std::filesystem::create_directories(path.parent_path());
    auto temporary=path;temporary+=L"."+std::to_wstring(Clock::now().time_since_epoch().count())+L".tmp";std::ofstream stream(temporary,std::ios::binary);stream<<state.dump(2);stream.close();
    if(!stream)throw std::runtime_error("작업 위치 저장 실패");
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("작업 위치 반영 실패");
}
void Application::restorePosition() {
    int index=0,camera=0;bool editor=false;
    if(config_.rememberUi){try {
        std::ifstream input(preferencesPath(),std::ios::binary);auto state=Json::parse(input);
        brightness_=std::clamp(state.value("brightness",1.65f),.5f,3.f);representatives_=state.value("representatives",true);filter_=std::clamp(state.value("filter",0),0,3);
        for(int i=0;i<int(catalog_["scenes"].size());++i){auto& scene=catalog_["scenes"][i];if(scene["recording"]==state.at("recording")&&scene["index"]==state.at("index")){
            index=i;editor=state.value("editor",false);for(int c=0;c<int(scene["members"].size());++c)if(scene["members"][c]["camera"]==state.at("camera"))camera=c;break;
        }}
    }catch(...) {}}
    if(!catalog_["scenes"].empty())loadScene(index,editor,camera);
}
bool Application::canClose() const {return !busy()&&!pending()&&std::none_of(frames_.begin(),frames_.end(),[](const FrameView& frame){return frame.dirty;});}
void Application::preserveRecovery(){
    auto text=std::string(BOCCIA_LABELER_ROOT);auto directory=std::filesystem::path(std::u8string(text.begin(),text.end()))/"output/native-labeler/recovery";std::filesystem::create_directories(directory);
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
void Application::connect() {
    const auto config=config_; const auto client=client_;
    launch("로컬 데이터 연결", [config,client] {
        Json base;
        try { base=client.get("/api/catalog"); }
        catch (...) {
            if(!config.startServer) throw;
            Client::startServer(config.port,config.source,config.project,config.exports);
            for(int i=0;i<100;++i) { try {base=client.get("/api/catalog");break;}catch(...){if(i==99)throw;std::this_thread::sleep_for(std::chrono::milliseconds(150));} }
        }
        auto same=[](const std::string& path,const std::filesystem::path& expected){return std::filesystem::weakly_canonical(fromUtf8(path))==std::filesystem::weakly_canonical(expected);};
        if(!same(base.at("source").get<std::string>(),config.source)||!same(base.at("project_path").get<std::string>(),config.project))
            throw std::runtime_error("이 포트는 다른 프로젝트를 사용 중입니다. 별도 --port와 --project를 지정하세요.");
        return JobResult{{{"catalog",client.get("/api/native/catalog")},{"auto",client.get("/api/auto-status")},{"assist",client.get("/api/assist-status")}}, {}};
    }, [this](JobResult result){ updateCatalog(result.data.at("catalog")); autoStatus_=result.data.at("auto"); message_="기존 촬영과 라벨 연결 완료"; restorePosition(); });
}
void Application::updateCatalog(Json value) {
    catalog_=std::move(value);rowsDirty_=true;
    for(const auto& recording:catalog_["recordings"]) {
        auto id=recording.at("id").get<std::string>();
        if(!groups_.contains(id))groups_[id]=id;
        if(!splits_.contains(id))splits_[id]=catalog_["summary"]["splits"].value(id,"unassigned");
    }
    if(catalog_.value("split_confirmed",false))groups_=catalog_.at("groups");
}
void Application::updateSummary(const Json& summary) {
    catalog_["summary"]=summary; catalog_["split_confirmed"]=false;rowsDirty_=true;
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
void Application::loadScene(int index,bool editor,int camera) {
    if(index<0||index>=int(catalog_["scenes"].size())||busy())return;
    const auto scene=catalog_["scenes"][index]; const auto client=client_; const auto token=++generation_;
    launch("카메라 이미지 불러오기", [client,scene]{
        JobResult result; const auto recording=scene.at("recording").get<std::string>();
        for(const auto& member:scene.at("members")){FrameView frame;frame.member=member;frame.annotation=client.get("/api/frame"+Client::frameQuery(member,recording));frame.annotation["review_status"]=member.value("status","draft");frame.image=client.image(member,recording);result.frames.push_back(std::move(frame));}
        return result;
    },[this,index,editor,camera,token](JobResult result){if(token!=generation_)return;releaseFrames();frames_=std::move(result.frames);scene_=index;camera_=std::clamp(camera,0,int(frames_.size())-1);selected_=-1;editor_=editor;page_=Page::Review;scrollToCurrent_=true;cancel();message_="원본과 저장 라벨 표시 중";});
}
bool Application::pending() const { return !proposal_.is_null()||!polygon_.empty()||!points_.empty()||dragging_; }
bool Application::guard(){if(busy()){message_="처리가 끝난 뒤 이동하세요. 화면 확대·이동은 계속할 수 있습니다.";return false;}if(pending()){message_="미확정 윤곽을 반영하거나 취소하세요.";return false;}return true;}
void Application::navigate(int index){if(!guard())return;if(!frames_.empty()&&current().dirty){save([this,index]{loadScene(index);});return;}loadScene(index);}
void Application::showEditor(int camera){focus(camera);}
void Application::switchCamera(int camera){focus(camera);}
void Application::openPage(Page page){if(!guard())return;if(!frames_.empty()&&current().dirty)save([this,page]{page_=page;});else page_=page;if(page==Page::Export)refreshExports();}
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
    checkpoint();box=trial;boxes_=true;changed();
}
void Application::checkpoint(){auto& f=current();f.history.push_back(f.annotation);if(f.history.size()>40)f.history.erase(f.history.begin());}
void Application::changed(){auto& f=current();f.dirty=true;f.repaint=true;f.annotation["reviewed"]=false;f.annotation["review_status"]="editing";f.member["reviewed"]=false;f.member["status"]="editing";catalog_["split_confirmed"]=false;rowsDirty_=true;changed_=Clock::now();message_="수정됨 · 자동 저장 대기";}
void Application::save(std::function<void()> next) {
    if(frames_.empty()||pending()||busy())return;
    const int slot=camera_; const auto key=current().annotation.at("key").get<std::string>();
    const auto payload=annotationPayload(current().annotation,catalog_["summary"].at("revision").get<int>()); const auto client=client_;
    launch("라벨 저장",[client,payload]{return JobResult{client.post("/api/annotation",payload),{}};},[this,slot,key,next](JobResult result){updateSummary(result.data.at("summary"));if(slot<int(frames_.size())&&frames_[slot].annotation.value("key","")==key)frames_[slot].dirty=false;message_="자동 저장 완료";if(next)next();});
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
void Application::undo(){if(busy())return;if(pending()){cancel();return;}auto& f=current();if(f.history.empty())return;f.annotation=f.history.back();f.history.pop_back();selected_=f.annotation["boxes"].empty()?-1:std::min(selected_,int(f.annotation["boxes"].size())-1);changed();}
void Application::segment(cv::Point2d point,bool negative) {
    if(busy())return;
    if(!proposal_.is_null()&&proposal_.value("source","")!="efficientsam"){message_="현재 윤곽을 먼저 반영하거나 취소하세요.";return;}
    if(points_.empty()) {if(negative){message_="첫 점은 공 내부에 클릭하세요.";return;}replacement_=hit(current(),point);if(replacement_>=0)selected_=replacement_;}
    if(points_.size()>=32){message_="점은 최대 32개입니다. 현재 결과를 반영하거나 취소하세요.";return;}
    points_.push_back({{"x",point.x},{"y",point.y},{"label",negative?0:1}});
    auto payload=annotationPayload(current().annotation,catalog_["summary"]["revision"].get<int>());payload["points"]=points_;payload["radius"]=radius_;
    const auto key=current().annotation.at("key").get<std::string>();const auto client=client_;const auto token=generation_;
    launch("EfficientSAM으로 공 윤곽 분리",[client,payload]{return JobResult{client.post("/api/assist/segment",payload),{}};},[this,key,token](JobResult result){if(token!=generation_||current().annotation.value("key","")!=key)return;proposal_=result.data.at("proposals").at(0);current().repaint=true;message_="제안 미확정 · Enter 반영, Esc 취소";});
}
// A dragged box is sent together with its center as the positive point.
void Application::segmentBox(cv::Rect2d box) {
    if(busy()||!points_.empty()||box.width<2||box.height<2)return;
    const cv::Point2d center{box.x+box.width/2,box.y+box.height/2};
    replacement_=hit(current(),center);if(replacement_>=0)selected_=replacement_;
    points_.push_back({{"x",center.x},{"y",center.y},{"label",1}});
    auto payload=annotationPayload(current().annotation,catalog_["summary"]["revision"].get<int>());payload["points"]=points_;payload["radius"]=std::max(box.width,box.height)/2;payload["box"]={box.x,box.y,box.width,box.height};
    const auto key=current().annotation.at("key").get<std::string>();const auto client=client_;const auto token=generation_;
    launch("EfficientSAM으로 공 윤곽 분리",[client,payload]{return JobResult{client.post("/api/assist/segment",payload),{}};},[this,key,token](JobResult result){if(token!=generation_||current().annotation.value("key","")!=key)return;proposal_=result.data.at("proposals").at(0);current().repaint=true;message_="제안 미확정 · Enter 반영, Esc 취소";});
}
void Application::polygonProposal(){
    if(busy()||polygon_.size()<3)return;Json points=Json::array();for(auto point:polygon_)points.push_back({point.x,point.y});
    auto payload=annotationPayload(current().annotation,catalog_["summary"]["revision"].get<int>());payload["points"]=points;const auto client=client_;const auto token=generation_;
    launch("수동 폴리곤 마스크 만들기",[client,payload]{return JobResult{client.post("/api/assist/polygon",payload),{}};},[this,token](JobResult result){if(token!=generation_)return;proposal_=result.data.at("proposals").at(0);polygon_.clear();current().repaint=true;message_="폴리곤 미리보기 · Enter로 반영";});
}
void Application::review(Scope scope,bool hold,int member) {
    if(!guard()||frames_.empty())return;
    if(current().dirty){save([this,scope,hold,member]{review(scope,hold,member);});return;}
    Json keys=Json::array();
    if(scope==Scope::Camera)keys.push_back(frames_.at(member>=0?member:camera_).annotation.at("key"));
    else for(auto& frame:frames_)if(frame.member.value("selected",true)&&(!hold||!frame.member.value("reviewed",false)))keys.push_back(frame.annotation.at("key"));
    if(keys.empty())return;
    const bool advance=scope==Scope::Scene||hold;
    const auto client=client_;Json payload={{"keys",keys},{"status",hold?"hold":"reviewed"},{"expected_revision",catalog_["summary"]["revision"]}};
    launch(hold?"보류 상태 저장":"확인 저장",[client,payload]{return JobResult{client.post("/api/native/review",payload),{}};},[this,advance,hold,keys](JobResult result){
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
void Application::nextUnreviewed(){
    const int index=nextUnreviewedIndex();
    if(index<0){editor_=false;message_="검수 대상을 모두 확인했습니다. 분할 단계로 넘어가세요.";return;}
    if(index==scene_){message_="남은 미검수 이미지는 이 장면뿐입니다.";return;}
    loadScene(index);
}
void Application::refresh(){if(!guard())return;if(!frames_.empty()&&current().dirty){message_="저장되지 않은 변경이 있습니다. 저장을 완료한 뒤 새로고침하세요.";return;}const auto client=client_;launch("저장 상태 새로고침",[client]{return JobResult{client.get("/api/native/catalog"),{}};},[this](JobResult result){updateCatalog(result.data);loadScene(scene_,editor_,camera_);});}
void Application::showSplit(){if(!guard())return;if(!frames_.empty()&&current().dirty){save([this]{showSplit();});return;}page_=Page::Split;editor_=false;}
void Application::selectSceneForCapture(int recordingNumber,int index){if(!ready()||busy())return;for(int i=0;i<int(catalog_["scenes"].size());++i)if(catalog_["scenes"][i].value("recording_number",0)==recordingNumber&&catalog_["scenes"][i].value("index",-1)==index){loadScene(i);return;}}
void Application::tick(){
    if(task_.valid()&&task_.wait_for(std::chrono::seconds(0))==std::future_status::ready){
        auto complete=std::move(completion_);
        try {auto result=task_.get();busyLabel_.clear();complete(std::move(result));}
        catch(const std::exception& error){error_=error.what();busyLabel_.clear();message_="처리에 실패했습니다. 변경과 현재 위치를 유지합니다.";
            if(!frames_.empty()&&current().dirty){try{preserveRecovery();message_="저장 실패 · 변경은 메모리와 recovery 폴더에 보관했습니다.";}catch(...) {}}
        }
    }
    pumpThumbs();
    if(ready()&&!busy()&&!pending()&&current().dirty&&Clock::now()-changed_>std::chrono::milliseconds(450)&&error_.empty())save();
    const bool activeJob=autoStatus_.value("job",Json::object()).value("status","")=="running"||exportStatus_.value("status","")=="running";
    if(activeJob&&ready()&&!busy()&&Clock::now()-poll_>std::chrono::seconds(3)&&error_.empty()&&!current().dirty&&!pending()){
        poll_=Clock::now();const auto client=client_;
        launch("작업 상태 확인",[client]{return JobResult{{{"auto",client.get("/api/auto-status")},{"export",client.get("/api/export-status")},{"catalog",client.get("/api/native/catalog")}}, {}};},[this](JobResult result){
            const auto oldRevision=catalog_["summary"].value("revision",0),oldAuto=catalog_["summary"].value("auto_revision",0);
            autoStatus_=result.data["auto"];exportStatus_=result.data["export"];updateCatalog(result.data["catalog"]);
            if(oldRevision!=catalog_["summary"].value("revision",0)||oldAuto!=catalog_["summary"].value("auto_revision",0))loadScene(scene_,editor_,camera_);
        });
    }
}
void Application::uploadTexture(GLuint& texture,const cv::Mat& pixels,bool rgba){
    if(!texture)glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glTexImage2D(GL_TEXTURE_2D,0,rgba?GL_RGBA:GL_RGB,pixels.cols,pixels.rows,0,rgba?GL_RGBA:GL_BGR,GL_UNSIGNED_BYTE,pixels.data);
}
// Rebuilds the display image, the mask fill overlay and the cached contours of one frame.
void Application::repaint(FrameView& f){
    cv::Mat display;f.image.convertTo(display,CV_8UC3,f.member.value("kind","")=="컬러"?brightness_:1.12);uploadTexture(f.texture,display);
    cv::Mat overlay(f.image.size(),CV_8UC4,cv::Scalar(0,0,0,0));
    const bool active=!frames_.empty()&&&f==&current();
    const auto& boxes=f.annotation["boxes"];f.contours.assign(boxes.size(),{});
    for(int i=0;i<int(boxes.size());++i){
        if(!boxes[i].contains("segmentation"))continue;
        auto mask=decodeMask(boxes[i]["segmentation"],f.image.cols,f.image.rows);
        cv::findContours(mask.clone(),f.contours[i],cv::RETR_EXTERNAL,cv::CHAIN_APPROX_SIMPLE);
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
int Application::hit(const FrameView& frame,cv::Point2d point)const {for(int i=int(frame.annotation["boxes"].size())-1;i>=0;--i)if(detectionBox(frame.annotation["boxes"][i]).contains(point))return i;return -1;}
// scale 0 means automatic: zoom to the selected ball, otherwise fit the whole image.
void Application::fit(FrameView& f,ImVec2 size,bool toBall){
    const auto& boxes=f.annotation["boxes"];
    const View view=toBall&&selected_>=0&&selected_<int(boxes.size())?ballView(detectionBox(boxes[selected_]),f.image.cols,f.image.rows,size.x,size.y):fitView(f.image.cols,f.image.rows,size.x,size.y);
    f.scale=view.scale;f.offsetX=view.x;f.offsetY=view.y;
}
void Application::preparePage(){
    ImGui::TextUnformatted("데이터 준비");ImGui::Separator();ImGui::TextWrapped("입력: %s",utf8(config_.source).c_str());ImGui::TextWrapped("주석: %s",utf8(config_.project).c_str());
    ImGui::Spacing();int raw=0;for(auto& recording:catalog_["recordings"])for(auto& camera:recording["cameras"])raw+=camera.value("count",0);
    ImGui::Text("촬영 %d개 · 원본 %d장 · 대표 검수 대상 %d장",int(catalog_["recordings"].size()),raw,catalog_.value("target_images",0));
    ImGui::TextWrapped("원본과 기존 사람 수정은 보존합니다. 학습·평가 용도는 전체 검수 후 다음 단계에서 확정합니다.");
    ImGui::Spacing();hint("EfficientSAM-Ti · 로컬 CPU 분할");
    auto job=autoStatus_.value("job",Json::object());auto status=job.value("status","idle");ImGui::Text("자동 초안 상태: %s",status.c_str());int done=job.value("done",0),total=job.value("total",0);if(total)ImGui::ProgressBar(float(done)/total,{-1,14});
    ImGui::BeginDisabled(busy());if(primary("자동 초안 시작 / 재개")){const auto client=client_;launch("자동 초안 시작",[client]{return JobResult{client.post("/api/auto-start",Json::object()),{}};},[this](JobResult result){autoStatus_["job"]=result.data;message_="자동 처리 시작 · 검수와 자동 생성은 별개입니다.";});}ImGui::SameLine();if(ImGui::Button("자동 처리 중단")){const auto client=client_;launch("자동 처리 중단",[client]{return JobResult{client.post("/api/auto-stop",Json::object()),{}};},[this](JobResult result){autoStatus_["job"]=result.data;});}ImGui::EndDisabled();
    ImGui::Spacing();ImGui::SetNextItemWidth(300);if(ImGui::SliderFloat("컬러 보기 밝기",&brightness_,.5f,3.f,"%.2f"))for(auto& f:frames_)f.repaint=true;hint("표시용 밝기 · 학습 원본과 독립");
    ImGui::Spacing();if(primary("검수 화면으로 →"))page_=Page::Review;
}
void Application::confirmSplit(){
    const auto client=client_;Json payload={{"groups",groups_},{"splits",splits_},{"expected_revision",catalog_["summary"]["revision"]}};
    launch("촬영 그룹 분할 확정",[client,payload]{return JobResult{client.post("/api/native/split-confirm",payload),{}};},[this](JobResult result){updateSummary(result.data.at("summary"));catalog_["split_confirmed"]=true;page_=Page::Export;message_="분할 확정됨 · 이 데이터 버전으로 모델을 비교하세요.";});
}
void Application::splitPage(){
    ImGui::TextUnformatted("검수 다음 단계 · 촬영 그룹으로 데이터 분할");hint("목표: 학습 80% · 검증 10% · 최종 평가 10%");ImGui::TextWrapped("같은 촬영의 모든 카메라와 연속 프레임을 함께 배분합니다. 같은 배치의 반복 촬영은 아래 그룹 번호를 같게 지정하세요.");
    int done=catalog_.value("reviewed_target_images",0),total=catalog_.value("target_images",0);bool complete=total>0&&done==total;
    ImGui::TextColored(complete?green:amber,"검수 %d / %d장 · %s",done,total,complete?"분할 확정 가능":"전체 검수를 먼저 완료하세요");ImGui::Separator();
    ImGui::BeginChild("SplitAssignments",{0,ImGui::GetContentRegionAvail().y-90},false);
    if(ImGui::BeginTable("RecordingSplits",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_SizingStretchProp)){ImGui::TableSetupColumn("촬영");ImGui::TableSetupColumn("같은 장면 그룹");ImGui::TableSetupColumn("분할");ImGui::TableSetupColumn("이미지");ImGui::TableHeadersRow();
        std::vector<std::string> ids;for(auto& r:catalog_["recordings"])ids.push_back(r.at("id").get<std::string>());
        for(int i=0;i<int(ids.size());++i){auto id=ids[i];ImGui::PushID(i);ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("촬영 %02d",i+1);ImGui::TableNextColumn();auto group=groups_.value(id,id);int position=int(std::find(ids.begin(),ids.end(),group)-ids.begin());position=std::min(position,int(ids.size())-1);auto label="그룹 "+std::to_string(position+1);
            ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##group",label.c_str())){for(int g=0;g<int(ids.size());++g){auto name="그룹 "+std::to_string(g+1);if(ImGui::Selectable(name.c_str(),g==position)){groups_[id]=ids[g];catalog_["split_confirmed"]=false;}}ImGui::EndCombo();}
            ImGui::TableNextColumn();const char* labels[]={"미지정","학습","검증","최종 평가"};const char* values[]={"unassigned","train","val","test"};int split=0;for(int s=0;s<4;++s)if(splits_.value(id,"unassigned")==values[s])split=s;ImGui::SetNextItemWidth(-1);if(ImGui::Combo("##split",&split,labels,4)){for(auto& [recording,value]:groups_.items())if(value==groups_[id])splits_[recording]=values[split];catalog_["split_confirmed"]=false;}
            ImGui::TableNextColumn();int count=0;for(auto& scene:catalog_["scenes"])if(scene["recording"]==id)for(auto& member:scene["members"])count+=member.value("selected",true);ImGui::Text("%d장",count);ImGui::PopID();
        }ImGui::EndTable();
    }ImGui::EndChild();
    int train=0,val=0,test=0;for(auto& scene:catalog_["scenes"]){auto split=splits_.value(scene.at("recording").get<std::string>(),"unassigned");for(auto& m:scene["members"])if(m.value("selected",true)){train+=split=="train";val+=split=="val";test+=split=="test";}}
    ImGui::Text("현재 배분: 학습 %d · 검증 %d · 최종 평가 %d장",train,val,test);ImGui::BeginDisabled(!complete||busy());
    if(ImGui::Button("촬영 그룹 자동 배분안")){const auto client=client_;const auto groups=groups_;launch("그룹 배분안 만들기",[client,groups]{return JobResult{client.post("/api/native/split-preview",{{"groups",groups}}),{}};},[this](JobResult result){splits_=result.data.at("splits");message_="배분안을 확인하고 최종 확정하세요.";});}ImGui::SameLine();if(primary("분할 확정 → 내보내기"))confirmSplit();ImGui::EndDisabled();hint("최종 평가는 첫 학습 전에 고정 · 그룹 크기에 따라 비율은 달라집니다.");
}
void Application::exportPage(){
    ImGui::TextUnformatted("검수 완료 데이터 내보내기");ImGui::Separator();bool confirmed=catalog_.value("split_confirmed",false);ImGui::TextColored(confirmed?green:amber,"분할 상태: %s",confirmed?"확정됨":"전체 검수 후 분할을 확정하세요");
    ImGui::TextUnformatted("COCO 분할 마스크 + COCO 검출 박스 + YOLO 검출 박스");ImGui::TextWrapped("박스 1px 보정은 검출용 COCO와 YOLO에 반영합니다. 분할용 COCO는 원래 마스크와 외접 박스를 보존합니다.");ImGui::Checkbox("공 잘라낸 PNG 함께 저장",&crops_);hint("YOLO 세그멘테이션 형식은 지원하지 않습니다.");
    ImGui::BeginDisabled(!confirmed||busy()||exportStatus_.value("status","")=="running");if(primary("학습 데이터 내보내기")){const auto client=client_;bool crops=crops_;launch("내보내기 시작",[client,crops]{return JobResult{client.post("/api/native/export",{{"crops",crops}}),{}};},[this](JobResult result){exportStatus_=result.data;message_="검수·분할된 데이터 내보내기 시작";});}ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("분할 확인"))showSplit();
    auto state=exportStatus_.value("status","idle");ImGui::Text("진행: %s",state.c_str());int total=exportStatus_.value("total",0),done=exportStatus_.value("done",0);if(total)ImGui::ProgressBar(float(done)/total,{-1,16});
    if(exportStatus_.contains("result"))ImGui::TextWrapped("완료 폴더: %s",exportStatus_["result"].value("path","").c_str());if(exportStatus_.contains("error"))ImGui::TextColored(amber,"%s",exportStatus_.value("error","").c_str());
}
void Application::selectForCapture(int camera,int rank){if(frames_.empty()||camera<0||camera>=int(frames_.size()))return;const auto order=sorted(frames_[camera]);if(rank>=0&&rank<int(order.size()))select(camera,order[rank]);}
// One step per call while the app is idle. Returns false when every step has passed; throws on a failed expectation.
bool Application::selfCheckStep(int step) {
    auto expect=[](bool condition,const char* what){if(!condition)throw std::runtime_error(std::string("self-test: ")+what);};
    static int checkedScene=-1;static Json before;
    switch(step){
    case 0:{const auto order=sorted(frames_.at(0));expect(!order.empty(),"scene has no ball in the first camera");checkedScene=scene_;select(0,order.front());return true;}
    case 1:{expect(editor_&&camera_==0&&selected_>=0,"ball was not selected");expect(current().scale>2,"selected ball was not zoomed");before=current().annotation["boxes"][selected_];nudge(0,true);return true;}
    case 2:{const auto& box=current().annotation["boxes"][selected_];const auto was=detectionBox(before),now=detectionBox(box);
        expect(now.y==was.y-1&&now.height==was.height+1&&now.x==was.x&&now.width==was.width,"top edge did not grow by one pixel");
        expect(box.at("bbox")==before.at("bbox")&&box.value("segmentation",Json())==before.value("segmentation",Json()),"box correction changed the mask");
        expect(current().member.value("status","")=="editing","an edited image must leave the confirmed state");nudge(0,false);return true;}
    case 3:{expect(detectionBox(current().annotation["boxes"][selected_])==detectionBox(before),"shrinking did not restore the box");
        const BallRef was{camera_,selected_};cycle(1);expect(camera_!=was.camera||selected_!=was.index,"Tab did not move to another ball");return true;}
    case 4:review(Scope::Scene,false);return true;
    case 5:{for(const auto& member:catalog_["scenes"][checkedScene]["members"])expect(!member.value("selected",true)||member.value("reviewed",false),"scene confirm left an image unconfirmed");
        expect(scene_!=checkedScene&&!editor_,"confirm did not advance to the next unreviewed scene");return false;}
    }
    return false;
}
void Application::refreshExports(){
    if(busy())return;const auto client=client_;
    launch("내보내기 기록 읽기",[client]{return JobResult{client.get("/api/native/exports"),{}};},[this](JobResult result){exports_=result.data.value("exports",Json::array());});
}
void Application::keys() {
    auto& io=ImGui::GetIO();
    if(page_!=Page::Review||io.WantTextInput||frames_.empty())return;
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
    if(pressed(ImGuiKey_Delete)){checkpoint();objects.erase(objects.begin()+selected_);selected_=-1;current().scale=0;changed();}
}
void Application::topBar() {
    const float width=ImGui::GetContentRegionAvail().x,height=48;const ImVec2 origin=ImGui::GetCursorScreenPos();
    ImGui::Dummy({width,height});const ImVec2 after=ImGui::GetCursorScreenPos();auto draw=ImGui::GetWindowDrawList();
    draw->AddLine({origin.x,origin.y+height-.5f},{origin.x+width,origin.y+height-.5f},theme::line);
    draw->AddCircle({origin.x+21,origin.y+24},7,theme::accent,0,1.5f);draw->AddCircleFilled({origin.x+21,origin.y+24},3.5f,theme::accent);
    theme::write(draw,{origin.x+36,origin.y+14},theme::text,"Boccia Labeler",15,theme::bold);
    const float brand=36+theme::extent("Boccia Labeler",15,theme::bold).x+14;
    draw->AddLine({origin.x+brand,origin.y+15},{origin.x+brand,origin.y+33},theme::line);
    theme::write(draw,{origin.x+brand+14,origin.y+15},theme::text2,"보치아 공 라벨 프로젝트");
    static const char* names[]={"데이터 준비","검수","분할","내보내기"};
    float total=0,widths[4];
    for(int i=0;i<4;++i){widths[i]=12+18+7+theme::extent(names[i]).x+12;total+=widths[i]+(i?30:0);}
    float x=origin.x+(width-total)/2;
    for(int i=0;i<4;++i){
        if(i){draw->AddLine({x+6,origin.y+24},{x+24,origin.y+24},theme::line2);x+=30;}
        const bool on=int(page_)==i,done=i<int(page_);
        ImGui::SetCursorScreenPos({x,origin.y+9});ImGui::PushID(i);
        if(ImGui::InvisibleButton("##step",{widths[i],30})&&!on)openPage(Page(i));
        const bool hovered=ImGui::IsItemHovered();ImGui::PopID();
        if(on||hovered)draw->AddRectFilled({x,origin.y+9},{x+widths[i],origin.y+39},on?theme::accentSoft:theme::bg2,6);
        const ImVec2 badge{x+12+9,origin.y+24};
        if(on)draw->AddCircleFilled(badge,9,theme::accent);else if(done)draw->AddCircleFilled(badge,9,theme::okSoft);else draw->AddCircle(badge,8.5f,theme::line2);
        if(done)theme::check(draw,badge,9,theme::ok);
        else{const std::string number=std::to_string(i+1);theme::write(draw,{badge.x-theme::extent(number,11,theme::bold).x/2,badge.y-7},on?theme::ink:theme::text2,number,11,theme::bold);}
        theme::write(draw,{x+12+18+7,origin.y+15},on?theme::text:theme::text2,names[i]);
        x+=widths[i];
    }
    const int target=catalog_.is_object()?catalog_.value("target_images",0):0,done=catalog_.is_object()?catalog_.value("reviewed_target_images",0):0;
    const std::string count=grouped(done)+" / "+grouped(target);
    const std::string saved=busy()?busyLabel_:!frames_.empty()&&current().dirty?"저장 대기":"자동 저장됨";
    float right=origin.x+width-14;
    right-=theme::extent(saved,theme::caption).x;theme::write(draw,{right,origin.y+16},busy()?theme::hold:theme::text3,saved,theme::caption);
    right-=18+120;draw->AddRectFilled({right,origin.y+22},{right+120,origin.y+26},theme::bg4,2);
    if(target&&done)draw->AddRectFilled({right,origin.y+22},{right+std::max(2.f,120.f*done/target),origin.y+26},theme::ok,2);
    right-=10+theme::extent(count,theme::caption,theme::mono).x;theme::write(draw,{right,origin.y+16},theme::text,count,theme::caption,theme::mono);
    right-=8+theme::extent("확인한 이미지",theme::caption).x;theme::write(draw,{right,origin.y+16},theme::text2,"확인한 이미지",theme::caption);
    ImGui::SetCursorScreenPos(after);
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
        const float width=ImGui::GetContentRegionAvail().x,list=width<1500?232.f:300.f,side=width<1500?296.f:344.f;
        ImGui::BeginChild("QueuePane",{list,body});queue();ImGui::EndChild();ImGui::SameLine(0,0);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,theme::bg0);
        ImGui::BeginChild("ReviewPane",{width-list-side,body},0,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);reviewPane();ImGui::EndChild();
        ImGui::PopStyleColor();ImGui::SameLine(0,0);
        ImGui::BeginChild("SidePane",{side,body});sidePanel();ImGui::EndChild();
        keys();
    }
    else{
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{28,20});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8,8});ImGui::PushStyleColor(ImGuiCol_ChildBg,theme::bg0);
        ImGui::BeginChild("StagePane",{0,body},ImGuiChildFlags_AlwaysUseWindowPadding);
        if(page_==Page::Prepare)preparePage();else if(page_==Page::Split)splitPage();else exportPage();
        ImGui::EndChild();ImGui::PopStyleColor();ImGui::PopStyleVar(2);
    }
    statusBar();
    ImGui::PopStyleVar();
    ImGui::End();
    if(later_){auto action=std::move(later_);later_=nullptr;action();}
}
}
