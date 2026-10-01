// Application members: floor alignment uses the saved rig's serial order, not connection order.
bool showOrigin=false,originConfirmed=false,originSaved=false,savingOrigin=false,originDirty=true;
int originTurns=0;double originHeight=0;
int originMethod=0,originRectangleCamera=0;
bool originOptionsLoaded=false,originRectangleFrozen=false;
RectangleOriginConfig originRectangle;
std::vector<cv::Point2f> originRectanglePoints;
std::string originMessage="측정 대기";
std::optional<Settings::SavedRig> originRig;
std::optional<BoardPose> originPose;
std::optional<WorldOrigin> originCandidate;
StereoDetection originDetected;
RigSample originSample;
RigCapture originCapture;
BackgroundTask<StereoDetection> originDetectTask;
BackgroundTask<BoardPose> originPoseTask;
std::array<Texture,3> originPreview;
std::array<uint64_t,3> originGenerations{};
std::array<uint64_t,3> originViewGenerations{};
ImVec2 originRectangleImageMin{},originRectangleImageSize{};
cv::Size originRectangleSourceSize;

void cancelOrigin(){originCapture.stop();originDetectTask.cancel();originPoseTask.cancel();}
void openOrigin(){
    cancelWork();cancelStereo();showCalibration=false;showStereo=false;showOrigin=true;
    if(originRig&&(!settings.rig||settings.rig->archive!=originRig->archive||!originRig->matches(settings.cameras))){
        resetOriginCapture();
        originMessage="보정값이 바뀌었습니다. 원점을 다시 측정하세요.";
    }
}
void resetOriginCapture(){
    cancelOrigin();originPose.reset();originCandidate.reset();originSample={};originDetected={};originRig.reset();
    originRectangleFrozen=false;originRectanglePoints.clear();originConfirmed=false;originSaved=false;originDirty=true;originGenerations={};
}
int rectangleCameraIndex()const{
    if(!settings.rig||originRectangleCamera<0||size_t(originRectangleCamera)>=settings.rig->serials.size())return -1;
    return deviceCamera(settings.rig->serials[originRectangleCamera]);
}
std::vector<int> originIndices()const{
    std::vector<int> indices;if(!settings.rig)return indices;
    for(const auto& serial:settings.rig->serials){
        auto it=std::find_if(cameras.begin(),cameras.end(),[&](const auto& c){return c->device.serial==serial;});
        if(it==cameras.end())return {};indices.push_back(int(it-cameras.begin()));
    }return indices;
}
std::string originReadiness()const{
    if(loadFailed)return "설정 파일을 읽지 못했습니다.";
    if(!settings.rig||!settings.rig->matches(settings.cameras))return "유효한 다중 카메라 상대 위치를 먼저 저장하세요.";
    if(originMethod==1){
        int i=rectangleCameraIndex();if(i<0||!cameras[i]->connected())return "선택한 카메라를 연결하세요.";
        if(!compatible(*cameras[i]))return "개별 보정과 현재 영상 조건이 다릅니다.";
        if(!cameras[i]->running())return "영상 수집을 시작하세요.";
        return {};
    }
    auto indices=originIndices();if(indices.size()!=settings.rig->serials.size())return "상대 보정에 사용한 모든 카메라를 연결하세요.";
    for(int i:indices){if(!compatible(*cameras[i]))return cameras[i]->device.serial+": 개별 보정과 현재 영상 조건이 다릅니다.";
        if(!cameras[i]->running())return cameras[i]->device.serial+": 영상 수집을 시작하세요.";}
    return {};
}
void beginOrigin(){try{
    auto readiness=originReadiness();if(!readiness.empty())throw std::runtime_error(readiness);
    cancelOrigin();originRig=*settings.rig;originPose.reset();originCandidate.reset();originSample={};originDetected={};
    originGenerations={};originConfirmed=false;originSaved=false;originDirty=true;
    originCapture.start(originRig->serials.size(),AutoCaptureConfig{10,1.0,1.5});
    originMessage="보드를 1초간 고정하세요.";
}catch(const std::exception& e){originMessage=e.what();}}
void rebuildOrigin(){try{
    if(!originPose||!originRig)return;
    originCandidate=originMethod==1?makeRectangleWorldOrigin(*originPose,originRectangle,originRig->calibration,originTurns,originHeight):
        makeWorldOrigin(*originPose,originRig->board,originRig->calibration,originTurns,originHeight);
    originConfirmed=false;originSaved=false;originDirty=true;
}catch(const std::exception& e){originCandidate.reset();originMessage=e.what();}}
void captureRectangleOrigin(){try{
    auto readiness=originReadiness();if(!readiness.empty())throw std::runtime_error(readiness);originRectangle.validate();
    int i=rectangleCameraIndex();auto frame=cameras[i]->latest();
    if(frame.image.empty()||frame.image.size()!=settings.cameras.at(cameras[i]->device.serial).calibration.imageSize)throw std::runtime_error("현재 영상 크기를 확인하세요.");
    if(seconds(Clock::now())-seconds(frame.receivedAt)>1.5||frame.receivedAt>Clock::now())throw std::runtime_error("최신 영상이 없습니다. 트리거와 Grab 상태를 확인하세요.");
    resetOriginCapture();originRig=*settings.rig;Observation observation;observation.image=frame.image.clone();observation.frameId=frame.id;
    observation.timestamp=frame.timestamp;observation.source="live:"+cameras[i]->device.serial;originSample.cameras.push_back(std::move(observation));
    originRectangleFrozen=true;originMessage="모서리 4개를 순서대로 선택하세요. 1→2 가로 / 2→3 세로";
}catch(const std::exception& e){originMessage=e.what();}}
void beginRectangleOrigin(){try{
    auto readiness=originReadiness();if(!readiness.empty())throw std::runtime_error(readiness);
    if(!originRectangleFrozen||originRectanglePoints.size()!=4||!originRig)return;
    if(originRig->archive!=settings.rig->archive||!originRig->matches(settings.cameras))throw std::runtime_error("보정값이 변경되었습니다. 다시 촬영하세요.");
    auto points=originRectanglePoints;auto rectangle=originRectangle;auto rig=originRig->calibration;size_t index=size_t(originRectangleCamera);
    auto intrinsics=settings.cameras.at(originRig->serials[index]).calibration;
    originCandidate.reset();originPose.reset();originConfirmed=false;originSaved=false;
    originPoseTask.start([points,rectangle,rig,index,intrinsics](std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("위치 보정을 취소했습니다.");return locateRectangle(points,rectangle,intrinsics,rig,index);});
    originMessage="계산 중...";
}catch(const std::exception& e){originMessage=e.what();}}
void pollOrigin(){
    try{if(auto d=originDetectTask.poll();d&&originCapture.running()&&originRig&&d->serials==originRig->serials){
        originDetected=std::move(*d);originDirty=true;std::vector<double> times;for(auto t:originDetected.receivedAt)times.push_back(seconds(t));
        if(originCapture.observe(originDetected.sample,originRig->board,{},times,seconds(Clock::now()))){
            originSample=originDetected.sample;originCapture.stop();
            auto sample=originSample;auto rig=originRig->calibration;auto b=originRig->board;std::vector<Calibration> intrinsics;
            for(const auto& serial:originRig->serials)intrinsics.push_back(settings.cameras.at(serial).calibration);
            originPoseTask.start([sample,rig,b,intrinsics](std::stop_token stop){return locateBoard(sample,b,intrinsics,rig,stop);});
            originMessage="계산 중...";
        }
    }}catch(const std::exception& e){cancelOrigin();originMessage=e.what();}
    try{if(auto pose=originPoseTask.poll()){
        originPose=std::move(*pose);rebuildOrigin();
        originMessage="측정 완료 · 저장 전";
    }}catch(const std::exception& e){originMessage=e.what();}
}
void scheduleOrigin(){
    if(!originCapture.running()||originDetectTask.pending()||!originRig)return;
    auto readiness=originReadiness();if(!readiness.empty()){cancelOrigin();originMessage=readiness;return;}
    auto indices=originIndices();std::vector<CameraFrame> frames;std::vector<double> times;std::vector<BrightnessOptions> lights;
    for(size_t i=0;i<indices.size();++i){auto frame=cameras[indices[i]]->latest();if(frame.image.empty()||frame.generation==originGenerations[i])return;
        frames.push_back(frame);times.push_back(seconds(frame.receivedAt));lights.push_back(savedBrightness(originRig->serials[i]));}
    if(!usableRigTiming(times,seconds(Clock::now())))return;
    for(size_t i=0;i<frames.size();++i)originGenerations[i]=frames[i].generation;
    auto b=originRig->board;auto serials=originRig->serials;
    originDetectTask.start([frames,times,lights,b,serials](std::stop_token stop){
        StereoDetection d;d.serials=serials;auto [lo,hi]=std::minmax_element(times.begin(),times.end());d.sample.receiptSkewMs=(*hi-*lo)*1000;
        for(size_t i=0;i<frames.size();++i){if(stop.stop_requested())return d;
            auto o=detectBoard(frames[i].image,b,lights[i]);o.frameId=frames[i].id;o.timestamp=frames[i].timestamp;o.source="live:"+serials[i];
            d.sample.cameras.push_back(std::move(o));d.receivedAt.push_back(frames[i].receivedAt);}
        return d;
    });
}
void saveOrigin(){try{
    if(!originCandidate||!originRig||!originConfirmed||busy()||loadFailed)return;
    if(!settings.rig||settings.rig->archive!=originRig->archive||!originRig->matches(settings.cameras))throw std::runtime_error("보정값이 변경되었습니다. 원점을 다시 측정하세요.");
    auto readiness=originReadiness();if(!readiness.empty())throw std::runtime_error(readiness);
    auto stamp=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto folder=data/"origin-captures"/std::to_string(stamp);Settings next=settings;
    next.origin=Settings::SavedOrigin{originRig->archive,utf8(folder),*originCandidate};
    if(originMethod==1){next.origin->method="rectangle";next.origin->rectangle=originRectangle;next.origin->referenceCamera=originRig->serials[originRectangleCamera];next.origin->points=originRectanglePoints;}
    auto sample=originSample;auto rig=*originRig;auto pose=*originPose;auto path=profile;
    saveFuture=std::async(std::launch::async,[next,sample,rig,pose,path,folder]{
        if(fs::exists(folder))throw std::runtime_error("원점 기록 폴더가 이미 있습니다.");fs::create_directories(folder);
        nlohmann::json metadata={{"rig_archive",rig.archive},{"camera_serials",rig.serials},{"receipt_span_ms",sample.receiptSkewMs},
            {"corner_quarter_turns",pose.cornerRotations},{"frames",nlohmann::json::array()}};
        const auto& record=*next.origin;metadata["method"]=record.method;metadata["measured_camera_indices"]=record.transform.measuredCameras;
        if(record.method=="rectangle"){
            metadata["reference_camera"]=record.referenceCamera;metadata["width_mm"]=record.rectangle.widthMm;metadata["height_mm"]=record.rectangle.heightMm;metadata["points_px"]=nlohmann::json::array();
            for(auto p:record.points)metadata["points_px"].push_back({p.x,p.y});
        }
        for(size_t i=0;i<sample.cameras.size();++i){const auto& o=sample.cameras[i];std::vector<uchar> bytes;
            if(!cv::imencode(".png",o.image,bytes))throw std::runtime_error("Origin image encode failed.");
            std::string filename=record.method=="rectangle"?"rectangle.png":std::string(1,char('A'+i))+".png";
            std::ofstream out(folder/filename,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));out.flush();if(!out)throw std::runtime_error("Origin image write failed.");
            auto& light=o.softwareBrightness;metadata["frames"].push_back({{"serial",record.method=="rectangle"?record.referenceCamera:rig.serials[i]},{"frame_id",o.frameId},{"timestamp",o.timestamp},
                {"software_brightness",{{"enabled",light.enabled},{"fitted",light.fitted},{"black_level",light.blackLevel},{"white_level",light.whiteLevel}}}});
        }
        const auto& w=next.origin->transform;
        metadata["R_world_to_A"]=std::vector<double>(w.R.ptr<double>(),w.R.ptr<double>()+9);metadata["T_world_to_A_m"]=std::vector<double>(w.T.ptr<double>(),w.T.ptr<double>()+3);
        metadata["camera_errors_px"]=w.errors;metadata["quarter_turns"]=w.quarterTurns;metadata["surface_height_mm"]=w.surfaceHeightMm;
        metadata["convention"]="X_A = R_world_to_A * X_world + T_world_to_A_m; metres; right handed; Z toward cameras";
        std::ofstream out(folder/"origin.json",std::ios::binary);out<<metadata.dump(2);out.flush();if(!out)throw std::runtime_error("Origin metadata write failed.");
        // Keep the exact intrinsics, board and rig alongside the raw origin images.
        writeSettings(folder/"settings.json",next);
        writeSettings(path,next);return next;
    });savingOrigin=true;originMessage="저장 중...";
}catch(const std::exception& e){originMessage=e.what();}}
void drawOriginPositions(const WorldOrigin& w,const Settings::SavedRig& rig){
    ImGui::TextUnformatted("카메라 위치 (mm)");
    for(size_t i=0;i<rig.serials.size();++i){cv::Mat p=w.cameraCenter(rig.calibration,i)*1000;
        ImGui::Text("%s | X %.1f / Y %.1f / Z %.1f",cameraName(rig.serials[i]).c_str(),p.at<double>(0),p.at<double>(1),p.at<double>(2));
        if(w.measured(i)){ImGui::SameLine();ImGui::Text("| RMS %.3f px",w.errors[i]);}}
    if(*std::max_element(w.errors.begin(),w.errors.end())>2)ImGui::TextColored({1,.65f,.2f,1},"오차 2px 이상 · 보드와 보정 상태를 확인하세요.");
}
void drawRectangleOriginPreview(){
    cv::Mat raw;uint64_t generation=0;
    if(originRectangleFrozen&&!originSample.cameras.empty())raw=originSample.cameras[0].image;
    else {int i=rectangleCameraIndex();if(i>=0&&cameras[i]->connected()){auto frame=cameras[i]->latest();raw=frame.image;generation=frame.generation;}}
    if(raw.empty()){ImGui::TextDisabled("영상 대기 중");originRectangleImageSize={};return;}
    if(originDirty||!originPreview[0].id||(!originRectangleFrozen&&generation!=originViewGenerations[0])){
        auto display=raw.clone();
        if(originCandidate&&originRig)display=drawWorldAxes(display,settings.cameras.at(originRig->serials[originRectangleCamera]).calibration,originRig->calibration,*originCandidate,size_t(originRectangleCamera),std::min(originRectangle.widthMm,originRectangle.heightMm)/3000);
        originPreview[0].update(display);originViewGenerations[0]=generation;originDirty=false;
    }
    auto available=ImGui::GetContentRegionAvail();float scale=std::min(available.x/raw.cols,available.y/raw.rows);
    if(scale<=0)return;
    originRectangleSourceSize=raw.size();originRectangleImageSize={raw.cols*scale,raw.rows*scale};
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(available.x-originRectangleImageSize.x)*.5f);
    originRectangleImageMin=ImGui::GetCursorScreenPos();ImGui::Image(ImTextureRef(ImTextureID(originPreview[0].id)),originRectangleImageSize);
    bool hovered=ImGui::IsItemHovered();auto mouse=ImGui::GetMousePos();
    auto point=cv::Point2f(std::clamp((mouse.x-originRectangleImageMin.x)/scale,0.f,float(raw.cols-1)),std::clamp((mouse.y-originRectangleImageMin.y)/scale,0.f,float(raw.rows-1)));
    if(hovered&&originRectangleFrozen&&!busy()&&originRectanglePoints.size()<4&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        originRectanglePoints.push_back(point);originPose.reset();originCandidate.reset();originConfirmed=false;originSaved=false;originDirty=true;
    }
    auto* draw=ImGui::GetWindowDrawList();std::vector<ImVec2> points;
    for(size_t i=0;i<originRectanglePoints.size();++i){auto p=originRectanglePoints[i];ImVec2 screen{originRectangleImageMin.x+p.x*scale,originRectangleImageMin.y+p.y*scale};points.push_back(screen);
        draw->AddCircle(screen,5,IM_COL32(255,210,60,255),16,2);draw->AddText({screen.x+8,screen.y-12},IM_COL32(255,230,120,255),std::to_string(i+1).c_str());}
    for(size_t i=1;i<points.size();++i)draw->AddLine(points[i-1],points[i],IM_COL32(255,210,60,255),1.5f);
    if(points.size()==4)draw->AddLine(points.back(),points.front(),IM_COL32(255,210,60,255),1.5f);
    if(hovered&&originRectangleFrozen&&!busy()&&originRectanglePoints.size()<4){
        cv::Mat crop,enlarged;cv::getRectSubPix(raw,{64,64},point,crop);cv::resize(crop,enlarged,{256,256},0,0,cv::INTER_NEAREST);
        cv::line(enlarged,{112,128},{144,128},{0,220,255},1);cv::line(enlarged,{128,112},{128,144},{0,220,255},1);originPreview[1].update(enlarged);
        ImGui::BeginTooltip();ImGui::Image(ImTextureRef(ImTextureID(originPreview[1].id)),{256,256});ImGui::Text("%d / 4  ·  %.1f, %.1f px",int(originRectanglePoints.size()+1),point.x,point.y);ImGui::EndTooltip();
    }
}
void drawBoardOriginPreview(){
    if(!settings.rig){ImGui::TextDisabled("상대 위치 보정 필요");return;}
    const auto& rig=originRig?*originRig:*settings.rig;const auto& sample=originCandidate?originSample:originDetected.sample;
    if(ImGui::BeginTable("origin-views",int(rig.serials.size()),ImGuiTableFlags_SizingStretchSame)){
        for(size_t i=0;i<rig.serials.size();++i){ImGui::TableNextColumn();ImGui::TextUnformatted(cameraName(rig.serials[i]).c_str());cv::Mat raw;uint64_t generation=0;
            bool detectedFrame=false;
            if(originCandidate&&i<sample.cameras.size())raw=sample.cameras[i].image;
            else {int index=deviceCamera(rig.serials[i]);if(index>=0&&cameras[index]->connected()){
                auto frame=cameras[index]->latest();raw=frame.image;generation=frame.generation;detectedFrame=i<sample.cameras.size()&&sample.cameras[i].frameId==frame.id;}
                if(smoke&&raw.empty()&&i<sample.cameras.size())raw=sample.cameras[i].image;}
            if(raw.empty()){ImGui::TextDisabled("영상 대기 중");continue;}
            if(originDirty||!originPreview[i].id||generation!=originViewGenerations[i]){
                auto display=raw.clone();if(originCandidate)display=drawWorldAxes(display,settings.cameras.at(rig.serials[i]).calibration,rig.calibration,*originCandidate,i,rig.board.squareMm*2/1000);
                else if(detectedFrame)for(auto p:sample.cameras[i].imagePoints)cv::circle(display,p,3,{0,255,0},-1);
                originPreview[i].update(display);originViewGenerations[i]=generation;}
            originPreview[i].draw();
        }ImGui::EndTable();originDirty=false;
    }
}
void drawOrigin(){
    if(!originOptionsLoaded){originOptionsLoaded=true;
        if(settings.origin){originTurns=settings.origin->transform.quarterTurns;originHeight=settings.origin->transform.surfaceHeightMm;
            if(settings.origin->method=="rectangle"){originMethod=1;originRectangle=settings.origin->rectangle;
                if(settings.rig){auto it=std::find(settings.rig->serials.begin(),settings.rig->serials.end(),settings.origin->referenceCamera);if(it!=settings.rig->serials.end())originRectangleCamera=int(it-settings.rig->serials.begin());}}}}
    if(!smoke){ImGuiWindowClass cls;cls.ViewportFlagsOverrideSet=ImGuiViewportFlags_NoAutoMerge|ImGuiViewportFlags_TopMost;ImGui::SetNextWindowClass(&cls);}
    ImGui::SetNextWindowSize({1240,850},ImGuiCond_FirstUseEver);
    bool expanded=ImGui::Begin("원점 설정###바닥 원점 / 좌표계 설정",&showOrigin,ImGuiWindowFlags_NoDocking);
    if(!showOrigin||!expanded){cancelOrigin();ImGui::End();return;}
    ImGui::BeginChild("origin-video",{0,std::max(180.f,ImGui::GetContentRegionAvail().y-240.f)},ImGuiChildFlags_Borders,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    if(originMethod==1)drawRectangleOriginPreview();else drawBoardOriginPreview();ImGui::EndChild();
    if(originCapture.running()){originCapture.tick(seconds(Clock::now()));scheduleOrigin();ImGui::ProgressBar(originCapture.progress(),{-1,16},"보드 고정 대기");}
    auto readiness=originReadiness();ImGui::TextWrapped("%s",readiness.empty()?originMessage.c_str():readiness.c_str());
    ImGui::Separator();bool pending=busy()||originDetectTask.pending()||originPoseTask.pending();ImGui::BeginDisabled(pending);
    ImGui::SetNextItemWidth(165);if(ImGui::Combo("Method",&originMethod,"Board\0Rectangle · 4 points\0")){resetOriginCapture();originMessage="영상 대기";}
    if(originMethod==1){
        ImGui::SameLine();ImGui::SetNextItemWidth(180);std::string name=settings.rig&&size_t(originRectangleCamera)<settings.rig->serials.size()?cameraName(settings.rig->serials[originRectangleCamera]):"Camera";
        if(ImGui::BeginCombo("Camera",name.c_str())){if(settings.rig)for(size_t i=0;i<settings.rig->serials.size();++i){
            int index=deviceCamera(settings.rig->serials[i]);bool available=index>=0&&cameras[index]->running()&&compatible(*cameras[index]);ImGui::BeginDisabled(!available);
            if(ImGui::Selectable(cameraName(settings.rig->serials[i]).c_str(),originRectangleCamera==int(i))){originRectangleCamera=int(i);resetOriginCapture();}ImGui::EndDisabled();}ImGui::EndCombo();}
        ImGui::SameLine();ImGui::SetNextItemWidth(90);bool dimensions=ImGui::InputDouble("W (mm)",&originRectangle.widthMm,0,0,"%.1f");
        ImGui::SameLine();ImGui::SetNextItemWidth(90);dimensions|=ImGui::InputDouble("H (mm)",&originRectangle.heightMm,0,0,"%.1f");
        if(dimensions){originCandidate.reset();originPose.reset();originConfirmed=false;originSaved=false;originDirty=true;}
        ImGui::BeginDisabled(!readiness.empty());if(ImGui::Button(originRectangleFrozen?"Retake":"Capture",{110,32}))captureRectangleOrigin();ImGui::EndDisabled();ImGui::SameLine();
        ImGui::BeginDisabled(originRectanglePoints.empty());if(ImGui::Button("Undo",{70,32})){originRectanglePoints.pop_back();originCandidate.reset();originPose.reset();originConfirmed=false;originSaved=false;originDirty=true;}ImGui::SameLine();
        if(ImGui::Button("Reset",{70,32})){originRectanglePoints.clear();originCandidate.reset();originPose.reset();originConfirmed=false;originSaved=false;originDirty=true;}ImGui::EndDisabled();ImGui::SameLine();
    }else if(settings.rig){const auto& b=settings.rig->board;ImGui::SameLine();ImGui::Text("%s %d × %d · %.2f mm",b.pattern==Pattern::Chessboard?"Chessboard":"ChArUco",b.columns,b.rows,b.squareMm);}
    ImGui::BeginDisabled(!readiness.empty()||(originMethod==1&&(!originRectangleFrozen||originRectanglePoints.size()!=4)));
    if(ImGui::Button("위치 보정",{140,32})){if(originMethod==1)beginRectangleOrigin();else beginOrigin();}ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!originCandidate);ImGui::Checkbox("원점·방향 확인",&originConfirmed);ImGui::SameLine();ImGui::BeginDisabled(!originConfirmed||originSaved);
    if(ImGui::Button(originSaved?"Saved":"Save",{90,32}))saveOrigin();ImGui::EndDisabled();ImGui::EndDisabled();ImGui::EndDisabled();
    if(originCapture.running()||originPoseTask.active()){ImGui::SameLine();if(ImGui::Button("Cancel",{90,32})){cancelOrigin();originMessage="위치 보정 취소됨";}}
    ImGui::BeginDisabled(pending);ImGui::SetNextItemWidth(160);if(ImGui::Combo("X·Y축 회전",&originTurns,"0도\0 90도\0 180도\0 270도\0"))rebuildOrigin();
    ImGui::SameLine();ImGui::SetNextItemWidth(120);if(ImGui::InputDouble("표면 높이 (mm)",&originHeight,0,0,"%.2f")){originHeight=std::isfinite(originHeight)?std::clamp(originHeight,0.,1000.):0.;rebuildOrigin();}ImGui::EndDisabled();
    ImGui::SameLine();ImGui::TextDisabled(originMethod==1?"1→2 가로 / 2→3 세로 · 마우스 확대":"원점: 보드 중앙 / Z: 카메라 쪽");
    if(originCandidate&&originRig){if(ImGui::CollapsingHeader("Camera positions"))drawOriginPositions(*originCandidate,*originRig);}
    else if(settings.originCurrent()){if(ImGui::CollapsingHeader("Saved origin"))drawOriginPositions(settings.origin->transform,*settings.rig);}
    else if(settings.origin)ImGui::TextColored({1,.65f,.2f,1},"보정값 변경 · 위치 보정 필요");
    if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)&&ImGui::IsKeyPressed(ImGuiKey_Escape)){cancelOrigin();originMessage="위치 보정 취소됨";}
    ImGui::End();
}
std::vector<cv::Point2f> originSmokeCorners;
public:
void prepareRectangleOriginSmoke(){
    prepareOriginSmoke();resetOriginCapture();originOptionsLoaded=true;originMethod=1;originRectangleCamera=1;originRectangle={};originRig=*settings.rig;
    const auto& k=settings.cameras.at(originRig->serials[1]).calibration;const auto& edge=originRig->calibration.fromReference[0];
    cv::Mat baseR;cv::Rodrigues(cv::Vec3d(.18,-.12,.08),baseR);cv::Mat baseT=(cv::Mat_<double>(3,1)<<-.15,-.1,1.0);
    cv::Mat r=edge.R*baseR,t=edge.R*baseT+edge.T;
    std::vector<cv::Point3f> objects{{0,0,0},{.297f,0,0},{.297f,.210f,0},{0,.210f,0}};
    cv::projectPoints(objects,r,t,k.K,k.distortion,originSmokeCorners);Observation sample;sample.image=cv::Mat(k.imageSize,CV_8UC3,cv::Scalar(42,45,48));
    std::vector<cv::Point> contour;for(auto p:originSmokeCorners)contour.emplace_back(cvRound(p.x),cvRound(p.y));cv::fillConvexPoly(sample.image,contour,{230,235,238},cv::LINE_AA);
    originSample.cameras={sample};originRectangleFrozen=true;originDirty=true;originMessage="4점 선택 검증 · 실제 카메라를 사용하지 않습니다.";
}
void rectangleOriginInputSmoke(int frame){
    if(!smoke||originMethod!=1)return;auto& io=ImGui::GetIO();
    for(int i=0;i<4;++i){int at=8+i*4;
        if(frame==at-1){auto p=originSmokeCorners[i];io.AddFocusEvent(true);io.AddMousePosEvent(originRectangleImageMin.x+p.x/originRectangleSourceSize.width*originRectangleImageSize.x,originRectangleImageMin.y+p.y/originRectangleSourceSize.height*originRectangleImageSize.y);}
        if(frame==at)io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);if(frame==at+1)io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);
    }
    if(frame==26){
        if(originRectanglePoints.size()!=4)throw std::runtime_error("Rectangle canvas click count: "+std::to_string(originRectanglePoints.size()));
        // ImGui floors mouse coordinates to screen pixels. At a reduced image
        // scale one screen pixel covers more than one source pixel; keep that
        // quantization bound while still rejecting incorrect canvas offsets.
        const double sourcePixelPerScreenPixel=double(originRectangleSourceSize.width)/originRectangleImageSize.x;
        for(size_t i=0;i<4;++i){auto delta=originRectanglePoints[i]-originSmokeCorners[i];
            if(std::abs(delta.x)>sourcePixelPerScreenPixel+.001||std::abs(delta.y)>sourcePixelPerScreenPixel+.001)
                throw std::runtime_error("Rectangle canvas source-pixel offset exceeds one screen pixel.");}
        originPose=locateRectangle(originRectanglePoints,originRectangle,settings.cameras.at(originRig->serials[1]).calibration,originRig->calibration,1);rebuildOrigin();originConfirmed=true;
        if(!originCandidate||originCandidate->measured(0)||!originCandidate->measured(1)||originCandidate->measured(2))throw std::runtime_error("Rectangle observation RMS mask differs.");
        originMessage="4점 위치 보정 완료 · 선택한 카메라만 RMS 측정";
        std::cout<<"PASS: rectangle canvas clicks map to original pixels; selected-camera pose and measured-only RMS\n";
    }
}
private:
