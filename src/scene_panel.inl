SceneRenderer sceneRenderer;
std::vector<SceneCamera> sceneCameras;
std::string sceneRevision,sceneError;
int sceneSelected=-1;
ImVec2 sceneCanvasCenter;
void drawScene(){
    ImGui::SetNextWindowSize({1200,820},ImGuiCond_FirstUseEver);
    if(triangulationSmoke){ImGui::SetNextWindowPos({10,510});ImGui::SetNextWindowSize({1410,415});}
    if(!ImGui::Begin("3D View###3D 공간 / 카메라",&layout.sceneVisible)){ImGui::End();return;}
    if(!settings.originCurrent()){
        sceneCameras.clear();sceneRevision.clear();
        ImGui::TextUnformatted(settings.origin?"보정값이 변경되었습니다. 원점을 다시 설정하세요.":"바닥 원점을 설정하세요.");
        ImGui::BeginDisabled(saveFuture.valid());if(ImGui::Button("원점 설정"))openOrigin();ImGui::EndDisabled();ImGui::End();return;
    }
    try{
        auto revision=settings.rig->archive+"|"+settings.origin->archive;
        if(revision!=sceneRevision){sceneCameras=buildSceneCameras(settings);sceneRevision=revision;sceneError.clear();sceneSelected=-1;}
        if(!layout.sceneView.initialized)frameScene(layout.sceneView,sceneCameras);
    }catch(const std::exception& e){sceneError=e.what();}
    if(!sceneError.empty()){ImGui::TextWrapped("3D 표시 오류: %s",sceneError.c_str());if(ImGui::Button("다시 시도")){sceneRevision.clear();sceneError.clear();}ImGui::End();return;}
    auto& view=layout.sceneView;auto& options=layout.sceneOptions;
    options.frusta=false;options.directions=true;
    float speed=float(view.speed);
    ImGui::SetNextItemWidth(std::min(240.f,ImGui::GetContentRegionAvail().x-90.f));
    if(ImGui::SliderFloat("Speed (m/s)",&speed,.01f,20.f,"%.2f",ImGuiSliderFlags_Logarithmic))view.speed=speed;
    {
        ImVec2 available=ImGui::GetContentRegionAvail();available.x=std::max(1.f,available.x);available.y=std::max(80.f,available.y);
        auto start=ImGui::GetCursorScreenPos();
        sceneCanvasCenter={start.x+available.x*.5f,start.y+available.y*.5f};
        ImGui::InvisibleButton("3d-canvas",available,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
        bool hover=ImGui::IsItemHovered(),active=ImGui::IsItemActive();auto& io=ImGui::GetIO();
        if(ImGui::IsItemClicked(ImGuiMouseButton_Left)||ImGui::IsItemClicked(ImGuiMouseButton_Right))ImGui::SetWindowFocus();
        bool controls=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)&&!io.WantTextInput&&!io.AppFocusLost&&
            !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)&&((hover&&!ImGui::IsAnyItemActive())||active);
        if(controls){
            bool leftDrag=ImGui::IsMouseDragging(ImGuiMouseButton_Left)&&!ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            bool rightDrag=ImGui::IsMouseDragging(ImGuiMouseButton_Right)&&!ImGui::IsMouseClicked(ImGuiMouseButton_Right);
            if(active&&(leftDrag||rightDrag))view.look(io.MouseDelta.x,io.MouseDelta.y);
            auto held=[](ImGuiKey key){return ImGui::IsKeyDown(key)?1.:0.;};
            double turn=std::clamp(double(io.DeltaTime),0.,.05)*(CV_PI/2)/.003;
            view.look((held(ImGuiKey_RightArrow)-held(ImGuiKey_LeftArrow))*turn,(held(ImGuiKey_DownArrow)-held(ImGuiKey_UpArrow))*turn);
            view.move(held(ImGuiKey_W)-held(ImGuiKey_S),held(ImGuiKey_D)-held(ImGuiKey_A),held(ImGuiKey_E)-held(ImGuiKey_Q),io.DeltaTime,io.KeyShift);
            if(hover&&io.MouseWheel!=0)view.speed=std::clamp(view.speed*std::pow(1.25,io.MouseWheel),.01,20.);
        }
        auto matrix=view.matrix(available.x/available.y);auto draw=ImGui::GetWindowDrawList();
        auto end=ImVec2(start.x+available.x,start.y+available.y);
        try{
            auto mesh=makeSceneMesh(sceneCameras,options,sceneSelected,pointSceneMeasurements(),pointSelected);float dpi=ImGui::GetWindowViewport()->DpiScale;
            auto texture=sceneRenderer.render(mesh,matrix,int(available.x*dpi),int(available.y*dpi));
            draw->AddImage(ImTextureRef(ImTextureID(texture)),start,end,{0,1},{1,0});
        }catch(const std::exception& e){sceneError=e.what();}
        draw->PushClipRect(start,end,true);
        auto point=[&](cv::Vec3d p)->std::optional<ImVec2>{auto n=projectScenePoint(matrix,p);if(!n)return {};return ImVec2(start.x+float(n->x)*available.x,start.y+float(n->y)*available.y);};
        if(options.axes){double axisLength=std::clamp(options.gridExtent*.2,.15,1.);
            const cv::Vec3d ends[]={{axisLength,0,0},{0,axisLength,0},{0,0,axisLength}};const ImU32 colors[]={IM_COL32(255,90,90,255),IM_COL32(90,245,140,255),IM_COL32(90,165,255,255)};
            for(int i=0;i<3;++i)if(auto p=point(ends[i])){char text[2]={char('X'+i),0};draw->AddText(*p,colors[i],text);}
            if(auto p=point({0,0,0}))draw->AddText({p->x+7,p->y+7},IM_COL32(235,240,250,255),"O (0,0,0)");
        }
        const ImU32 colors[]={IM_COL32(65,205,255,255),IM_COL32(255,175,70,255),IM_COL32(205,125,255,255)};
        for(const auto& measured:pointList)if(measured.world){const auto& geometry=*measured.world;bool selected=measured.id==pointSelected;
            if(auto p=point(geometry.position)){
                char label[160];if(selected)snprintf(label,sizeof(label),"P%d Midpoint  (%.1f, %.1f, %.1f) mm",measured.id,geometry.position[0]*1000,geometry.position[1]*1000,geometry.position[2]*1000);
                else snprintf(label,sizeof(label),"P%d",measured.id);
                auto color=selected?IM_COL32(255,230,65,255):IM_COL32(80,250,170,255);
                draw->AddCircle(*p,selected?6.f:4.f,color,24,2);draw->AddText({p->x+10,p->y+8},color,label);
                if(selected){char gap[80];snprintf(gap,sizeof(gap),"Gap %.3f mm",geometry.gapM*1000);
                    draw->AddText({p->x+10,p->y+28},IM_COL32(255,90,215,255),gap);}
            }
            if(selected)for(int side=0;side<2;++side)if(auto p=point(geometry.closest[side])){
                // Endpoint dots stay at true scale even when the gap is subpixel on screen.
                draw->AddCircleFilled(*p,2.5f,colors[side]);}
        }
        if(auto measured=selectedPoint();measured&&measured->world){
            draw->AddText({start.x+12,start.y+30},colors[0],("L · "+cameraName(layout.triangulationSerials[0])).c_str());
            draw->AddText({start.x+12,start.y+50},colors[1],("R · "+cameraName(layout.triangulationSerials[1])).c_str());
            draw->AddText({start.x+12,start.y+70},IM_COL32(255,90,215,255),"Shortest segment");
        }
        for(size_t i=0;i<sceneCameras.size();++i)if(auto p=point(sceneCameras[i].center)){
            auto label=cameraName(sceneCameras[i].serial);
            auto size=ImGui::CalcTextSize(label.c_str());float x=std::clamp(p->x-size.x*.5f,start.x+8,std::max(start.x+8,end.x-size.x-10));
            float y=std::clamp(p->y-28-float(i)*20,start.y+8,std::max(start.y+8,end.y-25));
            draw->AddLine(*p,{x,y+size.y*.5f},colors[i],1);
            draw->AddRectFilled({x-4,y-2},{x+size.x+4,y+size.y+2},IM_COL32(10,17,26,220),3);
            draw->AddText({x,y},colors[i],label.c_str());draw->AddCircleFilled(*p,3,colors[i]);
        }
        if(settings.rig->archive=="synthetic-rig")draw->AddText({start.x+12,start.y+12},IM_COL32(150,170,190,255),"TEST");
        draw->PopClipRect();
    }
    ImGui::End();
}
