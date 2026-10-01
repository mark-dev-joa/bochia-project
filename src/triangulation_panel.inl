// One frozen pair; camera acquisition and raw recording continue in the background.
bool pointFrozen=false;
std::array<CameraFrame,2> pointFrames;
std::array<std::optional<cv::Point2d>,2> pointPixels;
struct PickedPoint {
    int id=0;
    std::array<cv::Point2d,2> pixels;
    TriangulatedPoint result;
    std::optional<SceneMeasurement> world;
};
std::vector<PickedPoint> pointList;
int pointSelected=-1,pointEditing=-1,pointNextId=1;
std::map<int,ImVec2> pointRowCenters; // Also used by the hidden UI smoke.
std::string pointError,pointRevision;
uint64_t pointEpoch=0;
double pointSkewMs=0;
std::array<std::array<ImVec2,2>,2> pointCanvas{};

int pointSide(int camera)const{
    if(camera<0||camera>=int(cameras.size()))return -1;
    for(int side=0;side<2;++side)if(cameras[camera]->device.serial==layout.triangulationSerials[side])return side;
    return -1;
}
bool pointFrameActive(int camera)const{return pointFrozen&&pointSide(camera)>=0;}
void refreshPointViews(){for(auto& view:views)view.generation=0;comparisonDirty=true;}
void resetPointDraft(){pointPixels={};pointEditing=-1;pointError.clear();}
void resumePointLive(){pointFrozen=false;pointFrames={};resetPointDraft();refreshPointViews();}
void clearPoint(){resumePointLive();pointList.clear();pointSelected=-1;pointNextId=1;pointRevision.clear();}
PickedPoint* selectedPoint(){for(auto& p:pointList)if(p.id==pointSelected)return &p;return nullptr;}
void selectPoint(int id){pointSelected=id;if(auto p=selectedPoint();p&&p->world)layout.sceneVisible=true;}
void editSelectedPoint(){if(auto p=selectedPoint();p&&pointFrozen){pointPixels={p->pixels[0],p->pixels[1]};pointEditing=p->id;pointError.clear();}}
void deleteSelectedPoint(){
    if(pointEditing==pointSelected)resetPointDraft();
    std::erase_if(pointList,[&](const auto& p){return p.id==pointSelected;});
    pointSelected=pointList.empty()?-1:pointList.back().id;
}
std::vector<SceneMeasurement> pointSceneMeasurements()const{
    std::vector<SceneMeasurement> out;for(const auto& p:pointList)if(p.world)out.push_back(*p.world);return out;
}
void pointDefaults(){
    auto& serials=layout.triangulationSerials;if(!serials[0].empty()&&!serials[1].empty())return;
    std::vector<std::string> candidates;
    for(const auto& camera:cameras){auto format=camera->settings.find("PixelFormat");
        if((format!=camera->settings.end()&&format->second.starts_with("Mono"))||camera->device.model.find("MONO")!=std::string::npos)
            candidates.push_back(camera->device.serial);}
    for(const auto& camera:cameras)if(std::find(candidates.begin(),candidates.end(),camera->device.serial)==candidates.end())candidates.push_back(camera->device.serial);
    if(candidates.size()>=2)serials={candidates[0],candidates[1]};
}
std::string pointCalibrationRevision(){
    auto& serials=layout.triangulationSerials;savedStereoTransform(settings,serials[0],serials[1]);
    std::string revision;
    for(const auto& serial:serials){int i=deviceCamera(serial);
        if(!displayConnected(i))throw std::runtime_error("두 카메라를 연결하세요.");
        if(!compatible(*cameras[i]))throw std::runtime_error(cameraName(serial)+": 현재 영상에 맞는 개별 보정이 필요합니다.");
        revision+=displayCalibrationKey(i)+"|";}
    if(settings.rig)revision+=settings.rig->archive;
    if(settings.stereo)revision+="|"+settings.stereo->archive;
    if(settings.origin)revision+="|"+settings.origin->archive;
    return revision;
}
void pollPoint(){
    if(!pointFrozen&&pointList.empty())return;
    if(!layout.triangulationVisible&&pointFrozen)resumePointLive();
    try{
        if(pointEpoch!=sourceEpoch||pointRevision!=pointCalibrationRevision())throw std::runtime_error("영상 소스 또는 보정값이 바뀌었습니다. 다시 선택하세요.");
        for(int side=0;side<2;++side){int i=deviceCamera(layout.triangulationSerials[side]);auto frame=sourceFrame(i);
            if(!frame.image.empty()&&frame.image.size()!=settings.cameras.at(layout.triangulationSerials[side]).calibration.imageSize)
                throw std::runtime_error("영상 크기가 바뀌었습니다. 현재 해상도로 다시 보정하세요.");}
    }catch(const std::exception& e){clearPoint();pointError=e.what();}
}
void freezePointPair(){
    try{
        if(busy()||reconnectTask.pending()||replayOpenTask.pending())throw std::runtime_error("진행 중인 작업이 끝난 뒤 선택하세요.");
        if(replay&&(replayTask.pending()||replayDirty))throw std::runtime_error("재생 프레임을 불러오는 중입니다.");
        pointDefaults();auto revision=pointCalibrationRevision();std::array<CameraFrame,2> frames;
        for(int side=0;side<2;++side){int i=deviceCamera(layout.triangulationSerials[side]);
            if(!displayRunning(i)&&!replay)throw std::runtime_error("두 카메라의 Grab을 시작하세요.");
            frames[side]=sourceFrame(i);
            if(frames[side].image.empty())throw std::runtime_error("프레임 대기 중입니다.");
            if(frames[side].image.size()!=settings.cameras.at(layout.triangulationSerials[side]).calibration.imageSize)
                throw std::runtime_error("보정값과 영상 크기가 다릅니다.");}
        if(!replay&&!usableStereoTiming(seconds(frames[0].receivedAt),seconds(frames[1].receivedAt),seconds(Clock::now())))
            throw std::runtime_error("새 프레임을 기다린 뒤 다시 선택하세요.");
        double skew=std::abs(seconds(frames[0].receivedAt)-seconds(frames[1].receivedAt))*1000;
        if(replay&&replayTimes.size()==cameras.size()){
            double a=double(replayTimes[deviceCamera(layout.triangulationSerials[0])]),b=double(replayTimes[deviceCamera(layout.triangulationSerials[1])]);skew=std::abs(a-b)/1e6;}
        for(auto& frame:frames)frame.image=frame.image.clone();
        clearPoint();pointFrames=std::move(frames);pointFrozen=true;pointRevision=revision;pointEpoch=sourceEpoch;pointSkewMs=skew;
        roiCamera=-1;roiFrame={};roiPoints.clear();if(replay)replayPlaying=false;
        for(const auto& serial:layout.triangulationSerials)views[deviceCamera(serial)].visible=true;
        layout.triangulationVisible=true;refreshPointViews();
        layout.comparisonSerials=layout.triangulationSerials;
    }catch(const std::exception& e){pointError=e.what();}
}
void pickPointPixel(int side,cv::Point2d displayed,int mode,int stereoSide=-1){
    if(!pointFrozen||side<0||side>1)return;
    pointError.clear();
    try{
        const auto& calibration=settings.cameras.at(layout.triangulationSerials[side]).calibration;
        pointPixels[side]=stereoPixelToRaw(calibration,displayed,mode,mode==2?comparisonMaps.R.at(stereoSide):cv::Mat(),mode==2?comparisonMaps.P.at(stereoSide):cv::Mat());
        if(pointPixels[0]&&pointPixels[1]){
            auto& ids=layout.triangulationSerials;
            PickedPoint candidate;candidate.id=pointEditing>=0?pointEditing:pointNextId;candidate.pixels={*pointPixels[0],*pointPixels[1]};
            candidate.result=triangulateStereoPoint(settings.cameras.at(ids[0]).calibration,settings.cameras.at(ids[1]).calibration,
                                               savedStereoTransform(settings,ids[0],ids[1]),*pointPixels[0],*pointPixels[1]);
            if(auto world=cameraPointToWorld(settings,ids[0],candidate.result.cameraPoint)){
                SceneMeasurement geometry;geometry.id=candidate.id;geometry.position=*world;geometry.gapM=candidate.result.rayGapM;
                for(int i=0;i<2;++i){geometry.origins[i]=cameraPointToWorld(settings,ids[0],candidate.result.rayOrigins[i]).value();
                    geometry.closest[i]=cameraPointToWorld(settings,ids[0],candidate.result.closestPoints[i]).value();}
                candidate.world=geometry;
            }
            if(pointEditing>=0){auto it=std::find_if(pointList.begin(),pointList.end(),[&](const auto& p){return p.id==pointEditing;});
                if(it==pointList.end())throw std::runtime_error("수정할 점이 없습니다.");*it=candidate;}
            else {pointList.push_back(candidate);++pointNextId;}
            selectPoint(candidate.id);resetPointDraft();
        }
    }catch(const std::exception& e){pointError=e.what();}
}
void drawPointOverlay(int camera,ImVec2 a,ImVec2 size,cv::Size outputSize,int mode,int stereoSide=-1){
    if(!pointFrameActive(camera)||size.x<=0||size.y<=0)return;
    int side=pointSide(camera);ImVec2 b{a.x+size.x,a.y+size.y};pointCanvas[side]={a,b};auto draw=ImGui::GetWindowDrawList();
    auto mouse=ImGui::GetMousePos();bool hovered=ImGui::IsItemHovered()&&mouse.x>=a.x&&mouse.x<b.x&&mouse.y>=a.y&&mouse.y<b.y;
    if(hovered){ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Left))pickPointPixel(side,{std::clamp((mouse.x-a.x)*outputSize.width/size.x-.5,0.,double(outputSize.width-1)),
            std::clamp((mouse.y-a.y)*outputSize.height/size.y-.5,0.,double(outputSize.height-1))},mode,stereoSide);}
    draw->PushClipRect(a,b,true);
    if(hovered){draw->AddLine({mouse.x-10,mouse.y},{mouse.x+10,mouse.y},IM_COL32(255,255,255,255),1.5f);draw->AddLine({mouse.x,mouse.y-10},{mouse.x,mouse.y+10},IM_COL32(255,255,255,255),1.5f);}
    draw->AddRectFilled(a,{b.x,a.y+29},IM_COL32(10,17,26,200));
    int draftId=pointEditing>=0?pointEditing:pointNextId;char hint[160];
    snprintf(hint,sizeof(hint),"P%d · %s",draftId,pointPixels[side]?"반대 영상에서 같은 지점을 클릭하세요":"같은 지점을 클릭하세요");
    draw->AddText({a.x+8,a.y+5},IM_COL32(255,215,70,255),hint);
    auto marker=[&](cv::Point2d raw,int id,ImU32 color,bool active){
        auto p=stereoRawToPixel(settings.cameras.at(layout.triangulationSerials[side]).calibration,raw,mode,
            mode==2?comparisonMaps.R.at(stereoSide):cv::Mat(),mode==2?comparisonMaps.P.at(stereoSide):cv::Mat());
        ImVec2 center{a.x+float(p.x+.5)*size.x/outputSize.width,a.y+float(p.y+.5)*size.y/outputSize.height};
        draw->AddCircle(center,active?8.f:5.f,IM_COL32(0,0,0,220),24,4);draw->AddCircle(center,active?8.f:5.f,color,24,2);
        if(active){draw->AddLine({center.x-12,center.y},{center.x+12,center.y},color,1.5f);draw->AddLine({center.x,center.y-12},{center.x,center.y+12},color,1.5f);}
        auto label="P"+std::to_string(id);draw->AddText({center.x+10,center.y+6},color,label.c_str());
    };
    try{
        for(const auto& p:pointList)if(p.id!=pointEditing)marker(p.pixels[side],p.id,p.id==pointSelected?IM_COL32(255,230,65,255):IM_COL32(80,250,170,255),p.id==pointSelected);
        if(pointPixels[side])marker(*pointPixels[side],draftId,IM_COL32(255,215,70,255),true);
    }catch(const std::exception& e){pointError=e.what();}
    draw->PopClipRect();
}
void drawTriangulation(){
    ImGui::SetNextWindowSize({550,500},ImGuiCond_FirstUseEver);
    if(triangulationSmoke){ImGui::SetNextWindowPos({930,35});ImGui::SetNextWindowSize({490,460});}
    if(ImGui::Begin("Triangulation",&layout.triangulationVisible)){
        pointDefaults();
        for(int side=0;side<2;++side){ImGui::PushID(side);auto& serial=layout.triangulationSerials[side];ImGui::SetNextItemWidth(260);
            if(ImGui::BeginCombo(side?"Right":"Left",cameraName(serial).c_str())){
                for(const auto& camera:cameras){ImGui::PushID(camera->device.serial.c_str());
                    if(ImGui::Selectable(cameraName(camera->device.serial).c_str(),serial==camera->device.serial)){clearPoint();serial=camera->device.serial;}
                    ImGui::PopID();}
                ImGui::EndCombo();}ImGui::PopID();}
        if(ImGui::Button(pointFrozen?"New Pair":"Pick Point",{115,32}))freezePointPair();
        ImGui::SameLine();ImGui::BeginDisabled(!pointFrozen);if(ImGui::Button("Live",{80,32}))resumePointLive();ImGui::EndDisabled();
        ImGui::SameLine();if(ImGui::Button("Clear All",{90,32}))clearPoint();
        if(pointFrozen&&ImGui::IsItemHovered())ImGui::SetTooltip("모든 점을 지우고 화면 고정을 해제합니다.");
        ImGui::TextWrapped("같은 지점을 양쪽에서 클릭하면 P1, P2… 순서로 추가됩니다.");
        if(pointFrozen){ImGui::Text("Frozen  |  Frame %llu / %llu  |  Δt %.2f ms",(unsigned long long)pointFrames[0].id,(unsigned long long)pointFrames[1].id,pointSkewMs);
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Δt는 PC 수신 시각 차이입니다. 노출 동기나 같은 촬영 순간을 보장하지 않습니다.\n화면만 고정하며 카메라 Grab과 녹화는 계속됩니다.");}
        ImGui::Separator();
        bool world=!pointList.empty()&&pointList.front().world.has_value();
        ImGui::Text("%s  |  %zu points",world?"World (mm)":("Camera: "+cameraName(layout.triangulationSerials[0])+" (mm)").c_str(),pointList.size());
        ImGui::BeginDisabled(!selectedPoint()||!pointFrozen);if(ImGui::Button("Edit"))editSelectedPoint();ImGui::EndDisabled();
        ImGui::SameLine();ImGui::BeginDisabled(!selectedPoint());if(ImGui::Button("Delete"))deleteSelectedPoint();ImGui::EndDisabled();
        ImGui::SameLine();ImGui::BeginDisabled(!selectedPoint()||!selectedPoint()->world);
        if(ImGui::Button("3D View")){layout.sceneVisible=true;layout.sceneView.aimAt(selectedPoint()->world->position);}ImGui::EndDisabled();
        if(pointEditing>=0||pointPixels[0]||pointPixels[1]){ImGui::SameLine();if(ImGui::Button("Cancel Pick"))resetPointDraft();}
        if(pointFrozen)ImGui::Text("%s P%d  |  L %s  /  R %s",pointEditing>=0?"Edit":"Next",pointEditing>=0?pointEditing:pointNextId,pointPixels[0]?"OK":"-",pointPixels[1]?"OK":"-");
        else if(pointList.empty())ImGui::TextDisabled("Pick Point로 정지된 대상의 영상 한 쌍을 고정하세요.");
        ImGui::TextDisabled("Error: 광선 간 최단거리  |  XYZ: 중앙점");
        if(!world&&!pointList.empty())ImGui::TextWrapped("바닥 원점을 저장하면 3D View에 표시됩니다.");
        if(!pointError.empty())ImGui::TextWrapped("%s",pointError.c_str());
        pointRowCenters.clear();
        if(ImGui::BeginTable("point-list",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_ScrollY|ImGuiTableFlags_SizingStretchProp,
            {0,std::max(95.f,ImGui::GetContentRegionAvail().y)})){
            ImGui::TableSetupColumn("Point",ImGuiTableColumnFlags_WidthFixed,50);ImGui::TableSetupColumn("X (mm)");ImGui::TableSetupColumn("Y (mm)");ImGui::TableSetupColumn("Z (mm)");ImGui::TableSetupColumn("Error (mm)");
            ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
            for(const auto& p:pointList){ImGui::PushID(p.id);ImGui::TableNextRow();ImGui::TableNextColumn();auto label="P"+std::to_string(p.id);
                if(ImGui::Selectable(label.c_str(),p.id==pointSelected,ImGuiSelectableFlags_SpanAllColumns))selectPoint(p.id);
                auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();pointRowCenters[p.id]={a.x+20,(a.y+b.y)*.5f};
                auto value=(p.world?p.world->position:p.result.cameraPoint)*1000;
                for(int axis=0;axis<3;++axis){ImGui::TableNextColumn();ImGui::Text("%.2f",value[axis]);}
                ImGui::TableNextColumn();ImGui::Text("%.3f",p.result.rayGapM*1000);
                if(ImGui::IsItemHovered())ImGui::SetTooltip("두 광선 사이 최단거리입니다. 실제 위치 오차를 뜻하지 않습니다.\n재투영 오차: %.2f / %.2f px",p.result.reprojectionErrorPx[0],p.result.reprojectionErrorPx[1]);
                ImGui::PopID();}
            ImGui::EndTable();}
    }ImGui::End();
}
