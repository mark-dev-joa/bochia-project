#include "native_labeler_app.hpp"
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
Application::~Application() { if(task_.valid()) task_.wait(); if(config_.rememberUi){try{rememberPosition();}catch(...){}} releaseFrames(); }
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
    catalog_=std::move(value);
    for(const auto& recording:catalog_["recordings"]) {
        auto id=recording.at("id").get<std::string>();
        if(!groups_.contains(id))groups_[id]=id;
        if(!splits_.contains(id))splits_[id]=catalog_["summary"]["splits"].value(id,"unassigned");
    }
    if(catalog_.value("split_confirmed",false))groups_=catalog_.at("groups");
}
void Application::updateSummary(const Json& summary) {
    catalog_["summary"]=summary; catalog_["split_confirmed"]=false;
    int target=0,done=0;
    for(auto& scene:catalog_["scenes"])for(auto& member:scene["members"]) {
        const auto key=member.at("key").get<std::string>();
        if(summary["frames"].contains(key)) {const auto& item=summary["frames"][key];member["selected"]=item.value("selected",true);member["reviewed"]=item.value("reviewed",false);member["status"]=item.value("review_status","draft");member["count"]=item.value("count",0);}
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
    },[this,index,editor,camera,token](JobResult result){if(token!=generation_)return;releaseFrames();frames_=std::move(result.frames);scene_=index;camera_=std::clamp(camera,0,int(frames_.size())-1);selected_=current().annotation["boxes"].empty()?-1:0;editor_=editor;page_=Page::Review;scrollToCurrent_=true;cancel();message_="원본과 저장 라벨 표시 중";});
}
bool Application::pending() const { return !proposal_.is_null()||!polygon_.empty()||!points_.empty()||dragging_; }
bool Application::guard(){if(busy()){message_="처리가 끝난 뒤 이동하세요. 화면 확대·이동은 계속할 수 있습니다.";return false;}if(pending()){message_="미확정 윤곽을 반영하거나 취소하세요.";return false;}return true;}
void Application::navigate(int index){if(!guard())return;if(!frames_.empty()&&current().dirty){save([this,index]{loadScene(index);});return;}loadScene(index);}
void Application::showEditor(int camera){if(frames_.empty()||!guard())return;bool entering=!editor_;switchCamera(camera);editor_=true;page_=Page::Review;if(entering&&!frames_.empty())current().scale=0;}
void Application::switchCamera(int camera) {
    if(camera<0||camera>=int(frames_.size())||!guard())return;
    if(current().dirty){save([this,camera]{switchCamera(camera);});return;}
    camera_=camera;selected_=current().annotation["boxes"].empty()?-1:0;current().repaint=true;cancel();
}
void Application::checkpoint(){auto& f=current();f.history.push_back(f.annotation);if(f.history.size()>40)f.history.erase(f.history.begin());}
void Application::changed(){auto& f=current();f.dirty=true;f.repaint=true;f.annotation["reviewed"]=false;f.annotation["review_status"]="editing";f.member["reviewed"]=false;f.member["status"]="editing";catalog_["split_confirmed"]=false;changed_=Clock::now();message_="수정됨 · 자동 저장 대기";}
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
    if(points_.empty()) {if(negative){message_="첫 점은 공 내부에 클릭하세요.";return;}replacement_=hit(current(),point);if(replacement_<0&&replaceSelected_)replacement_=selected_;if(replacement_>=0)selected_=replacement_;}
    if(points_.size()>=32){message_="점은 최대 32개입니다. 현재 결과를 반영하거나 취소하세요.";return;}
    points_.push_back({{"x",point.x},{"y",point.y},{"label",negative?0:1}});
    auto payload=annotationPayload(current().annotation,catalog_["summary"]["revision"].get<int>());payload["points"]=points_;payload["radius"]=radius_;
    const auto key=current().annotation.at("key").get<std::string>();const auto client=client_;const auto token=generation_;
    launch("EfficientSAM으로 공 윤곽 분리",[client,payload]{return JobResult{client.post("/api/assist/segment",payload),{}};},[this,key,token](JobResult result){if(token!=generation_||current().annotation.value("key","")!=key)return;proposal_=result.data.at("proposals").at(0);current().repaint=true;message_="실제 분할 결과 · Enter 반영 / Esc 취소";});
}
void Application::polygonProposal(){
    if(busy()||polygon_.size()<3)return;Json points=Json::array();for(auto point:polygon_)points.push_back({point.x,point.y});
    auto payload=annotationPayload(current().annotation,catalog_["summary"]["revision"].get<int>());payload["points"]=points;const auto client=client_;const auto token=generation_;
    launch("수동 폴리곤 마스크 만들기",[client,payload]{return JobResult{client.post("/api/assist/polygon",payload),{}};},[this,token](JobResult result){if(token!=generation_)return;proposal_=result.data.at("proposals").at(0);polygon_.clear();current().repaint=true;message_="폴리곤 미리보기 · Enter로 반영";});
}
void Application::review(bool next,bool hold,int member) {
    if(!guard()||frames_.empty())return;
    if(current().dirty){save([this,next,hold,member]{review(next,hold,member);});return;}
    Json keys=Json::array();
    if(member>=0)keys.push_back(frames_.at(member).annotation.at("key"));
    else if(editor_)keys.push_back(current().annotation.at("key"));
    else for(auto& frame:frames_)if(frame.member.value("selected",true)&&(!hold||!frame.member.value("reviewed",false)))keys.push_back(frame.annotation.at("key"));
    if(keys.empty())return;
    const auto client=client_;Json payload={{"keys",keys},{"status",hold?"hold":"reviewed"},{"expected_revision",catalog_["summary"]["revision"]}};
    launch(hold?"보류 상태 저장":"검수 완료 저장",[client,payload]{return JobResult{client.post("/api/native/review",payload),{}};},[this,next,hold,keys](JobResult result){updateSummary(result.data.at("summary"));for(auto& f:frames_)if(std::find(keys.begin(),keys.end(),f.annotation["key"])!=keys.end()){f.annotation["reviewed"]=!hold;f.annotation["review_status"]=hold?"hold":"reviewed";}message_=hold?"보류 저장 완료":"검수 완료 저장됨";if(next)nextUnreviewed(hold);});
}
void Application::nextUnreviewed(bool skipCurrent){
    if(editor_&&!skipCurrent)for(int offset=1;offset<int(frames_.size());++offset){int slot=(camera_+offset)%int(frames_.size());auto& member=frames_[slot].member;if(member.value("selected",true)&&!member.value("reviewed",false)){switchCamera(slot);message_="같은 장면의 다음 카메라 검수";return;}}
    for(int offset=1;offset<=int(catalog_["scenes"].size());++offset){int index=(scene_+offset)%int(catalog_["scenes"].size());for(auto& member:catalog_["scenes"][index]["members"])if(member.value("selected",true)&&!member.value("reviewed",false)){loadScene(index);return;}}
    editor_=false;message_="대상 전체 검수 완료 · 데이터 분할 단계로 이동하세요.";
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
const char* Application::statusLabel(const Json& member){const auto state=member.value("status","draft");return state=="reviewed"?"검수 완료":state=="hold"?"보류":state=="editing"?"수정됨":"미검수";}
void Application::queue(){
    ImGui::TextUnformatted("프레임 데이터");hint("한 행 = 한 장면 · 카메라 함께 보기");
    int target=catalog_.value("target_images",0),done=catalog_.value("reviewed_target_images",0);
    ImGui::Text("%d / %d 이미지 완료",done,target);ImGui::ProgressBar(target?float(done)/target:0,{ -1,5 },"");
    if(ImGui::Checkbox("대표 프레임만",&representatives_)){}
    const char* filters[]={"전체","미검수","완료","보류"};ImGui::SetNextItemWidth(-1);ImGui::Combo("##statusFilter",&filter_,filters,4);ImGui::Separator();
    const float buttons=86;ImGui::BeginChild("SceneList",{0,ImGui::GetContentRegionAvail().y-buttons},false);
    for(int i=0;i<int(catalog_["scenes"].size());++i){const auto& scene=catalog_["scenes"][i];int reviewed=0,count=0,total=0;bool held=false,editing=false;
        for(const auto& member:scene["members"]){if(representatives_&&!member.value("selected",true))continue;++total;reviewed+=member.value("reviewed",false);count+=member.value("count",0);held|=member.value("status","")=="hold";editing|=member.value("status","")=="editing";}
        if(!total||(filter_==1&&reviewed==total)||(filter_==2&&reviewed!=total)||(filter_==3&&!held))continue;
        const char* state=held?"보류":reviewed==total?"검수 완료":reviewed||editing?"검수 중":"미검수";
        ImGui::PushID(i);auto pos=ImGui::GetCursorScreenPos();bool clicked=ImGui::Selectable("##scene",i==scene_,0,{0,69});auto draw=ImGui::GetWindowDrawList();
        draw->AddText({pos.x+7,pos.y+6},IM_COL32(220,233,241,255),title(scene).c_str());std::string label=std::string(state)+" · "+std::to_string(reviewed)+"/"+std::to_string(total)+" 대상 완료";
        draw->AddText({pos.x+7,pos.y+29},held?IM_COL32(241,187,91,255):reviewed==total?IM_COL32(104,231,195,255):IM_COL32(153,177,192,255),label.c_str());
        label="영상 "+std::to_string(total)+" · 주석 "+std::to_string(count);draw->AddText({pos.x+7,pos.y+49},IM_COL32(126,151,166,255),label.c_str());if(i==scene_&&scrollToCurrent_){ImGui::SetScrollHereY(.5f);scrollToCurrent_=false;}if(clicked)navigate(i);ImGui::PopID();
    }
    ImGui::EndChild();ImGui::Separator();if(primary("검수 후 데이터 분할 →",{-1,30}))showSplit();if(ImGui::Button("저장 상태 새로고침",{-1,27}))refresh();
}
void Application::uploadTexture(GLuint& texture,const cv::Mat& pixels,bool rgba){
    if(!texture)glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glTexImage2D(GL_TEXTURE_2D,0,rgba?GL_RGBA:GL_RGB,pixels.cols,pixels.rows,0,rgba?GL_RGBA:GL_BGR,GL_UNSIGNED_BYTE,pixels.data);
}
void Application::repaint(FrameView& f){
    cv::Mat display;f.image.convertTo(display,CV_8UC3,f.member.value("kind","")=="컬러"?brightness_:1.12);uploadTexture(f.texture,display);
    cv::Mat overlay(f.image.size(),CV_8UC4,cv::Scalar(0,0,0,0));
    auto paint=[&](const cv::Mat& mask,bool active){for(int y=0;y<mask.rows;++y){auto in=mask.ptr<uchar>(y);auto out=overlay.ptr<cv::Vec4b>(y);for(int x=0;x<mask.cols;++x)if(in[x])out[x]=active?cv::Vec4b(101,231,196,86):cv::Vec4b(89,185,225,74);}};
    for(int i=0;i<int(f.annotation["boxes"].size());++i){const auto& box=f.annotation["boxes"][i];if(&f==&current()&&replacement_==i&&!proposal_.is_null())continue;if(box.contains("segmentation"))paint(decodeMask(box["segmentation"],f.image.cols,f.image.rows),&f==&current()&&i==selected_);}
    if(&f==&current()&&!proposal_.is_null()){if(!brushMask_.empty())paint(brushMask_,true);else if(proposal_.contains("segmentation"))paint(decodeMask(proposal_["segmentation"],f.image.cols,f.image.rows),true);}
    uploadTexture(f.overlay,overlay,true);f.repaint=false;
}
int Application::hit(const FrameView& frame,cv::Point2d point)const {for(int i=int(frame.annotation["boxes"].size())-1;i>=0;--i)if(detectionBox(frame.annotation["boxes"][i]).contains(point))return i;return -1;}
void Application::fit(FrameView& f,ImVec2 size,bool detail){
    cv::Rect2d extent{0,0,double(f.image.cols),double(f.image.rows)};
    if(detail&&!f.annotation["boxes"].empty()){extent=detectionBox(f.annotation["boxes"][0]);for(auto& box:f.annotation["boxes"])extent|=detectionBox(box);extent.x-=38;extent.y-=38;extent.width+=76;extent.height+=76;}
    f.scale=float(std::min(size.x/extent.width,size.y/extent.height));f.offsetX=float(size.x*.5-(extent.x+extent.width*.5)*f.scale);f.offsetY=float(size.y*.5-(extent.y+extent.height*.5)*f.scale);
}
void Application::imageCanvas(FrameView& f,ImVec2 size,bool editable,bool detail){
    if(f.image.empty()||size.x<10||size.y<10)return;if(f.repaint)repaint(f);
    float scale=f.scale,ox=f.offsetX,oy=f.offsetY;
    if(!editable||scale<=0){fit(f,size,detail||editable);scale=f.scale;ox=f.offsetX;oy=f.offsetY;}
    auto start=ImGui::GetCursorScreenPos();ImGui::PushID(f.annotation.at("key").get<std::string>().c_str());ImGui::PushID(detail?1:0);
    ImGui::InvisibleButton("image",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle|ImGuiButtonFlags_MouseButtonRight);
    bool hovered=ImGui::IsItemHovered();auto draw=ImGui::GetWindowDrawList();draw->AddRectFilled(start,{start.x+size.x,start.y+size.y},IM_COL32(7,13,19,255));draw->PushClipRect(start,{start.x+size.x,start.y+size.y},true);
    ImVec2 a{start.x+ox,start.y+oy},b{a.x+f.image.cols*scale,a.y+f.image.rows*scale};draw->AddImage(ImTextureRef(ImTextureID(f.texture)),a,b);if(masks_)draw->AddImage(ImTextureRef(ImTextureID(f.overlay)),a,b);
    auto outline=[&](const Json& box,bool active){auto r=detectionBox(box);ImU32 color=active?IM_COL32(107,239,195,255):IM_COL32(111,192,228,255);
        if(boxes_)draw->AddRect({a.x+float(r.x)*scale,a.y+float(r.y)*scale},{a.x+float(r.x+r.width)*scale,a.y+float(r.y+r.height)*scale},color,0,0,active?2.f:1.f);
        if(contour_&&box.contains("segmentation")&&editable){std::vector<std::vector<cv::Point>> contours;auto mask=decodeMask(box["segmentation"],f.image.cols,f.image.rows);cv::findContours(mask,contours,cv::RETR_LIST,cv::CHAIN_APPROX_SIMPLE);for(auto& contour:contours){std::vector<ImVec2> pts;for(auto p:contour)pts.push_back({a.x+p.x*scale,a.y+p.y*scale});if(pts.size()>2)draw->AddPolyline(pts.data(),int(pts.size()),color,ImDrawFlags_Closed,1);}}
    };
    for(int i=0;i<int(f.annotation["boxes"].size());++i)if(!editable||replacement_!=i||proposal_.is_null())outline(f.annotation["boxes"][i],editable&&selected_==i);
    if(editable&&!proposal_.is_null())outline(proposal_,true);
    if(editable){for(size_t i=0;i<polygon_.size();++i){auto p=polygon_[i];ImVec2 v{a.x+float(p.x)*scale,a.y+float(p.y)*scale};draw->AddCircleFilled(v,3,IM_COL32(250,207,119,255));if(i){auto q=polygon_[i-1];draw->AddLine({a.x+float(q.x)*scale,a.y+float(q.y)*scale},v,IM_COL32(250,207,119,255),2);}}
        for(auto& point:points_){draw->AddCircleFilled({a.x+point["x"].get<float>()*scale,a.y+point["y"].get<float>()*scale},4,point["label"]==1?IM_COL32(99,242,173,255):IM_COL32(249,117,112,255));}
    }
    draw->PopClipRect();
    auto mouse=ImGui::GetIO().MousePos;cv::Point2d point{(mouse.x-a.x)/scale,(mouse.y-a.y)/scale};bool inside=point.x>=0&&point.y>=0&&point.x<f.image.cols&&point.y<f.image.rows;
    if(!editable){if(ImGui::IsItemClicked(ImGuiMouseButton_Left)&&guard()){int index=int(&f-frames_.data());int target=hit(f,point);showEditor(index);if(target>=0)selected_=target;current().repaint=true;}f.scale=0;}
    else {
        auto& io=ImGui::GetIO();bool pan=ImGui::IsKeyDown(ImGuiKey_Space)||ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        if(hovered&&io.MouseWheel!=0){float updated=std::clamp(scale*std::exp(io.MouseWheel*.16f),.1f,20.f);f.offsetX=mouse.x-start.x-float(point.x)*updated;f.offsetY=mouse.y-start.y-float(point.y)*updated;f.scale=updated;}
        if(ImGui::IsItemActive()&&pan&&ImGui::IsMouseDragging(ImGuiMouseButton_Left,0)){f.offsetX+=io.MouseDelta.x;f.offsetY+=io.MouseDelta.y;}
        if(ImGui::IsItemActive()&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle,0)){f.offsetX+=io.MouseDelta.x;f.offsetY+=io.MouseDelta.y;}
        if(hovered&&!pan&&inside&&!busy()&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
            if(tool_==Tool::Select&&!pending()){selected_=hit(f,point);f.repaint=true;}
            else if(tool_==Tool::Ai)segment(point,io.KeyShift);
            else if(tool_==Tool::Polygon&&proposal_.is_null()){if(polygon_.empty())replacement_=replaceSelected_?selected_:-1;polygon_.push_back(point);}
            else if(tool_==Tool::Box&&!pending()){dragStart_=point;dragging_=true;replacement_=replaceSelected_?selected_:-1;}
            else if(tool_==Tool::Brush&&selected_>=0){auto& box=f.annotation["boxes"][selected_];if(!box.contains("segmentation")){message_="마스크가 있는 공을 선택하세요.";}else if(proposal_.is_null()||!brushMask_.empty()){if(proposal_.is_null()){proposal_=box;brushMask_=decodeMask(box["segmentation"],f.image.cols,f.image.rows);replacement_=selected_;}dragging_=true;lastBrush_=point;cv::circle(brushMask_,point,int(brushSize_/2),cv::Scalar(erase_||io.KeyShift?0:1),-1);f.repaint=true;}}
        }
        if(dragging_&&inside&&ImGui::IsMouseDown(ImGuiMouseButton_Left)&&!brushMask_.empty()){cv::line(brushMask_,lastBrush_,point,cv::Scalar(erase_||io.KeyShift?0:1),int(brushSize_),cv::LINE_8);lastBrush_=point;f.repaint=true;}
        if(dragging_&&tool_==Tool::Box){draw->PushClipRect(start,{start.x+size.x,start.y+size.y},true);draw->AddRect({a.x+float(dragStart_.x)*scale,a.y+float(dragStart_.y)*scale},mouse,IM_COL32(245,208,132,255),0,0,2);draw->PopClipRect();}
        if(dragging_&&ImGui::IsMouseReleased(ImGuiMouseButton_Left)){dragging_=false;if(tool_==Tool::Box){point.x=std::clamp(point.x,0.,double(f.image.cols));point.y=std::clamp(point.y,0.,double(f.image.rows));double x=std::min(point.x,dragStart_.x),y=std::min(point.y,dragStart_.y),w=std::abs(point.x-dragStart_.x),h=std::abs(point.y-dragStart_.y);if(w>=1&&h>=1){if(replacement_>=0){proposal_=f.annotation["boxes"][replacement_];proposal_["detection_bbox"]={x,y,w,h};}else proposal_={{"bbox",{x,y,w,h}},{"occluded",false},{"source","manual"}};}}
            else if(!brushMask_.empty()){try{proposal_=maskAnnotation(brushMask_,proposal_,"manual_brush");}catch(const std::exception& error){error_=error.what();}}f.repaint=true;}
    }
    ImGui::PopID();ImGui::PopID();
}
void Application::overview(){
    ImGui::TextUnformatted(title(catalog_["scenes"][scene_]).c_str());ImGui::SameLine();hint("컬러 1 + 모노 2 · 클릭해서 편집");
    ImGui::Checkbox("마스크",&masks_);ImGui::SameLine();ImGui::Checkbox("박스",&boxes_);ImGui::Separator();
    float height=ImGui::GetContentRegionAvail().y-88;
    if(ImGui::BeginTable("Cameras",3,ImGuiTableFlags_SizingStretchSame)){
        for(int slot=0;slot<3;++slot){ImGui::TableNextColumn();ImGui::PushID(slot);ImGui::BeginChild("CameraCard",{0,height},ImGuiChildFlags_Borders);
            if(slot>=int(frames_.size())){hint("매칭된 카메라 없음");hint("수신 시각과 원본을 확인하세요.");}
            else {auto& frame=frames_[slot];ImGui::Text("%s · %s",frame.member.at("name").get<std::string>().c_str(),frame.member.at("kind").get<std::string>().c_str());ImGui::SameLine();ImGui::TextColored(frame.member.value("reviewed",false)?green:muted,"%s",statusLabel(frame.member));
                if(!frame.member.value("selected",true))ImGui::TextColored(amber,"반복 프레임 · 검수 대상 제외");else hint("전체 영상 · 클릭해서 편집");
                auto size=ImGui::GetContentRegionAvail();imageCanvas(frame,{size.x,std::max(120.f,size.y-190)},false);hint("공 주변 확대");imageCanvas(frame,{size.x,95},false,true);
                ImGui::Text("공 %d개 · 수신 Δ %+.3f ms",int(frame.annotation["boxes"].size()),frame.member.value("delta_ms",0.));
                ImGui::BeginDisabled(busy());if(ImGui::Button("보류",{53,29}))review(false,true,slot);ImGui::SameLine();ImGui::BeginDisabled(frame.member.value("reviewed",false));if(primary(frame.annotation["boxes"].empty()?"공 없음 확인":"확인 완료",{-1,29}))review(false,false,slot);ImGui::EndDisabled();ImGui::EndDisabled();
            }ImGui::EndChild();ImGui::PopID();
        }ImGui::EndTable();
    }
    ImGui::Separator();hint("수신 시각 기준 묶음 · 노출 동기화는 별도 확인");
    if(ImGui::Button("장면 보류"))review(true,true);ImGui::SameLine();if(primary("현재 장면 확인 완료 · 다음  E"))review(true);ImGui::SameLine();if(ImGui::Button("다음 장면 →"))navigate(std::min(scene_+1,int(catalog_["scenes"].size())-1));
}
void Application::editor(){
    ImGui::BeginChild("EditorCanvasPane",{ImGui::GetContentRegionAvail().x-274,0},false);
    if(ImGui::Button("← 3개 보기  Esc")){if(guard()){if(current().dirty)save([this]{editor_=false;});else editor_=false;}}ImGui::SameLine();ImGui::TextUnformatted(title(catalog_["scenes"][scene_]).c_str());
    for(int i=0;i<int(frames_.size());++i){if(i)ImGui::SameLine();ImGui::PushID(i);if(ImGui::Button((frames_[i].member.at("name").get<std::string>()+" · "+frames_[i].member.at("kind").get<std::string>()).c_str()))switchCamera(i);ImGui::PopID();}
    ImGui::Separator();const char* labels[]={"선택 V","AI 클릭 S","폴리곤 P","브러시 R","박스 B"};for(int i=0;i<5;++i){if(i)ImGui::SameLine();ImGui::PushID(i);if(ImGui::Selectable(labels[i],int(tool_)==i,0,{i==2?82.f:75.f,25})){if(guard())tool_=Tool(i);}ImGui::PopID();}
    ImGui::BeginDisabled(pending()||busy());if(ImGui::RadioButton("새 공 추가",!replaceSelected_))replaceSelected_=false;ImGui::SameLine();ImGui::BeginDisabled(selected_<0);if(ImGui::RadioButton("선택 공 보정",replaceSelected_))replaceSelected_=true;ImGui::EndDisabled();ImGui::EndDisabled();
    ImGui::Checkbox("마스크",&masks_);ImGui::SameLine();ImGui::Checkbox("윤곽",&contour_);ImGui::SameLine();ImGui::Checkbox("박스",&boxes_);ImGui::SameLine();if(ImGui::SmallButton("전체 영상")){auto size=ImGui::GetContentRegionAvail();size.y-=87;fit(current(),size,false);}ImGui::SameLine();if(ImGui::SmallButton("공 확대"))current().scale=0;
    auto size=ImGui::GetContentRegionAvail();imageCanvas(current(),{size.x,std::max(100.f,size.y-75)},true);
    ImGui::Separator();if(ImGui::Button("취소  Esc"))cancel();ImGui::SameLine();ImGui::BeginDisabled(proposal_.is_null()&&polygon_.size()<3);if(primary("윤곽 반영  Enter"))apply();ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("되돌리기 Ctrl+Z"))undo();
    hint("휠 확대 · Space+드래그 이동 · Shift+클릭 제외/지우기");ImGui::EndChild();ImGui::SameLine();ImGui::BeginChild("Properties",{0,0},ImGuiChildFlags_Borders);properties();ImGui::EndChild();
}
void Application::properties(){
    auto& frame=current();auto& objects=frame.annotation["boxes"];ImGui::Text("공 목록 · %d개",int(objects.size()));
    ImGui::BeginChild("Objects",{0,std::min(90.f,26.f*float(objects.size()+1))},true);for(int i=0;i<int(objects.size());++i){auto label="공 "+std::to_string(i+1)+(objects[i].contains("segmentation")?" · 마스크":" · 박스");if(ImGui::Selectable(label.c_str(),selected_==i)&&!pending()){selected_=i;frame.repaint=true;}}ImGui::EndChild();
    ImGui::BeginChild("ObjectDetails",{0,std::max(100.f,ImGui::GetContentRegionAvail().y-112)},false);
    if(selected_>=0&&selected_<int(objects.size())){auto& box=objects[selected_];ImGui::Text("공 %02d",selected_+1);auto bounds=detectionBox(box);hint("박스 1픽셀 보정 · 원본 px");ImGui::Text("X %.1f  Y %.1f",bounds.x,bounds.y);ImGui::Text("너비 %.1f  높이 %.1f",bounds.width,bounds.height);
        const char* edges[]={"위","아래","왼쪽","오른쪽"};ImGui::BeginDisabled(pending()||busy());
        if(ImGui::BeginTable("BoxEdges",3,ImGuiTableFlags_SizingStretchSame)){for(int edge=0;edge<4;++edge){ImGui::TableNextColumn();ImGui::TextUnformatted(edges[edge]);ImGui::TableNextColumn();ImGui::PushID(edge);if(ImGui::Button("+1 확장",{-1,24})){auto original=box;if(nudgeEdge(box,edge,true,frame.image.cols,frame.image.rows)){box=original;checkpoint();nudgeEdge(box,edge,true,frame.image.cols,frame.image.rows);boxes_=true;changed();}else message_="이미지 밖으로 확장할 수 없습니다.";}
            ImGui::TableNextColumn();if(ImGui::Button("-1 축소",{-1,24})){auto original=box;if(nudgeEdge(box,edge,false,frame.image.cols,frame.image.rows)){box=original;checkpoint();nudgeEdge(box,edge,false,frame.image.cols,frame.image.rows);boxes_=true;changed();}else message_="박스는 최소 1픽셀이어야 합니다.";}ImGui::PopID();}ImGui::EndTable();}
        if(ImGui::Button("보정 초기화",{-1,25})&&box.contains("detection_bbox")){checkpoint();box.erase("detection_bbox");changed();}hint("검출 박스만 변경 · 마스크 유지");
        bool occluded=box.value("occluded",false);if(ImGui::Checkbox("일부 가림",&occluded)){checkpoint();box["occluded"]=occluded;changed();}
        const char* colors[]={"미지정","빨강","파랑","흰색"};const char* values[]={"unknown","red","blue","white"};int color=0;for(int i=0;i<4;++i)if(box.value("color","unknown")==values[i])color=i;
        bool mono=frame.member.value("kind","")=="모노";ImGui::BeginDisabled(mono);ImGui::SetNextItemWidth(-1);if(ImGui::Combo("##color",&color,colors,4)){checkpoint();box["color"]=values[color];changed();}ImGui::EndDisabled();if(mono)hint("모노 색상은 미지정");
        if(ImGui::Button("선택 공 삭제",{-1,25})){checkpoint();objects.erase(objects.begin()+selected_);selected_=objects.empty()?-1:std::min(selected_,int(objects.size())-1);changed();}
        ImGui::EndDisabled();
    }else hint("공을 선택하거나 새 공을 표시하세요.");
    if(tool_==Tool::Ai){ImGui::SetNextItemWidth(-1);ImGui::SliderFloat("공 반지름",&radius_,3,80,"%.0f px");}
    if(tool_==Tool::Brush){ImGui::SetNextItemWidth(-1);ImGui::SliderFloat("브러시 크기",&brushSize_,2,30,"%.0f px");ImGui::Checkbox("지우개",&erase_);}
    ImGui::EndChild();ImGui::Separator();hint("보이는 공을 모두 확인하세요.");ImGui::BeginDisabled(pending()||busy());if(primary(objects.empty()?"공 없음 확인하고 다음 E":"완료하고 다음  E",{-1,29}))review(true);if(ImGui::Button("보류하고 다음",{-1,26}))review(true,true);ImGui::EndDisabled();
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
void Application::draw(){
    const auto viewport=ImGui::GetMainViewport();ImGui::SetNextWindowPos(viewport->WorkPos);ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Boccia Labeler",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(green,"Boccia Lab");ImGui::SameLine();hint("보치아 공 라벨링 · 로컬 프로젝트");
    const char* pages[]={"데이터 준비","라벨 검수","데이터 분할","내보내기"};for(int i=0;i<4;++i){if(i)ImGui::SameLine();if(ImGui::Selectable(pages[i],int(page_)==i,0,{110,28})){if(guard()){if(!frames_.empty()&&current().dirty)save([this,i]{page_=Page(i);});else page_=Page(i);}}}ImGui::Separator();
    if(!error_.empty()){ImGui::TextColored(amber,"%s",error_.c_str());if(ImGui::SmallButton("오류 메시지 닫기"))error_.clear();ImGui::SameLine();if(ImGui::SmallButton("다시 연결")){if(!busy()&&(frames_.empty()||!current().dirty))connect();else message_="현재 변경을 저장하거나 recovery 파일을 보관한 뒤 재실행하세요.";}ImGui::Separator();}
    if(!ready()){ImGui::TextWrapped("%s",message_.c_str());ImGui::TextUnformatted("기존 촬영과 자동 마스크를 읽고 있습니다.");}
    else {
        float body=ImGui::GetContentRegionAvail().y-32;
        ImGui::BeginChild("Workspace",{0,body},false);
        if(page_==Page::Review){ImGui::BeginChild("QueuePane",{218,0},ImGuiChildFlags_Borders);queue();ImGui::EndChild();ImGui::SameLine();ImGui::BeginChild("ReviewPane",{0,0},false);if(editor_)editor();else overview();ImGui::EndChild();}
        else if(page_==Page::Prepare)preparePage();else if(page_==Page::Split)splitPage();else exportPage();
        ImGui::EndChild();
        auto& io=ImGui::GetIO();if(page_==Page::Review&&!io.WantTextInput){
            if(ImGui::IsKeyPressed(ImGuiKey_E))review(true);if(ImGui::IsKeyPressed(ImGuiKey_Escape)){if(pending())cancel();else if(editor_&&guard()){if(current().dirty)save([this]{editor_=false;});else editor_=false;}}
            if(editor_){if(ImGui::IsKeyPressed(ImGuiKey_Enter))apply();if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_Z))undo();if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_S))save();
                if(!pending()&&!busy()){if(ImGui::IsKeyPressed(ImGuiKey_V))tool_=Tool::Select;if(ImGui::IsKeyPressed(ImGuiKey_S)&&!io.KeyCtrl)tool_=Tool::Ai;if(ImGui::IsKeyPressed(ImGuiKey_P))tool_=Tool::Polygon;if(ImGui::IsKeyPressed(ImGuiKey_R))tool_=Tool::Brush;if(ImGui::IsKeyPressed(ImGuiKey_B))tool_=Tool::Box;}
                if(ImGui::IsKeyPressed(ImGuiKey_Delete)&&selected_>=0&&guard()){checkpoint();auto& objects=current().annotation["boxes"];objects.erase(objects.begin()+selected_);selected_=objects.empty()?-1:std::min(selected_,int(objects.size())-1);changed();}
            }
            if(ImGui::IsKeyPressed(ImGuiKey_1))showEditor(0);if(ImGui::IsKeyPressed(ImGuiKey_2))showEditor(1);if(ImGui::IsKeyPressed(ImGuiKey_3))showEditor(2);
            if(ImGui::IsKeyPressed(ImGuiKey_RightArrow))navigate(std::min(scene_+1,int(catalog_["scenes"].size())-1));if(ImGui::IsKeyPressed(ImGuiKey_LeftArrow))navigate(std::max(0,scene_-1));
        }
    }
    ImGui::Separator();ImGui::TextColored(busy()?amber:muted,"%s",busy()?busyLabel_.c_str():message_.c_str());ImGui::End();
}
}
