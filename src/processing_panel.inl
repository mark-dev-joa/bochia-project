// Application members: raw recording/replay and the shared live/offline detector.
    bool showProcessing=false,showRecording=false;
    std::shared_ptr<RawRecorder> recorder;
    fs::path recordingParent,recordingFolder;
    std::string recordingMessage;
    std::shared_ptr<RawReplay> replay;
    std::optional<Settings> liveSettings;
    BackgroundTask<std::shared_ptr<RawReplay>> replayOpenTask;
    struct ReplayGroup {size_t frame=0;uint64_t request=0;std::vector<ReplayFrame> cameras;};
    BackgroundTask<ReplayGroup> replayTask;
    size_t replayFrameIndex=0,replayWanted=0;int replayReference=0;
    uint64_t replayRequest=0,replayGeneration=0,sourceEpoch=0;
    std::vector<uint64_t> replayTimes;
    bool replayPlaying=false,replayDirty=false;
    Clock::time_point replayNext{};
    float replaySpeed=1;
    struct BallJob {CameraFrame frame;BallDetection detection;uint64_t revision=0;double millis=0;};   // millis: the detection's wall time
    struct BallStats {   // Detection time of one camera: a smoothed average and the completions of the last second, for the panel's frame-time line.
        std::mutex lock;double smoothed=0;int samples=0;std::vector<Clock::time_point> completions;
        void prune(Clock::time_point now){completions.erase(completions.begin(),std::find_if(completions.begin(),completions.end(),[&](Clock::time_point t){return now-t<=std::chrono::seconds(1);}));}
        void note(double millis){std::lock_guard guard(lock);smoothed=samples++?smoothed*.8+millis*.2:millis;const auto now=Clock::now();completions.push_back(now);prune(now);}
        std::pair<double,int> read(){std::lock_guard guard(lock);prune(Clock::now());return {smoothed,int(completions.size())};}
    };
    struct BallRuntime {
        BackgroundTask<BallJob> task;
        std::optional<BallJob> result;
        BallOptions options;
        uint64_t revision=0,epoch=UINT64_MAX,submitted=0;
        const uchar* submittedPixels=nullptr;
        std::shared_ptr<BallStats> stats=std::make_shared<BallStats>();   // shared with the running task, so it outlives a cancelled runtime
        std::string error;
    };
    std::map<std::string,std::unique_ptr<BallRuntime>> ballRuntime;
    int ballCamera=0,roiCamera=-1;
    CameraFrame roiFrame;
    std::vector<cv::Point2f> roiPoints;
    std::array<ImVec2,2> roiCanvas;
    bool ballDirty=false;
    Clock::time_point ballEdited{};
    std::string ballMessage;
    bool recordingBusy()const{return recorder&&!recorder->statistics().finished;}
    bool sourceBusy()const{return recordingBusy()||replayOpenTask.pending();}
    void markBallChanged(){ballDirty=true;ballEdited=Clock::now();for(auto& v:views)v.generation=0;comparisonDirty=true;}
    void saveBallOptions(){
        if(!ballDirty||loadFailed||saveFuture.valid())return;
        try{Settings next=liveSettings?*liveSettings:settings;next.ball=settings.ball;
            if(!smoke)writeSettings(profile,next);if(liveSettings)liveSettings->ball=settings.ball;ballDirty=false;ballMessage.clear();
        }catch(const std::exception& e){ballMessage=e.what();ballEdited=Clock::now();}
    }
    BallOptions& ballOptions(size_t i){auto& c=*cameras.at(i);auto [it,inserted]=settings.ball.cameras.try_emplace(c.device.serial);
        if(inserted){auto format=c.settings.find("PixelFormat");if(format!=c.settings.end())it->second.color=format->second.find("Bayer")!=std::string::npos||format->second.find("RGB")!=std::string::npos||format->second.find("BGR")!=std::string::npos;}
        it->second.model=settings.ball.model;it->second.modelConfidence=settings.ball.modelConfidence;it->second.cuda=settings.ball.cuda;return it->second;}
    void stopRecording(){for(auto& c:cameras)c->setRecorder({});if(recorder)recorder->stop();}
    void startRecording(){try{
        if(replay||reconnectTask.pending()||busy()||recordingBusy())return;
        std::vector<RecordedCamera> descriptions;std::vector<size_t> indices;
        for(size_t i=0;i<cameras.size();++i)if(cameras[i]->running()){
            const auto& c=*cameras[i];descriptions.push_back({c.device.serial,c.device.model,cameraName(c.device.serial),{},c.settings});indices.push_back(i);}
        if(indices.empty())throw std::runtime_error("장치 패널에서 Grab을 시작하세요.");
        settings.recording.validate();if(!smoke)writeSettings(profile,settings);
        if(recordingParent.empty())recordingParent=data/"recordings";fs::create_directories(recordingParent);
        auto stamp=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        recordingFolder=recordingParent/("capture-"+std::to_string(stamp));
        recorder=std::make_shared<RawRecorder>(recordingFolder,descriptions,settings,256ull*1024*1024,settings.recording);
        for(size_t i=0;i<indices.size();++i)cameras[indices[i]]->saveDeviceConfig(recordingFolder/("camera-"+std::to_string(i))/"device.xml");
        for(size_t i=0;i<indices.size();++i)cameras[indices[i]]->setRecorder(recorder,i);
        recordingMessage.clear();
    }catch(const std::exception& e){for(auto& c:cameras)c->setRecorder({});if(recorder)recorder->stop(e.what());recordingMessage=e.what();}}
    void requestReplay(const fs::path& folder){
        if(folder.empty()||sourceBusy()||reconnectTask.pending()||busy())return;
        recordingMessage="Loading...";
        replayOpenTask.start([folder](std::stop_token stop){return std::make_shared<RawReplay>(folder,stop);});
    }
    void clearSourceViews(){
        clearPoint();
        rememberCameraWindows();cancelWork();cancelStereo();cancelOrigin();showCalibration=showStereo=showOrigin=false;
        roiCamera=-1;roiFrame={};roiPoints.clear();++sourceEpoch;
        for(auto& [id,r]:ballRuntime){r->task.cancel();r->result.reset();}
        for(auto& c:cameras)c->close();for(auto& v:views)v.texture.release();views.clear();cameras.clear();cameraNameEdits.clear();
        comparisonDirty=true;comparisonKey.clear();comparisonGeometryKey.clear();comparisonGuideKey.clear();comparisonGenerations={};
    }
    void applyReplay(std::shared_ptr<RawReplay> loaded){
        saveBallOptions();clearSourceViews();if(!liveSettings)liveSettings=settings;
        auto options=settings.ball;settings=loaded->settings;settings.ball=options;replay=std::move(loaded);
        std::vector<Device> found;for(const auto& recorded:replay->cameras){Device d{"replay",recorded.serial,recorded.model};
            auto c=std::make_unique<Camera>(d,true);c->settings=recorded.settings;cameras.push_back(std::move(c));views.emplace_back();found.push_back(d);}
        devices.clear();selectedDevices.clear();updateDeviceList(found);cameraIndex=ballCamera=0;
        replayReference=0;replayFrameIndex=replayWanted=0;replayPlaying=false;replayDirty=true;replayTimes.clear();++replayRequest;
        recordingMessage=replay->warning;initialGuide=false;offline=false;
    }
    void returnLive(){
        if(!replay||replayOpenTask.pending()||replayTask.pending())return;
        saveBallOptions();clearSourceViews();settings=*liveSettings;liveSettings.reset();replay.reset();replayPlaying=false;
        devices.clear();selectedDevices.clear();updateDeviceList({});recordingMessage.clear();
        if(!smoke&&settings.autoReconnect&&!settings.selectedSerials.empty())beginReconnect();
    }
    void seekReplay(size_t index){if(!replay)return;clearPoint();replayWanted=std::min(index,replay->index[replayReference].size()-1);++replayRequest;replayDirty=true;roiCamera=-1;roiFrame={};}
    bool replayProcessed(){
        if(!settings.ball.run)return true;
        for(size_t i=0;i<cameras.size();++i){if(!ballOptions(i).detect)continue;auto it=ballRuntime.find(cameras[i]->device.serial);
            if(it==ballRuntime.end())return false;auto& r=*it->second;auto frame=cameras[i]->latest();
            if(r.options!=ballOptions(i))return false;if(!r.error.empty())continue;if(!r.result||r.result->frame.generation!=frame.generation)return false;}
        return true;
    }
    void pollSource(){
        if(recorder){auto status=recorder->statistics();if(!status.accepting){for(auto& c:cameras)c->setRecorder({});if(!status.error.empty())showRecording=true;}}
        try{if(auto loaded=replayOpenTask.poll())applyReplay(std::move(*loaded));}catch(const std::exception& e){recordingMessage=e.what();}
        try{if(auto group=replayTask.poll();group&&group->request==replayRequest&&replay){
            replayFrameIndex=group->frame;replayTimes.clear();auto now=Clock::now();++replayGeneration;
            for(size_t i=0;i<group->cameras.size();++i){auto& f=group->cameras[i];replayTimes.push_back(f.info.hostNs);
                const auto& timeline=replay->index[i];double fps=timeline.size()>1&&timeline.back().hostNs>timeline.front().hostNs?(timeline.size()-1)*1e9/double(timeline.back().hostNs-timeline.front().hostNs):0;
                cameras[i]->setReplayFrame({f.image,f.info.id,f.info.timestamp,replayGeneration,now},fps,replay->cameras[i].gaps,replay->cameras[i].invalid);views[i].generation=0;}
            comparisonDirty=true;uint64_t interval=4000000;const auto& timeline=replay->index[replayReference];if(replayFrameIndex+1<timeline.size())interval=timeline[replayFrameIndex+1].hostNs-timeline[replayFrameIndex].hostNs;
            replayNext=now+std::chrono::nanoseconds(int64_t(std::min<double>(interval/replaySpeed,5e9)));
        }}catch(const std::exception& e){recordingMessage=e.what();replayPlaying=false;}
        if(replay&&replayPlaying&&!replayDirty&&!replayTask.pending()&&Clock::now()>=replayNext&&replayProcessed()){
            if(replayFrameIndex+1<replay->index[replayReference].size())seekReplay(replayFrameIndex+1);else replayPlaying=false;}
        if(replay&&replayDirty&&!replayTask.pending()){
            auto source=replay;auto frame=replayWanted,request=replayRequest;size_t reference=replayReference;replayDirty=false;
            replayTask.start([source,frame,request,reference](std::stop_token stop){ReplayGroup group;group.frame=frame;group.request=request;
                auto time=source->index[reference][frame].hostNs;for(size_t i=0;i<source->cameras.size();++i){if(stop.stop_requested())return group;group.cameras.push_back(source->read(i,i==reference?frame:source->nearest(i,time)));}return group;});}
    }
    void drawRecording(){
        ImGui::SetNextWindowSize({530,350},ImGuiCond_FirstUseEver);
        if(processingSmokeActive){ImGui::SetNextWindowPos({462,447});ImGui::SetNextWindowSize({445,470});}
        if(ImGui::Begin("Recording / Playback",&showRecording)){
            if(replay){ImGui::TextUnformatted("Playback");ImGui::TextWrapped("%s",utf8(replay->directory()).c_str());
                if(ImGui::Button(replayPlaying?"Pause":"Play",{85,30})){replayPlaying=!replayPlaying;replayNext=Clock::now();}
                ImGui::SameLine();if(ImGui::Button("< Frame",{85,30})){replayPlaying=false;seekReplay(replayWanted?replayWanted-1:0);}
                ImGui::SameLine();if(ImGui::Button("Frame >",{85,30})){replayPlaying=false;seekReplay(replayWanted+1);}
                int frame=int(replayWanted);ImGui::SetNextItemWidth(-1);
                if(ImGui::SliderInt("##frame",&frame,0,int(replay->index[replayReference].size()-1),"Frame %d",ImGuiSliderFlags_AlwaysClamp)){replayPlaying=false;seekReplay(size_t(frame));}
                double time=(replay->index[replayReference][replayFrameIndex].hostNs-replay->index[replayReference][0].hostNs)/1e9;
                ImGui::Text("%zu / %zu  |  %.3f s%s",replayFrameIndex+1,replay->index[replayReference].size(),time,replayTask.pending()?"  |  Loading...":"");
                ImGui::SetNextItemWidth(180);ImGui::SliderFloat("Speed",&replaySpeed,.1f,2.f,"%.1fx",ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetNextItemWidth(180);if(ImGui::BeginCombo("Reference",cameraName(replay->cameras[replayReference].serial).c_str())){
                    for(int i=0;i<int(replay->cameras.size());++i)if(ImGui::Selectable(cameraName(replay->cameras[i].serial).c_str(),i==replayReference)){
                        auto selectedTime=replay->index[replayReference][replayFrameIndex].hostNs;replayReference=i;replayFrameIndex=replay->nearest(i,selectedTime);replayPlaying=false;seekReplay(replayFrameIndex);}
                    ImGui::EndCombo();}
                ImGui::TextDisabled("PC 수신 시각으로 가까운 프레임 선택");
                if(replayTimes.size()>1){auto [lo,hi]=std::minmax_element(replayTimes.begin(),replayTimes.end());ImGui::TextDisabled("Δt %.3f ms · 노출 동기 오차가 아닙니다.",(*hi-*lo)/1e6);}
                ImGui::BeginDisabled(replayTask.pending()||replayOpenTask.pending());if(ImGui::Button("Live Cameras"))returnLive();ImGui::EndDisabled();
            }else{
                ImGui::TextUnformatted("Raw Recording");
                if(recordingParent.empty())recordingParent=data/"recordings";
                ImGui::TextWrapped("%s",utf8(recordingParent).c_str());
                ImGui::BeginDisabled(sourceBusy()||reconnectTask.pending()||busy()||smoke||loadFailed);
                auto& options=settings.recording;int storage=options.lossless?0:1;ImGui::SetNextItemWidth(170);
                if(ImGui::Combo("Storage",&storage,"Lossless\0Raw\0"))options.lossless=storage==0;
                bool original=options.fps==0;if(ImGui::Checkbox("Original FPS",&original))options.fps=original?0:30;
                ImGui::SameLine();ImGui::BeginDisabled(original);float fps=original?30.f:float(options.fps);ImGui::SetNextItemWidth(120);
                if(ImGui::InputFloat("Record FPS",&fps,1,10,"%.1f"))options.fps=std::isfinite(fps)?std::clamp(double(fps),1.,1000.):30.;ImGui::EndDisabled();
                double estimated=0;for(auto& camera:cameras)if(camera->running()){auto stats=camera->statistics();estimated+=stats.frameBytes*(original?stats.fps:std::min(stats.fps,options.fps));}
                if(estimated>0)ImGui::TextDisabled("압축 전 약 %.1f MB/s · %.2f GB/min",estimated/1e6,estimated*60/1e9);
                if(ImGui::Button("Folder...")){auto folder=chooseFolder();if(!folder.empty())recordingParent=folder;}
                ImGui::SameLine();if(ImGui::Button("Record",{100,30}))startRecording();ImGui::EndDisabled();
                ImGui::SameLine();ImGui::BeginDisabled(!recordingBusy());if(ImGui::Button("Stop",{100,30}))stopRecording();ImGui::EndDisabled();
                if(recorder){auto status=recorder->statistics();ImGui::Text("%s  |  %.1f MB  |  Queue %.1f MB",status.accepting?"Recording":status.finished?(status.error.empty()?"Saved":"Partial"):"Finishing...",status.bytes/1048576.,status.queuedBytes/1048576.);
                    ImGui::Text("%.1f MB/s  |  Lossless saved %.1f%%",status.seconds>0?status.bytes/status.seconds/1048576.:0,status.rawBytes?100.*(1.-double(status.bytes)/status.rawBytes):0);
                    if(ImGui::BeginTable("recording-counts",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_SizingStretchProp)){
                        for(const char* title:{"Camera","Frames","Skipped","Gaps","Invalid"})ImGui::TableSetupColumn(title);ImGui::TableHeadersRow();
                        for(size_t i=0;i<status.frames.size();++i){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%zu",i+1);
                            for(auto count:{status.frames[i],status.sampledOut[i],status.gaps[i],status.invalid[i]}){ImGui::TableNextColumn();ImGui::Text("%llu",(unsigned long long)count);}}
                        ImGui::EndTable();}
                    if(!status.error.empty())ImGui::TextColored({1,.55f,.35f,1},"%s",status.error.c_str());
                    if(status.finished&&ImGui::Button("Open Recording"))requestReplay(recordingFolder);}
            }
            ImGui::Separator();ImGui::BeginDisabled(sourceBusy()||reconnectTask.pending()||busy()||replayTask.pending());
            if(ImGui::Button("Open Folder...")){auto folder=chooseFolder();if(!folder.empty())requestReplay(folder);}ImGui::EndDisabled();
            if(replayOpenTask.active()){ImGui::SameLine();if(ImGui::Button("Cancel Load")){replayOpenTask.cancel();recordingMessage.clear();}}
            if(!recordingMessage.empty())ImGui::TextWrapped("%s",recordingMessage.c_str());
        }ImGui::End();
    }
    void pollBalls(){
        for(size_t i=0;i<cameras.size();++i){auto& ptr=ballRuntime[cameras[i]->device.serial];if(!ptr)ptr=std::make_unique<BallRuntime>();auto& r=*ptr;auto option=ballOptions(i);
            if(r.options!=option||r.epoch!=sourceEpoch){r.task.cancel();r.result.reset();r.options=option;r.epoch=sourceEpoch;r.submitted=0;r.submittedPixels=nullptr;++r.revision;r.error.clear();views[i].generation=0;}
            try{if(auto completed=r.task.poll();completed&&completed->revision==r.revision){r.result=std::move(*completed);r.error.clear();views[i].generation=0;comparisonDirty=true;}}
            catch(const std::exception& e){r.error=e.what();}
            if(!settings.ball.run||!option.detect){r.task.cancel();r.result.reset();r.submitted=0;r.submittedPixels=nullptr;continue;}
            auto frame=sourceFrame(int(i));
            if(frame.image.empty()||r.task.pending()||(r.submitted==frame.generation&&r.submittedPixels==frame.image.data))continue;
            r.submitted=frame.generation;r.submittedPixels=frame.image.data;auto revision=r.revision;
            r.task.start([frame,option,revision,stats=r.stats](std::stop_token stop){const auto started=Clock::now();BallJob job{frame,detectBalls(frame.image,option,stop),revision};
                job.millis=std::chrono::duration<double,std::milli>(Clock::now()-started).count();stats->note(job.millis);return job;});
        }
        if(ballDirty&&Clock::now()-ballEdited>std::chrono::milliseconds(600))saveBallOptions();
    }
    BallJob* displayedBall(int i){
        if(!settings.ball.run||!ballOptions(i).detect)return nullptr;
        auto it=ballRuntime.find(cameras[i]->device.serial);if(it==ballRuntime.end()||!it->second->result)return nullptr;
        if(replay&&it->second->result->frame.generation!=cameras[i]->latest().generation)return nullptr;
        auto& options=ballOptions(i);if(!options.stage&&(!settings.ball.overlay||!options.overlay))return nullptr;
        return &*it->second->result;
    }
    cv::Mat ballStage(int i,const CameraFrame& frame){
        if(pointFrameActive(i))return frame.image;
        auto job=displayedBall(i);if(!job||job->frame.image.data!=frame.image.data||job->frame.generation!=frame.generation)return frame.image;
        auto stage=ballOptions(i).stage;if(stage==0||stage==4)return frame.image;cv::Mat out;
        cv::cvtColor(stage==1?job->detection.gray:stage==2?job->detection.blurred:job->detection.edges,out,cv::COLOR_GRAY2BGR);return out;
    }
    void drawBallOverlay(int i,uint64_t generation,ImVec2 a,ImVec2 size,cv::Size outputSize,int mode,int stereoSide=-1){
        auto& options=ballOptions(i);auto job=displayedBall(i);bool drawingRoi=roiCamera==i&&mode==0&&stereoSide<0;
        bool overlay=settings.ball.run&&settings.ball.overlay&&options.overlay&&options.detect;
        if(!drawingRoi&&!overlay)return;
        auto native=sourceFrame(i).image.size();if(native.empty())return;auto draw=ImGui::GetWindowDrawList();ImVec2 b{a.x+size.x,a.y+size.y};draw->PushClipRect(a,b,true);
        auto project=[&](const std::vector<cv::Point2f>& raw){std::vector<cv::Point2f> pixels=raw;
            if(mode){auto found=settings.cameras.find(cameras[i]->device.serial);if(found==settings.cameras.end())return std::vector<ImVec2>{};const auto& c=found->second.calibration;
                cv::undistortPoints(raw,pixels,c.K,c.distortion,mode==2?comparisonMaps.R[stereoSide]:cv::Mat(),mode==2?comparisonMaps.P[stereoSide]:c.K);}
            std::vector<ImVec2> screen;for(auto p:pixels){if(!std::isfinite(p.x)||!std::isfinite(p.y))return std::vector<ImVec2>{};screen.push_back({a.x+(p.x+.5f)*size.x/outputSize.width,a.y+(p.y+.5f)*size.y/outputSize.height});}return screen;};
        auto line=[&](const std::vector<cv::Point2f>& raw,ImU32 color,bool closed){auto points=project(raw);if(points.size()>1)draw->AddPolyline(points.data(),int(points.size()),color,closed?ImDrawFlags_Closed:0,1.8f);};
        if(drawingRoi||(options.showRoi&&options.roiEnabled&&!options.roi.empty())){
            const auto& polygon=drawingRoi?roiPoints:options.roi;std::vector<cv::Point2f> path;
            for(size_t n=0;n<polygon.size();++n){auto start=polygon[n],end=polygon[(n+1)%polygon.size()];int subdivisions=drawingRoi&&polygon.size()<4?1:16;
                for(int k=0;k<subdivisions;++k){auto p=start+(end-start)*(float(k)/subdivisions);path.emplace_back(p.x*native.width,p.y*native.height);}}
            line(path,IM_COL32(255,220,80,255),!drawingRoi||polygon.size()==4);
        }
        if(overlay&&job&&job->frame.generation==generation){
            auto circle=[&](const BallCircle& ball,ImU32 color,bool label){std::vector<cv::Point2f> ring;for(int n=0;n<64;++n){double angle=2*CV_PI*n/64;ring.push_back(ball.center+cv::Point2f(float(std::cos(angle)*ball.radius),float(std::sin(angle)*ball.radius)));}line(ring,color,true);
                auto center=project({ball.center});if(center.empty())return;auto p=center[0];if(options.showCenter){draw->AddLine({p.x-5,p.y},{p.x+5,p.y},color,1.5f);draw->AddLine({p.x,p.y-5},{p.x,p.y+5},color,1.5f);}
                if(label){char text[128];snprintf(text,sizeof(text),"%s (%.1f, %.1f) r %.1f",ball.color.c_str(),ball.center.x,ball.center.y,ball.radius);draw->AddText({p.x+8,p.y+5},color,text);}};
            if(options.showCandidates)for(const auto& c:job->detection.candidates)circle(c,IM_COL32(255,175,45,190),false);
            for(const auto& c:job->detection.balls)circle(c,c.color=="Red"?IM_COL32(255,95,95,255):c.color=="Blue"?IM_COL32(60,170,255,255):c.color=="White"?IM_COL32(255,255,255,255):IM_COL32(65,245,145,255),true);
        }
        if(drawingRoi){roiCanvas={a,b};draw->AddText({a.x+8,a.y+8},IM_COL32(255,225,100,255),"ROI: 모서리 4점 클릭 · Esc 취소");
            if(ImGui::IsItemHovered()&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)){auto p=ImGui::GetMousePos();roiPoints.emplace_back(std::clamp((p.x-a.x)/size.x,0.f,1.f),std::clamp((p.y-a.y)/size.y,0.f,1.f));
                if(roiPoints.size()==4){try{auto next=options;next.roi=roiPoints;next.roiEnabled=true;next.validate();options=next;roiCamera=-1;roiFrame={};markBallChanged();ballMessage.clear();}
                    catch(const std::exception& e){ballMessage=e.what();roiPoints.clear();}}}
            if(ImGui::IsKeyPressed(ImGuiKey_Escape)){roiCamera=-1;roiFrame={};roiPoints.clear();}}
        draw->PopClipRect();
    }
    void drawProcessing(){
        ImGui::SetNextWindowSize({485,690},ImGuiCond_FirstUseEver);
        if(processingSmokeActive){ImGui::SetNextWindowPos({915,35});ImGui::SetNextWindowSize({515,882});}
        if(ImGui::Begin("Ball Detection",&showProcessing)){
            ImGui::BeginDisabled(loadFailed||saveFuture.valid());auto before=settings.ball;
            ImGui::Checkbox("Run",&settings.ball.run);ImGui::SameLine();ImGui::Checkbox("Overlay",&settings.ball.overlay);
            if(settings.ball.run&&!cameras.empty()){   // Time per frame: detection per camera (the cameras run in parallel), the frame as a whole and the rate reached.
                std::string line;double slowest=0;int rate=1<<30,timed=0;
                for(size_t i=0;i<cameras.size();++i){auto it=ballRuntime.find(cameras[i]->device.serial);if(it==ballRuntime.end()||!ballOptions(i).detect)continue;
                    auto [millis,perSecond]=it->second->stats->read();if(millis<=0)continue;++timed;slowest=std::max(slowest,millis);rate=std::min(rate,perSecond);
                    line+=(line.empty()?"":"   ")+cameraName(cameras[i]->device.serial)+" "+std::to_string(int(std::lround(millis)))+" ms";}
                if(timed){ImGui::Text("검출 시간   %s",line.c_str());
                    ImGui::Text("한 프레임 %.0f ms (카메라 %d대 병렬) → 최대 %.1f fps, 지금 %d fps",slowest,timed,1000/slowest,rate);ImGui::SameLine();ImGui::TextDisabled("스테레오 매칭·3D 계산은 제외");}
                else ImGui::TextDisabled("검출 시간: 아직 처리한 프레임이 없습니다.");
            }
            {   // The learned detector (trained on the labeler's labels) for every camera, in place of Hough + RANSAC (2026-10-09).
                ImGui::SeparatorText("Model");
                const bool usingModel=!settings.ball.model.empty();
                if(usingModel){const auto file=fs::path(reinterpret_cast<const char8_t*>(settings.ball.model.c_str())).filename().u8string();ImGui::Text("ONNX: %s",std::string(file.begin(),file.end()).c_str());}
                else ImGui::TextDisabled("ONNX 모델 없음 · Hough + RANSAC 사용");
                auto applyModel=[&]{for(auto& [id,camera]:settings.ball.cameras){camera.model=settings.ball.model;camera.modelConfidence=settings.ball.modelConfidence;camera.cuda=settings.ball.cuda;}};
                if(ImGui::Button("ONNX 모델 고르기…")){if(auto chosen=chooseModelFile();!chosen.empty()){const auto text=chosen.u8string();settings.ball.model=std::string(text.begin(),text.end());applyModel();ballMessage="모든 카메라가 모델로 공을 찾습니다.";}}
                if(usingModel){ImGui::SameLine();if(ImGui::Button("모델 끄기")){settings.ball.model.clear();applyModel();}
                    float confidence=float(settings.ball.modelConfidence);ImGui::SetNextItemWidth(210);
                    if(ImGui::SliderFloat("Model Confidence",&confidence,.05f,.95f,"%.2f",ImGuiSliderFlags_AlwaysClamp)){settings.ball.modelConfidence=confidence;applyModel();}
                    int device=settings.ball.cuda?0:1;ImGui::SetNextItemWidth(150);
                    if(ImGui::Combo("실행 장치",&device,"CUDA (GPU)\0CPU\0")){settings.ball.cuda=device==0;applyModel();}
                    ImGui::SameLine();
                    if(auto loaded=yoloDetectorIfLoaded(fs::path(reinterpret_cast<const char8_t*>(settings.ball.model.c_str())),settings.ball.cuda))ImGui::TextDisabled("실행 중: %s",loaded->provider().c_str());
                    else ImGui::TextDisabled("첫 프레임에서 모델을 올립니다");
                    if(settings.ball.cuda){char folder[1024];std::snprintf(folder,sizeof folder,"%s",settings.ball.cudaDlls.c_str());ImGui::SetNextItemWidth(420);
                        if(ImGui::InputText("CUDA DLL 폴더",folder,sizeof folder)){settings.ball.cudaDlls=folder;yoloCudaLibraryDirectory(fs::path(reinterpret_cast<const char8_t*>(settings.ball.cudaDlls.c_str())));}
                        ImGui::TextDisabled("cudart·cublas·cudnn DLL이 프로그램 옆에 없을 때 그 폴더. 예: C:\\boccia-train\\.venv\\Lib\\site-packages\\torch\\lib");}
                    ImGui::TextDisabled("모델을 쓰면 Preprocess/Hough/RANSAC은 쓰지 않습니다. ROI와 색상 분류는 그대로입니다.");}
            }
            if(cameras.empty())ImGui::TextDisabled("카메라를 연결하거나 녹화 폴더를 여세요.");
            else {ballCamera=std::clamp(ballCamera,0,int(cameras.size())-1);ImGui::SetNextItemWidth(240);
                if(ImGui::BeginCombo("Camera",cameraName(cameras[ballCamera]->device.serial).c_str())){for(int i=0;i<int(cameras.size());++i)if(ImGui::Selectable(cameraName(cameras[i]->device.serial).c_str(),i==ballCamera))ballCamera=i;ImGui::EndCombo();}
                auto& o=ballOptions(ballCamera);int type=o.color?1:0;ImGui::SetNextItemWidth(110);if(ImGui::Combo("Type",&type,"Mono\0Color\0"))o.color=type==1;
                ImGui::SameLine();ImGui::Checkbox("Detect##camera",&o.detect);ImGui::SameLine();ImGui::Checkbox("Overlay##camera",&o.overlay);
                if(auto it=ballRuntime.find(cameras[ballCamera]->device.serial);it!=ballRuntime.end()){auto& r=*it->second;
                    if(r.result)ImGui::Text("%zu balls  |  %.1f ms  |  Frame %llu",r.result->detection.balls.size(),r.result->detection.milliseconds,(unsigned long long)r.result->frame.id);
                    if(!r.error.empty())ImGui::TextWrapped("%s",r.error.c_str());if(r.result&&r.result->detection.candidateLimit)ImGui::TextDisabled("후보 64개 제한 · ROI 또는 임계값을 조절하세요.");}
                ImGui::Separator();ImGui::SetNextItemWidth(190);ImGui::Combo("View",&o.stage,"Original\0Gray\0Blur\0Edges\0Result\0");
                ImGui::Checkbox("Candidates",&o.showCandidates);ImGui::SameLine();ImGui::Checkbox("Center",&o.showCenter);ImGui::SameLine();ImGui::Checkbox("ROI",&o.showRoi);
                ImGui::SeparatorText("ROI");ImGui::Checkbox("Use ROI",&o.roiEnabled);ImGui::SameLine();
                if(ImGui::Button(roiCamera==ballCamera?"Cancel ROI":"Draw 4 Points")){
                    if(roiCamera==ballCamera){roiCamera=-1;roiFrame={};roiPoints.clear();}
                    else {resumePointLive();roiFrame=sourceFrame(ballCamera);if(!roiFrame.image.empty()){roiCamera=ballCamera;roiPoints.clear();views[ballCamera].visible=true;views[ballCamera].generation=0;layout.cameraDisplay[cameras[ballCamera]->device.serial].mode=0;}}}
                ImGui::SameLine();if(ImGui::Button("Reset ROI")){o.roi.clear();roiCamera=-1;roiFrame={};roiPoints.clear();}
                auto slider=[&](const char* name,double& value,float lo,float hi,const char* format="%.2f"){float f=float(value);ImGui::SetNextItemWidth(210);if(ImGui::SliderFloat(name,&f,lo,hi,format,ImGuiSliderFlags_AlwaysClamp))value=f;};
                auto integer=[&](const char* name,int& value,int lo,int hi){ImGui::SetNextItemWidth(210);ImGui::SliderInt(name,&value,lo,hi,"%d",ImGuiSliderFlags_AlwaysClamp);};
                ImGui::BeginDisabled(!settings.ball.model.empty());   // with the model in use (Model, at the top) these settings do nothing
                ImGui::SeparatorText("Preprocess");slider("Brightness",o.brightness,-100,100,"%.0f");slider("Contrast",o.contrast,.25f,3);slider("Scale",o.scale,.25f,1);
                ImGui::SetNextItemWidth(210);ImGui::Combo("Blur",&o.blur,"None\0Median\0Gaussian\0Bilateral\0Box\0");int kernel=(o.kernel-1)/2;integer("Kernel",kernel,0,15);o.kernel=kernel*2+1;
                ImGui::SameLine();ImGui::TextDisabled("%d px",o.kernel);integer("Canny Low",o.cannyLow,0,499);integer("Canny High",o.cannyHigh,o.cannyLow+1,500);o.cannyHigh=std::max(o.cannyHigh,o.cannyLow+1);
                ImGui::SeparatorText("Hough");integer("Min Radius (px)",o.minRadius,1,500);integer("Max Radius (px)",o.maxRadius,o.minRadius,2000);o.maxRadius=std::max(o.maxRadius,o.minRadius);
                integer("Min Distance (px)",o.minDistance,1,1000);slider("dp",o.dp,1,4);integer("Edge Threshold",o.houghEdge,1,500);integer("Votes",o.houghVotes,1,300);
                ImGui::SeparatorText("RANSAC");ImGui::Checkbox("Refine Circle",&o.ransac);ImGui::BeginDisabled(!o.ransac);
                integer("Iterations",o.iterations,8,200);slider("Search Radius",o.reach,1,4);slider("Tolerance (px)",o.tolerance,.5f,10);slider("Min Coverage",o.minCoverage,0,1);ImGui::EndDisabled();
                ImGui::EndDisabled();   // the model
                if(o.color){ImGui::SeparatorText("Color / HSV");integer("Red Hue Width",o.redWidth,1,40);integer("Blue Hue Low",o.blueLow,0,178);integer("Blue Hue High",o.blueHigh,o.blueLow+1,179);o.blueHigh=std::max(o.blueHigh,o.blueLow+1);
                    integer("Min Saturation",o.minSaturation,0,255);integer("Min Value",o.minValue,0,255);integer("White Max S",o.whiteSaturation,0,255);integer("White Min V",o.whiteValue,0,255);slider("Color Fraction",o.colorFraction,.1f,1);}
                ImGui::Separator();if(!o.color&&ImGui::Button("Copy to Other Mono")){auto copy=o;for(size_t i=0;i<cameras.size();++i)if(i!=size_t(ballCamera)&&!ballOptions(i).color){auto roi=ballOptions(i).roi;bool enabled=ballOptions(i).detect,overlay=ballOptions(i).overlay;ballOptions(i)=copy;ballOptions(i).roi=roi;ballOptions(i).detect=enabled;ballOptions(i).overlay=overlay;}}
            }
            if(nlohmann::json(before)!=nlohmann::json(settings.ball))markBallChanged();ImGui::EndDisabled();
            if(!ballMessage.empty())ImGui::TextWrapped("%s",ballMessage.c_str());
        }ImGui::End();
    }
