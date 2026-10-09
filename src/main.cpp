#include "calibration.hpp"
#include "camera.hpp"
#include "settings.hpp"
#include "background_task.hpp"
#include "ui_layout.hpp"
#include "scene_renderer.hpp"
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <chrono>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
static double seconds(Clock::time_point t){return std::chrono::duration<double>(t.time_since_epoch()).count();}
static fs::path dataDirectory(bool smoke){
    if(smoke)return fs::temp_directory_path()/"boccia-ui-smoke";
    wchar_t module[32768]{};auto length=GetModuleFileNameW(nullptr,module,32768);
    if(length&&length<32768){auto directory=fs::path(module).parent_path();
        if(fs::is_regular_file(directory/"portable.txt"))return directory/"data";}
    const std::string path=BOCCIA_DATA_DIR;return fs::path(std::u8string(path.begin(),path.end()));
}
static std::string utf8(const fs::path& p){auto s=p.u8string();return {s.begin(),s.end()};}
static fs::path chooseFolder(){
    HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);IFileDialog* dialog=nullptr;fs::path result;
    if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))){
        DWORD flags=0;dialog->GetOptions(&flags);dialog->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_NOCHANGEDIR);
        if(SUCCEEDED(dialog->Show(static_cast<HWND>(ImGui::GetWindowViewport()->PlatformHandleRaw)))){IShellItem* item=nullptr;
            if(SUCCEEDED(dialog->GetResult(&item))){PWSTR path=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){result=path;CoTaskMemFree(path);}item->Release();}}
        dialog->Release();}
    if(SUCCEEDED(initialized))CoUninitialize();return result;
}
// The ball detector model (ONNX) for the processing panel.
static fs::path chooseModelFile(){
    std::vector<wchar_t> buffer(4096,0);OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);
    ofn.hwndOwner=static_cast<HWND>(ImGui::GetWindowViewport()->PlatformHandleRaw);
    ofn.lpstrFile=buffer.data();ofn.nMaxFile=DWORD(buffer.size());ofn.lpstrTitle=L"공 검출 모델 선택 (ONNX)";
    ofn.lpstrFilter=L"ONNX model\0*.onnx\0All files\0*.*\0";
    ofn.Flags=OFN_EXPLORER|OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetOpenFileNameW(&ofn))return {};
    return fs::path(buffer.data());
}
static std::vector<fs::path> chooseImages(){
    std::vector<wchar_t> buffer(65536,0);OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);
    ofn.hwndOwner=static_cast<HWND>(ImGui::GetWindowViewport()->PlatformHandleRaw);
    ofn.lpstrFile=buffer.data();ofn.nMaxFile=DWORD(buffer.size());ofn.lpstrTitle=L"보정 이미지 선택 (같은 카메라의 이미지)";
    ofn.lpstrFilter=L"Images\0*.png;*.bmp;*.jpg;*.jpeg;*.tif;*.tiff\0All files\0*.*\0";
    ofn.Flags=OFN_ALLOWMULTISELECT|OFN_EXPLORER|OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetOpenFileNameW(&ofn))return {};
    std::vector<fs::path> result;fs::path first=buffer.data();const wchar_t* next=buffer.data()+wcslen(buffer.data())+1;
    if(!*next)result.push_back(first);
    else while(*next){result.push_back(first/next);next+=wcslen(next)+1;}
    return result;
}
static cv::Mat readImage(const fs::path& p){
    std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("Cannot open image: "+utf8(p));
    std::vector<uchar> bytes((std::istreambuf_iterator<char>(f)),{});
    auto image=cv::imdecode(bytes,cv::IMREAD_COLOR);if(image.empty())throw std::runtime_error("Unsupported image: "+utf8(p));return image;
}
struct Texture{
    GLuint id=0;cv::Size size;
    void update(const cv::Mat& bgr,double maxWidth=1600){
        if(bgr.empty())return;cv::Mat scaled;
        double factor=std::min(1.0,maxWidth/bgr.cols);cv::resize(bgr,scaled,{},factor,factor,cv::INTER_AREA);
        if(!id)glGenTextures(1,&id);glBindTexture(GL_TEXTURE_2D,id);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,scaled.cols,scaled.rows,0,GL_BGR,GL_UNSIGNED_BYTE,scaled.data);
        size=scaled.size();
    }
    void draw(float maxHeight=100000){
        if(!id)return;auto available=ImGui::GetContentRegionAvail();
        float scale=std::min(available.x/size.width,std::min(available.y,maxHeight)/size.height);
        if(scale>0)ImGui::Image(ImTextureRef(ImTextureID(id)),{size.width*scale,size.height*scale});
    }
    void release(){if(id){glDeleteTextures(1,&id);id=0;}}
};
struct ImportResult{std::vector<Observation> samples;std::vector<std::string> errors;};
struct DetectionResult{Observation observation;std::string identity;Clock::time_point capturedAt;};
struct ReconnectResult{std::vector<Device> devices;std::vector<std::unique_ptr<Camera>> cameras;std::string warnings;};
struct StereoDetection {RigSample sample;std::vector<std::string> serials;std::vector<Clock::time_point> receivedAt;double detectionMs=0;};
struct ViewState{
    bool visible=true;uint64_t generation=0;Texture texture;BrightnessOptions brightness;
    Clock::time_point updated{};std::string mapKey,error;cv::Mat mapX,mapY;
};
class Application{
public:
    Application(bool test,GLFWwindow* window,UiLayout restored,bool hasLayout,bool layoutWritable):smoke(test),mainWindow(window),layout(std::move(restored)),restoredLayout(hasLayout),canSaveLayout(layoutWritable){
        data=dataDirectory(smoke);
        profile=data/"settings.json";
        try{settings=loadSettings(profile);board=settings.board;confirmed=settings.boardConfirmed;autoOptions=settings.autoCapture;}
        catch(const std::exception& e){loadFailed=true;deviceError=std::string("설정 파일 오류: ")+e.what();message=deviceError;}
        updateDeviceList({});
        showDevices=layout.devicesVisible;showCalibration=layout.calibrationVisible;showProcessing=layout.processingVisible;showRecording=layout.recordingVisible;
        showStereo=layout.stereoVisible;if(showStereo)showCalibration=false;
        showOrigin=layout.originVisible;if(showOrigin){showCalibration=false;showStereo=false;}
        stereoBoard=settings.board;stereoConfirmed=settings.boardConfirmed;
        stereoCount=settings.relativeCameraCount;
        initialGuide=settings.cameras.empty()&&!restoredLayout;
        if(smoke){offline=true;snprintf(offlineId,sizeof(offlineId),"offline-demo");identity=offlineId;confirmed=true;createDemo();}
        else if(!loadFailed&&settings.autoReconnect&&!settings.selectedSerials.empty())beginReconnect();
    }
    ~Application(){stopRecording();replayTask.cancel();replayOpenTask.cancel();for(auto& [id,r]:ballRuntime)r->task.cancel();cancelOrigin();originDetectTask.wait();originPoseTask.wait();reconnectTask.cancel();cancelStereo();cancelWork();for(auto& c:cameras)c->close();reconnectTask.wait();stereoDetectTask.wait();stereoCalTask.wait();detectTask.wait();calTask.wait();importTask.wait();if(saveFuture.valid()){try{settings=saveFuture.get();}catch(...){}}saveBallOptions();}
    void releaseThumbnails(){for(auto& t:thumbnails)t.release();thumbnails.clear();}
    void releaseTextures(){sceneRenderer.release();for(auto& t:comparisonTextures)t.release();for(auto& t:originPreview)t.release();preview.release();correctedPreview.release();releaseThumbnails();releaseStereoThumbnails();for(auto& t:stereoPreview)t.release();for(auto& v:views)v.texture.release();}
    void draw(){
        pollJobs();pollSource();pollBalls();pollPoint();
        ImGui::DockSpaceOverViewport(0,ImGui::GetMainViewport());
        if(ImGui::BeginMainMenuBar()){
            if(ImGui::BeginMenu("도구")){
                if(ImGui::MenuItem("개별 보정",nullptr,false,!saveFuture.valid()&&!replay&&!sourceBusy()))openCalibration();
                if(ImGui::MenuItem("상대 위치 보정",nullptr,false,!saveFuture.valid()&&!replay&&!sourceBusy()))openStereo();
                if(ImGui::MenuItem("원점 설정",nullptr,false,!saveFuture.valid()&&!replay&&!sourceBusy()))openOrigin();
                ImGui::Separator();ImGui::MenuItem("Ball Detection",nullptr,&showProcessing);
                ImGui::MenuItem("Recording / Playback",nullptr,&showRecording);
                ImGui::MenuItem("Triangulation",nullptr,&layout.triangulationVisible);
                ImGui::EndMenu();}
            if(ImGui::BeginMenu("보기")){
                ImGui::MenuItem("장치 / 설정",nullptr,&showDevices);
                ImGui::MenuItem("3D View",nullptr,&layout.sceneVisible);
                ImGui::MenuItem("Stereo Compare",nullptr,&layout.comparisonVisible);
                for(size_t i=0;i<cameras.size();++i)ImGui::MenuItem((cameraName(cameras[i]->device.serial)+"###"+cameras[i]->device.serial).c_str(),nullptr,&views[i].visible);
                for(auto& [serial,visible]:layout.cameraWindows)if(!connectedSerial(serial))ImGui::MenuItem((cameraName(serial)+" (미연결)###offline-"+serial).c_str(),nullptr,&visible);
                ImGui::EndMenu();
            }
            if(recordingBusy())ImGui::TextColored({1,.35f,.3f,1},"REC");
            if(replay)ImGui::TextDisabled("Playback");
            ImGui::EndMainMenuBar();
        }
        if(initialGuide){ImGui::OpenPopup("첫 보정 안내");initialGuide=false;}
        ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,{.5f,.5f});
        if(ImGui::BeginPopupModal("첫 보정 안내",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextUnformatted("저장된 카메라 보정값이 없습니다.");

            if(ImGui::Button("캘리브레이션 시작",{220,36})){openCalibration();ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button("나중에",{90,36}))ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if(showDevices)drawDevices();
        if(showRecording)drawRecording();
        if(showProcessing)drawProcessing();
        if(layout.triangulationVisible)drawTriangulation();
        for(size_t i=0;i<cameras.size();++i)drawCamera(i);
        if(layout.comparisonVisible)drawComparison();
        rememberCameraWindows();
        for(auto& [serial,visible]:layout.cameraWindows)if(!connectedSerial(serial)&&visible){
            ImGui::SetNextWindowSize({700,500},ImGuiCond_FirstUseEver);
            if(ImGui::Begin(("영상 / "+cameraName(serial)+"###영상 / "+serial).c_str(),&visible))ImGui::TextWrapped("%s · 연결 대기",serial.c_str());
            ImGui::End();
        }
        if(showCalibration)drawCalibration();
        if(showStereo)drawStereo();
        if(showOrigin)drawOrigin();
        if(layout.sceneVisible)drawScene();
    }
    void saveLayout(bool force=false){
        if(smoke||!canSaveLayout)return;
        if(!glfwGetWindowAttrib(mainWindow,GLFW_ICONIFIED)){
            layout.maximized=glfwGetWindowAttrib(mainWindow,GLFW_MAXIMIZED)!=0;
            if(!layout.maximized){
                glfwGetWindowPos(mainWindow,&layout.mainWindow.x,&layout.mainWindow.y);
                glfwGetWindowSize(mainWindow,&layout.mainWindow.width,&layout.mainWindow.height);
            }
        }
        if(!force&&Clock::now()-lastLayoutSave<std::chrono::seconds(2))return;
        lastLayoutSave=Clock::now();
        try{
            rememberCameraWindows();layout.devicesVisible=showDevices;layout.calibrationVisible=showCalibration;
            layout.stereoVisible=showStereo;layout.originVisible=showOrigin;layout.processingVisible=showProcessing;layout.recordingVisible=showRecording;
            size_t length=0;const char* ini=ImGui::SaveIniSettingsToMemory(&length);layout.imgui.assign(ini,length);
            ImGui::GetIO().WantSaveIniSettings=false;
            auto serialized=serializeUiLayout(layout);
            if(serialized!=lastLayoutContent){writeUiLayout(data/"ui-layout.json",layout);lastLayoutContent=std::move(serialized);}
        }catch(const std::exception& e){layoutWarning=std::string("화면 배치 저장 실패: ")+e.what();}
    }
    std::string layoutWarning;
    bool smoke=false;
    void prepareStereoSmoke(){
        if(!smoke)throw std::logic_error("Synthetic setup requires smoke mode.");
        cameras.push_back(std::make_unique<Camera>(Device{"synthetic-A","SYNTHETIC-A","MONO TEST"}));
        cameras.push_back(std::make_unique<Camera>(Device{"synthetic-B","SYNTHETIC-B","MONO TEST"}));
        cameras.push_back(std::make_unique<Camera>(Device{"synthetic-C","SYNTHETIC-C","COLOR TEST"}));views.resize(3);stereoCount=3;
        settings.softwareBrightness["SYNTHETIC-A"]={true,true,30,110};
        offline=false;cameraIndex=0;showCalibration=true;detectionEnabled=true;result.reset();exported=false;
        auto require=[](bool ok){if(!ok)throw std::runtime_error("Brightness lifecycle smoke check failed.");};
        require(liveBrightness("SYNTHETIC-A").enabled&&!liveBrightness("SYNTHETIC-B").enabled);
        result=Calibration{};require(!liveBrightness("SYNTHETIC-A").enabled);result.reset();
        cancelWork();require(!liveBrightness("SYNTHETIC-A").enabled);
        detectionEnabled=true;showCalibration=false;require(!liveBrightness("SYNTHETIC-A").enabled);
        showStereo=true;stereoDetectionEnabled=true;require(liveBrightness("SYNTHETIC-A").enabled);
        cancelStereo();require(!liveBrightness("SYNTHETIC-A").enabled);
        for(const auto& s:samples)stereoSamples.push_back({{s,s,s},12});
        if(!stereoSamples.empty())stereoDetected.sample=stereoSamples.front();
        StereoCalibration r;r.R=cv::Mat::eye(3,3,CV_64F);r.T=(cv::Mat_<double>(3,1)<<-.12,.002,0);
        r.rms=.4;r.perPairErrors.assign(stereoSamples.size(),.4);r.secondRotations.assign(stereoSamples.size(),0);auto c=r;c.T=(cv::Mat_<double>(3,1)<<.08,-.05,.01);
        stereoResult=RigCalibration{{r,c}};
        stereoMessage="합성 UI 검증 화면 · 실제 카메라 상대 위치가 아닙니다.";
        std::cout<<"PASS: brightness restored on calculation, cancellation, close and stereo stop\n";
    }
    void prepareStereoWorkflowSmoke(){
        prepareStereoSmoke();resetStereo();settings=Settings{};stereoBoard=BoardConfig{};stereoBoard.columns=8;stereoBoard.rows=6;
        stereoAutoOptions={12,.7,1.5};stereoConfirmed=false;stereoSmokeLive=true;showDevices=showCalibration=showOrigin=layout.sceneVisible=false;
        for(auto& view:views)view.visible=false;
        Calibration intrinsic;intrinsic.imageSize={960,720};intrinsic.K=(cv::Mat_<double>(3,3)<<880,0,480,0,875,360,0,0,1);intrinsic.distortion=(cv::Mat_<double>(1,5)<<-.05,.01,.001,-.0005,0);intrinsic.rms=.1;
        for(int side=0;side<3;++side){SavedCamera saved;saved.identity=cameras[side]->device.serial;saved.sourceKind="live";saved.archive="synthetic-intrinsic-"+std::to_string(side);saved.board=stereoBoard;saved.calibration=intrinsic;settings.cameras[saved.identity]=saved;}
        settings.softwareBrightness[cameras[0]->device.serial]={true,true,30,210};
        for(int n=0;n<12;++n){RigSample group;group.receiptSkewMs=10;
            cv::Vec3d rotation(-.3+(n%4)*.17,-.24+(n%5)*.12,-.10+(n%3)*.10),translation(-.10+(n%4)*.025,-.06+(n%3)*.015,.65+(n%4)*.06);
            for(int side=0;side<3;++side){Observation o;o.image=cv::Mat(intrinsic.imageSize,CV_8UC3,cv::Scalar(180,180,180));
                cv::Vec3d offset=side==1?cv::Vec3d(-.12,.002,0):side==2?cv::Vec3d(.08,-.05,.01):cv::Vec3d();
                for(int y=0;y<stereoBoard.rows;++y)for(int x=0;x<stereoBoard.columns;++x)o.objectPoints.emplace_back(float(x*.025),float(y*.025),0.f);
                cv::projectPoints(o.objectPoints,rotation,translation+offset,intrinsic.K,intrinsic.distortion,o.imagePoints);
                for(int y=-1;y<stereoBoard.rows;++y)for(int x=-1;x<stereoBoard.columns;++x){std::vector<cv::Point3f> corners{{float(x*.025),float(y*.025),0},{float((x+1)*.025),float(y*.025),0},{float((x+1)*.025),float((y+1)*.025),0},{float(x*.025),float((y+1)*.025),0}};
                    std::vector<cv::Point2f> projected;cv::projectPoints(corners,rotation,translation+offset,intrinsic.K,intrinsic.distortion,projected);std::vector<cv::Point> polygon;for(auto p:projected)polygon.emplace_back(cvRound(p.x),cvRound(p.y));cv::fillConvexPoly(o.image,polygon,(x+y)%2?cv::Scalar(235,235,235):cv::Scalar(35,35,35),cv::LINE_AA);}
                o.source="live:"+cameras[side]->device.serial;o.frameId=n+1;o.softwareBrightness=savedBrightness(cameras[side]->device.serial);group.cameras.push_back(std::move(o));}
            stereoSmokeSamples.push_back(std::move(group));}
        for(int side=0;side<3;++side)stereoSmokeFrames[side]={stereoSmokeSamples[0].cameras[side].image,1,0,1,Clock::now()};
        stereoSmokeOriginal=stereoSmokeSamples.back().cameras[0].image.clone();stereoMessage="Synthetic UI test";
    }
    bool stereoSmokeCollected=false,stereoSmokeFolded=false;int stereoSmokeResumeFrame=0;
    void stereoInputSmoke(int frame){
        auto require=[](bool ok,const char* text){if(!ok)throw std::runtime_error(text);};
        if(frame>=10&&!stereoSmokeCollected){
            stereoSmokeFolded=frame>10;
            for(auto& source:stereoSmokeFrames){source.receivedAt=Clock::now();++source.generation;++source.id;}
            if(frame==10){require(stereoPreview[0].id&&stereoPreview[1].id&&stereoPreview[2].id&&!stereoConfirmed&&stereoSamples.empty(),"Stereo raw live preview must precede detection");
                beginStereoCapture();require(stereoWorkflow&&stereoCapture.running(),"Start must begin automatic rig collection");}
            if(stereoSamples.empty())return;
            require(stereoSamples.size()==1&&stereoSamples[0].cameras.size()==3,"Actual detection and stable hold must automatically append a complete group");
            for(const auto& observation:stereoSamples[0].cameras)require(observation.imagePoints.size()==48,"All three real detector results must have every board corner");
            require(stereoWorkflow&&stereoSmokeFolded,"Folding the relative-calibration window must not cancel automatic capture");
            stereoSmokeCollected=true;stereoSmokeFolded=false;stereoSmokeResumeFrame=frame-10;
            std::cout<<"PASS: actual three-camera detection-to-automatic-capture loop while the calibration window is folded\n";
            stereoSamples.assign(stereoSmokeSamples.begin(),stereoSmokeSamples.begin()+8);stereoGenerations={};
        }
        frame-=stereoSmokeResumeFrame;
        if(frame==18){cancelStereo();require(stereoSamples.size()==8&&!stereoWorkflow&&!stereoBrightnessActive()&&!stereoSmokeFrames[0].image.empty(),"Cancel must preserve rig samples and live Grab");}
        if(frame==23){beginStereoCapture();stereoGenerations={1,1,1};require(stereoWorkflow&&stereoSamples.size()==8,"Restart must resume partial rig samples");}
        if(frame==27){stereoSamples=stereoSmokeSamples;advanceStereoCalibration();require(stereoCalTask.pending()&&!stereoWorkflow&&!stereoDetectionEnabled&&!stereoBrightnessActive(),"Rig target must automatically solve and restore brightness");advanceStereoCalibration();}
    }
    bool stereoSmokePassed()const{
        if(!stereoResult||stereoPreviewDirty||stereoCalTask.pending())return false;
        if(stereoResult->fromReference.size()!=2||stereoResult->fromReference[0].rms>.001||stereoResult->fromReference[1].rms>.001||stereoSelected!=11||cv::norm(stereoSmokeOriginal,stereoSamples.back().cameras[0].image,cv::NORM_INF)!=0)
            throw std::runtime_error("Stereo automatic solve/raw preservation smoke failed.");
        return true;
    }
    void prepareOriginSmoke(){
        prepareStereoSmoke();openOrigin();showDevices=false;
        detected=samples.front();
        for(auto& v:views)v.visible=false;
        Settings::SavedRig saved;saved.serials={"SYNTHETIC-A","SYNTHETIC-B","SYNTHETIC-C"};saved.archive="synthetic-rig";saved.board=board;saved.calibration=*stereoResult;
        Calibration k;k.imageSize=detected.image.size();k.K=(cv::Mat_<double>(3,3)<<1400,0,500,0,1400,360,0,0,1);k.distortion=cv::Mat::zeros(1,5,CV_64F);
        for(const auto& id:saved.serials){SavedCamera c;c.identity=id;c.sourceKind="live";c.archive="synthetic-"+id;c.calibration=k;c.board=board;settings.cameras[id]=c;saved.intrinsicArchives.push_back(c.archive);}
        settings.rig=saved;originRig=saved;originSample={{detected,detected,detected},0};
        BoardPose p;cv::Mat rv;cv::solvePnP(detected.objectPoints,detected.imagePoints,k.K,k.distortion,rv,p.T);cv::Rodrigues(rv,p.R);p.errors={.2,.3,.4};originPose=p;rebuildOrigin();
        originMessage="합성 UI 검증 화면 · 실제 원점 측정값이 아닙니다.";
    }
    void prepareSceneSmoke(){
        prepareOriginSmoke();showOrigin=false;layout.sceneVisible=true;
        settings.origin=Settings::SavedOrigin{settings.rig->archive,"synthetic-origin",*originCandidate};
        layout.sceneView={};layout.sceneOptions={};
    }
    void prepareSavedSceneSmoke(){
        if(!smoke)throw std::logic_error("Hidden scene requires smoke mode.");
        settings=loadSettings(dataDirectory(false)/"settings.json");
        showCalibration=showStereo=showOrigin=showDevices=false;layout.sceneVisible=true;
        layout.sceneView={};layout.sceneOptions={};
    }
    bool sceneSmokePassed()const{return sceneError.empty()&&sceneCameras.size()==3;}
    void prepareDeviceSmoke(){
        prepareOriginSmoke();showOrigin=false;showDevices=true;
        settings.rig->serials.resize(2);settings.rig->intrinsicArchives.resize(2);settings.rig->calibration.fromReference.resize(1);
        auto world=*originCandidate;world.errors.resize(2);
        settings.origin=Settings::SavedOrigin{settings.rig->archive,"synthetic-origin",world};
        settings.cameras["SYNTHETIC-C"].sourceKind="offline";
        settings.selectedSerials={"SYNTHETIC-A","SYNTHETIC-B","SYNTHETIC-C"};
        std::vector<Device> found;for(const auto& c:cameras)found.push_back(c->device);updateDeviceList(found);
        if(!deviceSelected("SYNTHETIC-B")||deviceCalState("SYNTHETIC-C")!=RecordState::Stale||devicePose("SYNTHETIC-C").state!=RecordState::Missing)
            throw std::runtime_error("Device record state smoke failed.");
        selectedDevices[1]=false;std::reverse(found.begin(),found.end());updateDeviceList(found);
        if(deviceSelected("SYNTHETIC-B")||!deviceSelected("SYNTHETIC-A"))throw std::runtime_error("Scan lost serial selection.");
        std::reverse(found.begin(),found.end());updateDeviceList(found);
    }
    void deviceInputSmoke(int frame){
        auto& io=ImGui::GetIO();
        if(frame==12){io.AddFocusEvent(true);io.AddMousePosEvent(deviceCalPoint.x,deviceCalPoint.y);io.AddMouseButtonEvent(0,true);}
        if(frame==13)io.AddMouseButtonEvent(0,false);
        if(frame==18&&!deviceCalSeen)throw std::runtime_error("Cal popup did not open.");
        if(frame==22){io.AddMousePosEvent(deviceClosePoint.x,deviceClosePoint.y);io.AddMouseButtonEvent(0,true);}
        if(frame==23)io.AddMouseButtonEvent(0,false);
        if(frame==25)io.AddMousePosEvent(devicePosePoint.x,devicePosePoint.y);
        if(frame==26)io.AddMouseButtonEvent(0,true);
        if(frame==27)io.AddMouseButtonEvent(0,false);
        if(frame==32&&!devicePoseSeen)throw std::runtime_error("Pose popup did not open.");
        if(frame==42){io.AddMousePosEvent(deviceClosePoint.x,deviceClosePoint.y);io.AddMouseButtonEvent(0,true);}
        if(frame==43)io.AddMouseButtonEvent(0,false);
        if(frame==45)io.AddMousePosEvent(deviceWorldPoint.x,deviceWorldPoint.y);
        if(frame==46)io.AddMouseButtonEvent(0,true);
        if(frame==47)io.AddMouseButtonEvent(0,false);
        if(frame==52){if(!deviceWorldSeen||deviceWorldPose("SYNTHETIC-A").state!=RecordState::Saved||deviceWorldPose("SYNTHETIC-C").state!=RecordState::Missing)
            throw std::runtime_error("World popup/state smoke failed.");std::cout<<"PASS: device selection survives Scan; Cal/Pose/World icon popups open\n";}
    }
    void sceneInputSmoke(int frame){
        auto& io=ImGui::GetIO();static cv::Vec3d before;static double yawBefore=0;
        auto require=[](bool ok){if(!ok)throw std::runtime_error("3D FPS input smoke failed.");};
        if(frame==2){io.AddFocusEvent(true);io.AddMousePosEvent(sceneCanvasCenter.x,sceneCanvasCenter.y);io.AddMouseButtonEvent(0,true);}
        if(frame==3)io.AddMouseButtonEvent(0,false);
        if(frame==4){before=layout.sceneView.eye;io.AddKeyEvent(ImGuiKey_W,true);}
        if(frame==8){require(cv::norm(layout.sceneView.eye-before)>1e-5);io.AddKeyEvent(ImGuiKey_W,false);}
        if(frame==10){io.AddMousePosEvent(5,5);}
        if(frame==11){before=layout.sceneView.eye;io.AddKeyEvent(ImGuiKey_W,true);}
        if(frame==15){require(cv::norm(layout.sceneView.eye-before)<1e-10);io.AddKeyEvent(ImGuiKey_W,false);io.AddMousePosEvent(sceneCanvasCenter.x,sceneCanvasCenter.y);}
        if(frame==17){yawBefore=layout.sceneView.yaw;io.AddMouseButtonEvent(1,true);}
        if(frame==18)io.AddMousePosEvent(sceneCanvasCenter.x+25,sceneCanvasCenter.y+8);
        if(frame==20){require(std::abs(layout.sceneView.yaw-yawBefore)>.01);io.AddMouseButtonEvent(1,false);}
        if(frame==22){yawBefore=layout.sceneView.yaw;io.AddMouseButtonEvent(0,true);}
        if(frame==23)io.AddMousePosEvent(sceneCanvasCenter.x-30,sceneCanvasCenter.y-10);
        if(frame==25){require(std::abs(layout.sceneView.yaw-yawBefore)>.01);io.AddMouseButtonEvent(0,false);}
        if(frame==27){yawBefore=layout.sceneView.yaw;io.AddKeyEvent(ImGuiKey_RightArrow,true);}
        if(frame==31){require(std::abs(layout.sceneView.yaw-yawBefore)>1e-5);io.AddKeyEvent(ImGuiKey_RightArrow,false);}
        if(frame==33){frameScene(layout.sceneView,sceneCameras);std::cout<<"PASS: FPS movement, input isolation, left/right drag and arrow-key rotation\n";}
    }
    void prepareMonoSmoke(){
        if(!smoke)throw std::logic_error("Synthetic setup requires smoke mode.");
        resetSession();settings=Settings{};board=BoardConfig{};autoOptions={22,.7,1.5};confirmed=false;
        showDevices=showStereo=showOrigin=layout.sceneVisible=false;showCalibration=true;offline=false;monoSmokeLive=true;
        cameras.clear();cameras.push_back(std::make_unique<Camera>(Device{"mono-test","MONO-TEST","SYNTHETIC"}));views.resize(1);views[0].visible=false;
        cameraIndex=0;identity="MONO-TEST";settings.softwareBrightness[identity]={true,true,30,210};
        cv::Mat K=(cv::Mat_<double>(3,3)<<1000,0,640,0,980,480,0,0,1),D=(cv::Mat_<double>(1,5)<<-.08,.015,.001,-.0005,0);
        for(int i=0;i<22;++i){Observation o;o.image=cv::Mat(960,1280,CV_8UC3,cv::Scalar(180,180,180));
            for(int y=0;y<board.rows;++y)for(int x=0;x<board.columns;++x)o.objectPoints.emplace_back(float(x*.025),float(y*.025),0.f);
            cv::Vec3d r(-.4+(i%5)*.17,-.32+(i%7)*.11,-.12+(i%3)*.1),t(-.16+(i%4)*.025,-.10+(i%6)*.016,.65+(i%5)*.065);
            cv::projectPoints(o.objectPoints,r,t,K,D,o.imagePoints);
            for(int y=-1;y<board.rows;++y)for(int x=-1;x<board.columns;++x){
                std::vector<cv::Point3f> corners{{float(x*.025),float(y*.025),0},{float((x+1)*.025),float(y*.025),0},{float((x+1)*.025),float((y+1)*.025),0},{float(x*.025),float((y+1)*.025),0}};
                std::vector<cv::Point2f> points;cv::projectPoints(corners,r,t,K,D,points);std::vector<cv::Point> polygon;for(auto p:points)polygon.emplace_back(cvRound(p.x),cvRound(p.y));
                cv::fillConvexPoly(o.image,polygon,(x+y)%2?cv::Scalar(235,235,235):cv::Scalar(35,35,35),cv::LINE_AA);}
            o.source="live:"+identity;o.frameId=i+1;o.softwareBrightness=settings.softwareBrightness[identity];monoSmokeSamples.push_back(std::move(o));}
        monoSmokeFrame={monoSmokeSamples[0].image,1,0,1,Clock::now()};monoSmokeOriginal=monoSmokeSamples.back().image.clone();
        message="Synthetic UI test";
    }
    void monoInputSmoke(int frame){
        auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
        if(frame==12){require(preview.id&&!confirmed&&samples.empty(),"Raw live preview must precede board setup/detection");startMonoCalibration();require(monoWorkflow&&autoCapture.running(),"Start must collect automatically");samples.assign(monoSmokeSamples.begin(),monoSmokeSamples.begin()+8);sessionGeometry={};}
        if(frame==14){
            detected=detectBoard(monoSmokeFrame.image,board,liveBrightness(identity));
            require(detected.imagePoints.size()==54,"Mono overlay requires a real complete detector result");
            detected.source="live:"+identity;detected.frameId=monoSmokeFrame.id;detectedAt=Clock::now();
            // The next Grab frame is a different pose: the overlay must still use the detection image.
            monoSmokeFrame.image=monoSmokeSamples[1].image;++monoSmokeFrame.id;++monoSmokeFrame.generation;
            require(monoSmokeFrame.id!=detected.frameId,"Overlay smoke must exercise mismatched live/detection frame IDs");
            previewDirty=true;
        }
        if(frame==20){cancelWork();require(liveRunning()&&samples.size()==8&&!monoWorkflow&&!calibrationBrightnessActive(),"Cancel must preserve Grab/samples and restore brightness");}
        if(frame==25){startMonoCalibration();require(monoWorkflow&&samples.size()==8,"Restart must resume partial samples");}
        if(frame==30){samples=monoSmokeSamples;advanceMonoCalibration();require(calTask.pending()&&!detectionEnabled&&!monoWorkflow&&!calibrationBrightnessActive(),"Target must launch solver once and restore brightness");advanceMonoCalibration();}
    }
    bool monoSmokePassed()const{
        if(!result||!preview.id||!correctedPreview.id||previewDirty)return false;
        if(result->rms>.001||selectedSample!=21||calTask.pending()||cv::norm(monoSmokeOriginal,samples.back().image,cv::NORM_INF)!=0)
            throw std::runtime_error("Mono solve/compare/raw preservation smoke failed.");
        return true;
    }
    void prepareVideoSmoke(){
        prepareOriginSmoke();showCalibration=showStereo=showOrigin=showDevices=layout.sceneVisible=false;
        videoSmoke=true;for(auto& v:views)v.visible=false;views[0].visible=true;
        for(int i=0;i<3;++i)videoSmokeFrames[i]={samples.at(i).image,uint64_t(i+1),0,1,Clock::now()};
        layout.cameraDisplay.clear();layout.cameraDisplay["SYNTHETIC-A"]={0,25,1.25f};
        layout.comparisonSerials={"SYNTHETIC-A","SYNTHETIC-B"};layout.comparisonMode=1;layout.comparisonVisible=false;
        videoSmokeOriginal=videoSmokeFrames[0].image.clone();
    }
    void videoInputSmoke(int frame){
        auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
        for(int i=0;i<3;++i)videoSmokeFrames[i].receivedAt=Clock::now()-std::chrono::milliseconds(i);
        if(frame==12){require(views[0].texture.id&&views[0].error.empty(),"Camera raw/display adjustment failed");layout.cameraDisplay["SYNTHETIC-A"].mode=1;views[0].generation=0;}
        if(frame==22){require(!views[0].mapX.empty()&&views[0].error.empty(),"Camera undistortion failed");views[0].visible=false;layout.comparisonVisible=true;}
        if(frame==28){require(comparisonTextures[0].id&&comparisonTextures[1].id&&comparisonError.empty()&&comparisonGuideError.empty()&&comparisonGuides[0].size()==4,"Undistorted comparison failed");layout.comparisonMode=1;comparisonDirty=true;}
        if(frame==34){require(comparisonError.empty(),"Undistorted comparison failed");layout.comparisonMode=2;comparisonDirty=true;}
        if(frame==44){require(!comparisonMaps.mapX[0].empty()&&comparisonError.empty(),"Rectified comparison failed");settings.cameras["SYNTHETIC-C"].archive+="-changed";}
        if(frame==47){require(!comparisonError.empty(),"Stale rig was accepted");settings.cameras["SYNTHETIC-C"].archive="synthetic-SYNTHETIC-C";}
        if(frame==50){require(comparisonError.empty(),"Restored rig failed");std::swap(layout.comparisonSerials[0],layout.comparisonSerials[1]);comparisonDirty=true;}
        if(frame==54){require(comparisonError.empty(),"Reversed stereo pair failed");layout.comparisonSerials={"SYNTHETIC-B","SYNTHETIC-C"};comparisonDirty=true;}
        if(frame==60){require(comparisonError.empty()&&cv::norm(videoSmokeOriginal,videoSmokeFrames[0].image,cv::NORM_INF)==0,"Arbitrary pair/raw preservation failed");std::cout<<"PASS: camera display options, comparison modes, reversed/arbitrary pairs, stale gating and raw preservation\n";}
    }
    bool triangulationSmoke=false;
    std::array<cv::Point2d,2> pointSmokePixels;
    const cv::Vec3d pointSmokeExpected{.08,.035,.5};
    void prepareTriangulationSmoke(){
        prepareVideoSmoke();triangulationSmoke=true;showRecording=showProcessing=false;settings.ball.run=false;settings.softwareBrightness.clear();
        layout.triangulationVisible=true;layout.triangulationSerials={};layout.sceneView={};layout.sceneVisible=true;
        settings.origin=Settings::SavedOrigin{settings.rig->archive,"synthetic-origin",*originCandidate};
        for(int side=0;side<3;++side){auto id=cameras[side]->device.serial;cameras[side]->settings["PixelFormat"]=side<2?"Mono8":"BayerRG8";
            if(side==2)continue;settings.cameraNames[id]=side?"Mono B":"Mono A";views[side].visible=true;layout.cameraDisplay[id]={};
            auto& c=settings.cameras[id].calibration;c.distortion=(cv::Mat_<double>(1,5)<<-.16,.04,.002,-.001,0);
            std::vector<cv::Point3d> object{{pointSmokeExpected[0],pointSmokeExpected[1],pointSmokeExpected[2]}};std::vector<cv::Point2d> pixels;
            cv::Mat r=cv::Mat::eye(3,3,CV_64F),t=cv::Mat::zeros(3,1,CV_64F);if(side){r=settings.rig->calibration.fromReference[0].R;t=settings.rig->calibration.fromReference[0].T;}
            cv::projectPoints(object,r,t,c.K,c.distortion,pixels);pointSmokePixels[side]=pixels[0];
            auto& frame=videoSmokeFrames[side];frame.image=cv::Mat(c.imageSize,CV_8UC3,cv::Scalar(28,32,38));
            for(int x=0;x<c.imageSize.width;x+=100)cv::line(frame.image,{x,0},{x,c.imageSize.height},{45,48,55});
            for(int y=0;y<c.imageSize.height;y+=100)cv::line(frame.image,{0,y},{c.imageSize.width,y},{45,48,55});
            cv::circle(frame.image,pixels[0],20,{230,230,230},-1,cv::LINE_AA);cv::circle(frame.image,pixels[0],2,{30,30,30},-1);
        }
        videoSmokeOriginal=videoSmokeFrames[0].image.clone();
    }
    void triangulationInputSmoke(int frame){
        auto require=[](bool ok,const char* text){if(!ok)throw std::runtime_error(text);};
        for(auto& image:videoSmokeFrames)image.receivedAt=Clock::now();
        auto click=[&](int side,int mode){const auto& c=settings.cameras.at(layout.triangulationSerials[side]).calibration;
            auto p=stereoRawToPixel(c,pointSmokePixels[side],mode,mode==2?comparisonMaps.R[side]:cv::Mat(),mode==2?comparisonMaps.P[side]:cv::Mat());
            auto a=pointCanvas[side][0],b=pointCanvas[side][1];auto shape=mode==2?comparisonMaps.outputSize:c.imageSize;
            require(b.x>a.x&&b.y>a.y,"Point image hit area missing");auto& io=ImGui::GetIO();io.AddFocusEvent(true);
            io.AddMousePosEvent(a.x+float(p.x+.5)*(b.x-a.x)/shape.width,a.y+float(p.y+.5)*(b.y-a.y)/shape.height);io.AddMouseButtonEvent(0,true);};
        auto check=[&]{auto p=selectedPoint();require(p&&p->world.has_value(),"Point solve/world result missing");
            // ImGui floors mouse events to screen pixels; these small previews magnify that quantization in depth.
            if(cv::norm(p->result.cameraPoint-pointSmokeExpected)>=.01){std::ostringstream diagnostic;diagnostic<<"Clicked 3D point differs at smoke frame "<<frame<<": "<<p->result.cameraPoint;throw std::runtime_error(diagnostic.str());}
            require(sceneError.empty(),"Point scene failed to render");};
        if(frame==3){freezePointPair();require(pointFrozen&&layout.triangulationSerials[1]=="SYNTHETIC-B","Mono pair freeze/default selection failed");
            require(pointFrames[0].image.data!=videoSmokeFrames[0].image.data,"Frozen pair must own its pixels");}
        if(frame==6)click(0,0);if(frame==10)click(1,0);
        if(frame==14){check();videoSmokeFrames[0].image=videoSmokeFrames[0].image.clone();videoSmokeFrames[0].image.setTo(cv::Scalar(40,50,60));++videoSmokeFrames[0].generation;}
        if(frame==16){require(displayFrame(0).generation==1&&sourceFrame(0).generation==2,"Grab must advance while selected pair stays frozen");
            for(int side=0;side<2;++side)layout.cameraDisplay[layout.triangulationSerials[side]].mode=1;refreshPointViews();}
        if(frame==18)click(0,1);if(frame==22)click(1,1);
        if(frame==26){check();require(pointList.size()==2&&pointNextId==3&&!pointPixels[0]&&!pointPixels[1],"Pairs append automatically and reset the draft");}
        if(frame==28){views[0].visible=views[1].visible=false;layout.comparisonVisible=true;layout.comparisonMode=2;comparisonDirty=true;}
        if(frame==34)click(0,2);if(frame==38)click(1,2);
        if(frame==40){for(auto& frozen:pointFrames)frozen.receivedAt=Clock::now()-std::chrono::seconds(5);comparisonDirty=true;}
        if(frame==42){check();require(pointList.size()==3&&comparisonError.empty(),"Three points persist and frozen comparison does not expire");}
        if(frame==43){auto p=pointRowCenters.at(1);ImGui::GetIO().AddMousePosEvent(p.x,p.y);ImGui::GetIO().AddMouseButtonEvent(0,true);}
        if(frame==45){require(pointSelected==1,"Clicking a point list row selects its rays");editSelectedPoint();
            pickPointPixel(0,pointSmokePixels[0]+cv::Point2d(0,100),0);require(pointList.size()==3&&!pointError.empty(),"Bad edit preserves the existing valid point");
            pickPointPixel(0,pointSmokePixels[0],0);require(pointList.size()==3&&pointSelected==1&&pointEditing==-1,"Valid edit replaces a point without duplicating it");
            selectPoint(2);deleteSelectedPoint();require(pointList.size()==2&&pointList[1].id==3&&pointNextId==4,"Deleting a point preserves remaining IDs");}
        if(frame==46){resumePointLive();require(!pointFrozen&&pointList.size()==2&&displayFrame(0).generation==sourceFrame(0).generation,"Live must resume without losing measured points");
            cameras[0]->settings["Width"]="wrong";}
        if(frame==48){require(pointList.empty()&&!pointError.empty(),"Changed camera geometry must invalidate every point");cameras[0]->settings.erase("Width");freezePointPair();++sourceEpoch;}
        if(frame==50){require(!pointFrozen&&pointList.empty()&&!pointError.empty(),"Changed source must invalidate frozen pair");freezePointPair();pickPointPixel(0,pointSmokePixels[0],0);pickPointPixel(1,pointSmokePixels[1],0);check();layout.triangulationVisible=false;}
        if(frame==53){require(!pointFrozen&&pointList.size()==1,"Closing point panel must resume images and preserve results");layout.triangulationVisible=true;
            videoSmokeFrames[0].image=videoSmokeOriginal.clone();freezePointPair();pickPointPixel(0,pointSmokePixels[0],0);pickPointPixel(1,pointSmokePixels[1],0);
            for(auto object:std::vector<cv::Point3d>{{.02,.06,.55},{.10,-.03,.52}}){
                for(int side=0;side<2;++side){const auto& c=settings.cameras.at(layout.triangulationSerials[side]).calibration;std::vector<cv::Point2d> pixels;
                    auto relation=savedStereoTransform(settings,layout.triangulationSerials[0],layout.triangulationSerials[1]);
                    cv::projectPoints(std::vector<cv::Point3d>{object},side?relation.R:cv::Mat(cv::Mat::eye(3,3,CV_64F)),side?relation.T:cv::Mat(cv::Mat::zeros(3,1,CV_64F)),c.K,c.distortion,pixels);
                    pickPointPixel(side,pixels[0]+cv::Point2d(0,side?2.:0),0);}}
            require(pointList.size()==3,"New pair supports multiple distinct points");
            views[0].visible=views[1].visible=false;layout.sceneView.aimAt(pointList.front().world->position);}
        if(frame==56){auto p=pointRowCenters.at(1);ImGui::GetIO().AddMousePosEvent(p.x,p.y);ImGui::GetIO().AddMouseButtonEvent(0,true);}
        if(frame==58){check();require(pointSelected==1&&pointSceneMeasurements().size()==3,"List selection retains all world markers");
            // Give the selected pair a small skew so the shortest segment is nonzero in the preview.
            editSelectedPoint();pickPointPixel(1,pointSmokePixels[1]+cv::Point2d(0,3),0);require(pointList.size()==3&&selectedPoint()->result.rayGapM>0,"Selected ray gap retained in mm");}
        if(frame==64){check();clearPoint();require(pointList.empty()&&pointSelected==-1&&!pointFrozen,"Clear All removes measurements and rays");
            std::cout<<"PASS: multi-point raw/undistorted/rectified clicks, list selection, edit/delete, mm gap, midpoint/rays, continuing Grab and stale-source/calibration invalidation\n";}
        if(frame==7||frame==11||frame==19||frame==23||frame==35||frame==39||frame==44||frame==57)ImGui::GetIO().AddMouseButtonEvent(0,false);
    }
    bool processingSmokeActive=false;int processingSmokeStep=0,processingRoiTick=0;uint64_t processingSmokeGeneration=0;cv::Mat processingSmokeOriginal;
    void prepareProcessingSmoke(){
        if(!smoke)throw std::logic_error("Synthetic setup requires smoke mode.");
        processingSmokeActive=true;settings=Settings{};settings.cameraNames["LIVE-ONLY"]="Preserved live profile";
        settings.ball.run=true;Settings snapshot;
        std::vector<RecordedCamera> descriptions;
        Calibration intrinsic;intrinsic.imageSize={640,480};intrinsic.K=(cv::Mat_<double>(3,3)<<700,0,320,0,700,240,0,0,1);intrinsic.distortion=(cv::Mat_<double>(1,5)<<-.08,.01,0,0,0);
        Settings::SavedRig rig;rig.archive="test-rig";
        for(int i=0;i<3;++i){auto id="REC-"+std::to_string(i);descriptions.push_back({id,"Synthetic",i==1?"Color":"Mono",{},{{"PixelFormat","BGR8"}}});
            SavedCamera c;c.identity=id;c.sourceKind="live";c.archive="intrinsic-"+id;c.calibration=intrinsic;snapshot.cameras[id]=c;snapshot.cameraNames[id]="Recorded "+std::to_string(i+1);
            rig.serials.push_back(id);rig.intrinsicArchives.push_back(c.archive);if(i){StereoCalibration edge;edge.R=cv::Mat::eye(3,3,CV_64F);edge.T=(cv::Mat_<double>(3,1)<<-.10*i,.002*i,0);rig.calibration.fromReference.push_back(edge);}
            auto& o=settings.ball.cameras[id];o.color=i==1;o.scale=1;o.minRadius=20;o.maxRadius=38;o.houghVotes=18;o.minCoverage=.4;}
        snapshot.rig=rig;fs::create_directories(data);auto folder=data/("processing-"+std::to_string(Clock::now().time_since_epoch().count()));
        {RawRecorder capture(folder,descriptions,snapshot,256ull*1024*1024,{true,0});
            for(int n=0;n<40;++n)for(int i=0;i<3;++i){cv::Mat pixels(intrinsic.imageSize,CV_8UC3,cv::Scalar(25,25,25));
                cv::circle(pixels,{170+n*3-i*20,220},29,i==1?cv::Scalar(40,40,245):cv::Scalar(235,235,235),-1,cv::LINE_AA);
                RawFrameInfo info{640,480,0,0,0x02180015,0,uint64_t(100+n),uint64_t(9000+n*4000),uint64_t(1000000000+n*4000000+i*100000),pixels.total()*3};
                capture.submit(i,info,{pixels.data,pixels.total()*3});}
            capture.stop();}
        showDevices=showCalibration=showStereo=showOrigin=layout.sceneVisible=false;showProcessing=showRecording=true;
        layout.comparisonVisible=false;layout.cameraDisplay.clear();requestReplay(folder);
    }
    bool processingInputSmoke(){
        auto require=[](bool value,const char* text){if(!value)throw std::runtime_error(text);};
        if(!recordingMessage.empty()&&recordingMessage!="Loading...")throw std::runtime_error(recordingMessage);
        if(processingSmokeStep==0&&replay&&replayTimes.size()==3&&replayProcessed()){
            require(cameras.size()==3&&liveSettings&&liveSettings->cameraNames.at("LIVE-ONLY")=="Preserved live profile","Replay isolates live settings");
            for(size_t i=0;i<3;++i){auto job=displayedBall(int(i));require(job&&!job->detection.balls.empty(),"Replay uses same circle detector for all cameras");}
            require(displayedBall(1)->detection.balls[0].color=="Red","Replay color classification");
            processingSmokeOriginal=sourceFrame(0).image.clone();settings.ball.overlay=false;require(!displayedBall(0),"Global overlay toggle");settings.ball.overlay=true;
            ballOptions(0).stage=3;markBallChanged();processingSmokeStep=1;
        }else if(processingSmokeStep==1&&replayProcessed()&&ballRuntime.at("REC-0")->options.stage==3){
            auto frame=displayFrame(0);require(cv::norm(ballStage(0,frame),frame.image,cv::NORM_INF)>0,"Processing stage appears on same source frame");
            require(cv::norm(sourceFrame(0).image,processingSmokeOriginal,cv::NORM_INF)==0,"Processing preserves replay raw source");
            ballOptions(0).stage=0;layout.cameraDisplay["REC-0"].mode=1;markBallChanged();seekReplay(17);processingSmokeStep=2;
        }else if(processingSmokeStep==2&&replayFrameIndex==17&&replayProcessed()){
            require(sourceFrame(0).id==117&&sourceFrame(1).id==117&&sourceFrame(2).id==117,"Replay frame stepping pairs all cameras by recorded time");
            require(views[0].error.empty()&&!views[0].mapX.empty(),"Replay supports saved undistortion");
            layout.comparisonSerials={"REC-0","REC-2"};layout.comparisonMode=2;layout.comparisonVisible=true;
            settings.ball.run=false;markBallChanged();processingSmokeStep=3;
        }else if(processingSmokeStep==3){
            require(!ballRuntime.at("REC-0")->result,"Run off clears processed frames");settings.ball.run=true;markBallChanged();processingSmokeStep=4;
        }else if(processingSmokeStep==4&&replayProcessed()){
            require(comparisonError.empty()&&comparisonGuideError.empty(),"Replay rectified pair and epipolar guides");
            require(displayedBall(0),"Run resumes on paused frame");replayPlaying=true;replayNext=Clock::now();processingSmokeStep=5;
        }else if(processingSmokeStep==5&&replayFrameIndex>=20&&replayProcessed()){
            replayPlaying=false;replayDirty=false;processingSmokeGeneration=replayGeneration;processingSmokeStep=6;
        }else if(processingSmokeStep==6&&!replayTask.pending()){
            processingSmokeGeneration=replayGeneration;processingSmokeStep=7;
        }else if(processingSmokeStep==7&&replayProcessed()){
            require(replayGeneration==processingSmokeGeneration,"Pause retains the source frame");
            auto frame=displayFrame(0);require(displayedBall(0)->frame.image.data==frame.image.data,"Overlay corresponds to the displayed frame");
            layout.triangulationSerials={"REC-0","REC-2"};freezePointPair();
            require(pointFrozen&&!replayPlaying&&std::abs(pointSkewMs-.2)<1e-6,"Replay point capture uses recorded timing and pauses playback");
            std::vector<cv::Point3d> object{{.03,.02,1.}};
            for(int side=0;side<2;++side){const auto& c=settings.cameras.at(layout.triangulationSerials[side]).calibration;std::vector<cv::Point2d> pixels;
                cv::Mat r=cv::Mat::eye(3,3,CV_64F),t=cv::Mat::zeros(3,1,CV_64F);if(side){auto relation=savedStereoTransform(settings,"REC-0","REC-2");r=relation.R;t=relation.T;}
                cv::projectPoints(object,r,t,c.K,c.distortion,pixels);pickPointPixel(side,pixels[0],0);}
            require(pointList.size()==1&&!pointList[0].world&&cv::norm(pointList[0].result.cameraPoint-cv::Vec3d(.03,.02,1))<1e-8,"Replay triangulates in camera frame without inventing a world origin");
            resumePointLive();layout.triangulationVisible=false;
            ballOptions(0).brightness=22;markBallChanged();saveBallOptions();
            require(liveSettings->ball.cameras.at("REC-0").brightness==22&&!liveSettings->cameras.contains("REC-0"),"Processing edits preserve live calibration profile");
            layout.comparisonVisible=false;roiCamera=0;roiFrame=sourceFrame(0);roiPoints.clear();layout.cameraDisplay["REC-0"].mode=0;views[0].generation=0;processingSmokeStep=8;
        }else if(processingSmokeStep==8&&replayProcessed()){
            auto& io=ImGui::GetIO();std::array<cv::Point2f,4> corners{{{.1f,.1f},{.9f,.1f},{.9f,.9f},{.1f,.9f}}};
            if(processingRoiTick<8){auto p=corners[processingRoiTick/2];io.AddFocusEvent(true);
                io.AddMousePosEvent(roiCanvas[0].x+p.x*(roiCanvas[1].x-roiCanvas[0].x),roiCanvas[0].y+p.y*(roiCanvas[1].y-roiCanvas[0].y));io.AddMouseButtonEvent(0,processingRoiTick%2==0);++processingRoiTick;}
            else {require(roiCamera==-1&&ballOptions(0).roi.size()==4,"Four video clicks commit ROI");for(int i=0;i<4;++i)require(cv::norm(ballOptions(0).roi[i]-corners[i])<.01,"ROI screen clicks retain native normalized coordinates");
                replayReference=1;seekReplay(9);processingSmokeStep=9;}
        }else if(processingSmokeStep==9&&replayFrameIndex==9&&replayProcessed()){
            require(pointList.empty()&&!pointFrozen,"Replay seek clears previous-frame triangulation");
            require(sourceFrame(0).id==109&&sourceFrame(1).id==109,"Changing timeline reference preserves paired replay");
            returnLive();require(!replay&&settings.cameraNames.contains("LIVE-ONLY")&&!settings.cameras.contains("REC-0"),"Return Live restores profile");
            std::cout<<"PASS: offline 3-camera source, detector, color, stage, undistort, rectification, triangulation, seek, play/pause, ROI clicks, timeline reference, restart on paused frame, raw preservation and live profile isolation\n";return true;
        }
        return false;
    }
private:
    GLFWwindow* mainWindow;
    UiLayout layout;
    bool restoredLayout=false,canSaveLayout=true;
    Clock::time_point lastLayoutSave=Clock::now();
    std::string lastLayoutContent;
    bool connectedSerial(const std::string& serial)const{return std::any_of(cameras.begin(),cameras.end(),[&](const auto& c){return c->device.serial==serial;});}
    void rememberCameraWindows(){for(size_t i=0;i<cameras.size();++i)layout.cameraWindows[cameras[i]->device.serial]=views[i].visible;}
    fs::path data,profile;
    Settings settings;BoardConfig board;bool confirmed=false,loadFailed=false;
    std::vector<Device> devices;std::vector<bool> selectedDevices;
    std::vector<std::unique_ptr<Camera>> cameras;std::vector<ViewState> views;
    bool showDevices=true,showCalibration=false,initialGuide=false;
    bool offline=false,undistorted=false;char offlineId[128]="offline-camera-A";
    int cameraIndex=0,selectedSample=-1;
    std::string identity,message="카메라를 연결하세요.";
    Observation detected;std::vector<Observation> samples;std::optional<Calibration> result;
    BackgroundTask<DetectionResult> detectTask;BackgroundTask<Calibration> calTask;BackgroundTask<ImportResult> importTask;
    BackgroundTask<ReconnectResult> reconnectTask;
    std::future<Settings> saveFuture;
    bool detectionEnabled=false;
    AutoCapture autoCapture;AutoCaptureConfig autoOptions;
    bool newDetection=false;
    std::string brightnessCamera;
    Clock::time_point brightnessRequestedAt{};
    std::vector<Texture> thumbnails;
    Clock::time_point lastSampleAdded{};
    uint64_t lastGeneration=0;
    std::map<std::string,std::string> sessionGeometry;
    Clock::time_point detectedAt=Clock::now();
    bool monoSmokeLive=false;CameraFrame monoSmokeFrame;std::vector<Observation> monoSmokeSamples;cv::Mat monoSmokeOriginal;
    Texture preview,correctedPreview;bool previewDirty=true,exported=false;
    bool monoWorkflow=false;uint64_t monoPreviewGeneration=0;
    Clock::time_point monoPreviewUpdated{};
    int pendingMonoCamera=-1;
    bool previewBrightnessActive=false;
    bool showStereo=false,stereoConfirmed=false,stereoDetectionEnabled=false,stereoExported=false,savingStereo=false;
    int stereoA=0,stereoB=1,stereoC=2,stereoCount=3,stereoSelected=-1;
    BoardConfig stereoBoard;
    BackgroundTask<StereoDetection> stereoDetectTask;
    BackgroundTask<RigCalibration> stereoCalTask;
    std::optional<RigCalibration> stereoResult;
    StereoDetection stereoDetected;
    std::vector<RigSample> stereoSamples;
    std::array<Texture,3> stereoPreview;
    std::vector<std::array<Texture,3>> stereoThumbnails;
    RigCapture stereoCapture;AutoCaptureConfig stereoAutoOptions{20,1.0,1.5};
    std::array<uint64_t,3> stereoGenerations{};
    std::array<uint64_t,3> stereoPreviewGenerations{};
    std::vector<std::string> stereoSessionSerials,stereoSessionArchives;
    bool stereoWorkflow=false,stereoSmokeLive=false;
    std::array<CameraFrame,3> stereoSmokeFrames;
    std::vector<RigSample> stereoSmokeSamples;
    cv::Mat stereoSmokeOriginal;
    bool stereoPreviewDirty=true,stereoPreviewEnhanced=false;
    std::string stereoMessage="각 카메라를 보정한 뒤 시작하세요.";
    std::string stereoWaitReason;
     #include "origin_panel.inl"
     #include "scene_panel.inl"
     #include "processing_panel.inl"
     #include "triangulation_panel.inl"
    void openCalibration(){
        cancelOrigin();showOrigin=false;cancelStereo();showStereo=false;showCalibration=true;
        if(samples.empty()){
            board=settings.board;confirmed=settings.boardConfirmed;
            for(int i=0;i<int(cameras.size());++i)if(cameras[i]->running()){cameraIndex=i;break;}
            identity=cameras.empty()?"":cameras[cameraIndex]->device.serial;previewDirty=true;
        }
    }
    void openStereo(){cancelOrigin();showOrigin=false;cancelWork();showCalibration=false;showStereo=true;if(stereoSamples.empty()){stereoBoard=settings.board;stereoConfirmed=settings.boardConfirmed;selectStereoDefaults();}stereoPreviewDirty=true;}
    bool calibrationBrightnessActive()const{return showCalibration&&detectionEnabled&&!calTask.pending()&&!result&&!exported;}
    BrightnessOptions savedBrightness(const std::string& serial)const{
        auto it=settings.softwareBrightness.find(serial);
        return it==settings.softwareBrightness.end()?BrightnessOptions{}:it->second;
    }
    BrightnessOptions liveBrightness(const std::string& serial)const{
        if(stereoBrightnessActive()&&stereoIndicesValid())for(int i:stereoIndices())if(serial==cameras[i]->device.serial)return savedBrightness(serial);
        if(!calibrationBrightnessActive()||offline||cameras.empty()||cameras[cameraIndex]->device.serial!=serial)return {};
        return savedBrightness(serial);
    }
    bool busy()const{return originCapture.running()||originPoseTask.active()||calTask.active()||importTask.active()||stereoCalTask.active()||saveFuture.valid();}
    void discardDetection(){brightnessCamera.clear();autoCapture.stop();newDetection=false;detectTask.cancel();detected={};lastGeneration=0;previewDirty=true;}
    void cancelWork(){monoWorkflow=false;detectionEnabled=false;discardDetection();calTask.cancel();importTask.cancel();message="취소됨";}
    void fail(const std::exception& e){message=e.what();}
    void invalidate(){result.reset();exported=false;undistorted=false;previewDirty=true;}
    void resetSession(){monoWorkflow=false;detectionEnabled=false;discardDetection();releaseThumbnails();samples.clear();selectedSample=-1;sessionGeometry.clear();monoPreviewGeneration=0;invalidate();}
    void stopCameras(){cancelOrigin();reconnectTask.cancel();cancelStereo();detectionEnabled=false;discardDetection();for(auto& c:cameras)c->stop();message="수집 중지";}
    std::string deviceError;
    void updateDeviceList(std::vector<Device> found){
        std::map<std::string,bool> checked;
        for(size_t i=0;i<devices.size();++i)checked[devices[i].serial]=selectedDevices[i];
        auto add=[&](Device d){if(std::none_of(found.begin(),found.end(),[&](const auto& v){return v.serial==d.serial;}))found.push_back(std::move(d));};
        for(const auto& c:cameras){auto d=c->device;if(!c->connected())d.key.clear();add(std::move(d));}
        for(const auto& id:settings.selectedSerials){auto it=settings.cameras.find(id);add(Device{"",id,it==settings.cameras.end()?"":it->second.model});}
        devices=std::move(found);selectedDevices.assign(devices.size(),false);
        for(size_t i=0;i<devices.size();++i){auto it=checked.find(devices[i].serial);
            selectedDevices[i]=it!=checked.end()?it->second:std::find(settings.selectedSerials.begin(),settings.selectedSerials.end(),devices[i].serial)!=settings.selectedSerials.end();}
    }
    bool deviceSelected(const std::string& id)const{
        for(size_t i=0;i<devices.size();++i)if(devices[i].serial==id)return selectedDevices[i];return false;
    }
    int deviceCamera(const std::string& id)const{
        for(size_t i=0;i<cameras.size();++i)if(cameras[i]->device.serial==id)return int(i);return -1;
    }
    void scan(){try{deviceError.clear();updateDeviceList(Camera::enumerate());}catch(const std::exception& e){deviceError=e.what();}}
    void beginReconnect(){
        auto serials=settings.selectedSerials;
        reconnectTask.start([serials](std::stop_token stop){
            ReconnectResult r;r.devices=Camera::enumerate();
            for(const auto& serial:serials){
                if(stop.stop_requested()||r.cameras.size()>=3)break;
                if(std::any_of(r.cameras.begin(),r.cameras.end(),[&](const auto& c){return c->device.serial==serial;}))continue;
                auto it=std::find_if(r.devices.begin(),r.devices.end(),[&](const auto& d){return d.serial==serial;});
                if(it==r.devices.end()){r.warnings+=serial+": 장치 없음. ";continue;}
                try{auto c=std::make_unique<Camera>(*it);c->open();r.cameras.push_back(std::move(c));}
                catch(const std::exception& e){r.warnings+=serial+": "+e.what()+" ";}
            }
            // Arm external-trigger slaves before starting a free-running master.
            for(bool triggered:{true,false})for(auto& c:r.cameras){
                if(stop.stop_requested())return r;
                if((c->settings["TriggerMode"]=="On")==triggered)try{c->start();}
                catch(const std::exception& e){r.warnings+=c->device.serial+": "+e.what()+" ";}
            }
            return r;
        });message="자동 연결 중...";
    }
    void rememberConnections(){
        if(loadFailed||smoke)return;
        Settings next=settings;next.selectedSerials.clear();
        for(const auto& c:cameras)if(c->connected())next.selectedSerials.push_back(c->device.serial);
        writeSettings(profile,next);settings=std::move(next);
    }
    void connectSelected(){
        if(reconnectTask.pending()||saveFuture.valid())return;
        try{
            deviceError.clear();size_t count=0,requested=0;
            for(const auto& c:cameras)if(c->connected())++count;
            for(size_t i=0;i<devices.size();++i)if(selectedDevices[i]){int index=deviceCamera(devices[i].serial);
                if(!devices[i].key.empty()&&(index<0||!cameras[index]->connected()))++requested;}
            if(count+requested>3)throw std::runtime_error("카메라는 최대 3대까지 연결할 수 있습니다.");
            for(size_t i=0;i<devices.size();++i)if(selectedDevices[i]){
                int index=deviceCamera(devices[i].serial);if(index>=0&&cameras[index]->connected())continue;
                try{
                    if(devices[i].key.empty())throw std::runtime_error("장치를 찾지 못했습니다. Scan을 눌러 다시 검색하세요.");
                    if(index<0){auto c=std::make_unique<Camera>(devices[i]);c->open();cameras.push_back(std::move(c));views.emplace_back();index=int(cameras.size())-1;
                        if(auto it=layout.cameraWindows.find(devices[i].serial);it!=layout.cameraWindows.end())views[index].visible=it->second;}
                    else {cameras[index]->device=devices[i];cameras[index]->open();}
                    views[index].generation=0;
                }catch(const std::exception& e){deviceError+=devices[i].serial+": "+e.what()+"\n";}
            }
            if(samples.empty()&&stereoSamples.empty())selectStereoDefaults();
            rememberConnections();
        }catch(const std::exception& e){deviceError=e.what();}
    }
    void controlSelectedDevices(int action){ // 0 Grab, 1 Stop, 2 Disconnect.
        if(reconnectTask.pending()||saveFuture.valid())return;
        deviceError.clear();cancelOrigin();cancelStereo();cancelWork();
        if(action==0){
            for(bool triggered:{true,false})for(size_t i=0;i<cameras.size();++i){auto& c=cameras[i];
                if(!deviceSelected(c->device.serial)||!c->connected()||c->running()||(c->settings["TriggerMode"]=="On")!=triggered)continue;
                try{c->start();views[i].generation=0;}catch(const std::exception& e){deviceError+=c->device.serial+": "+e.what()+"\n";}
            }
        }else for(size_t i=0;i<cameras.size();++i){auto& c=cameras[i];if(!deviceSelected(c->device.serial)||!c->connected())continue;
            if(action==1)c->stop();else{c->close();views[i].texture.release();views[i].generation=0;}
        }
        if(action==2)try{rememberConnections();}catch(const std::exception& e){deviceError=e.what();}
    }
    void startCameras(){
        try{
            deviceError.clear();
            cancelOrigin();
            discardDetection();
            cancelStereo();
            for(auto& v:views)v.generation=0;
            for(auto& c:cameras)if(c->connected()&&c->settings["TriggerMode"]=="On")c->start();
            for(auto& c:cameras)if(c->connected()&&c->settings["TriggerMode"]!="On")c->start();
            message="수집 중";
        }catch(const std::exception& e){for(auto& c:cameras)c->stop();deviceError=e.what();fail(e);}
    }
    std::map<std::string,std::array<char,128>> cameraNameEdits;
    std::string cameraName(const std::string& id,const std::string& fallback={})const{
        if(auto it=settings.cameraNames.find(id);it!=settings.cameraNames.end()&&!it->second.empty())return it->second;
        if(settings.rig){auto it=std::find(settings.rig->serials.begin(),settings.rig->serials.end(),id);
            if(it!=settings.rig->serials.end())return "Camera "+std::string(1,char('A'+(it-settings.rig->serials.begin())));}
        return fallback.empty()?id:fallback;
    }
    void saveCameraName(const std::string& id,const char* value){
        try{std::string name=value;auto first=name.find_first_not_of(" \t\r\n");
            name=first==std::string::npos?std::string{}:name.substr(first,name.find_last_not_of(" \t\r\n")-first+1);
            Settings next=settings;if(name.empty())next.cameraNames.erase(id);else next.cameraNames[id]=name;
            if(!smoke)writeSettings(profile,next);settings=std::move(next);
        }catch(const std::exception& e){deviceError=e.what();}
    }
    RecordState deviceCalState(const std::string& id)const{
        int i=deviceCamera(id);if(i<0||!cameras[i]->connected())return calibrationState(settings,id);
        auto live=geometry(*cameras[i]);return calibrationState(settings,id,&live);
    }
    SavedPoseView devicePose(const std::string& id)const{
        auto pose=savedPoseView(settings,id);
        if(pose.state==RecordState::Saved)for(const auto& member:pose.members)if(deviceCalState(member)!=RecordState::Saved){pose.state=RecordState::Stale;break;}
        return pose;
    }
    SavedPoseView deviceWorldPose(const std::string& id)const{
        auto pose=savedWorldPoseView(settings,id);
        if(pose.state==RecordState::Saved)for(const auto& member:pose.members)if(deviceCalState(member)!=RecordState::Saved){
            pose.state=RecordState::Stale;pose.center.release();pose.rotation.release();pose.rms.reset();break;}
        return pose;
    }
    ImVec2 deviceCalPoint,devicePosePoint,deviceWorldPoint,deviceClosePoint;bool deviceCalSeen=false,devicePoseSeen=false,deviceWorldSeen=false;
    bool recordIcon(const char* id,RecordState state){
        ImVec2 p=ImGui::GetCursorScreenPos();float size=ImGui::GetFrameHeight();
        bool click=ImGui::InvisibleButton(id,{28,size},ImGuiButtonFlags_EnableNav);
        auto draw=ImGui::GetWindowDrawList();ImVec2 center(p.x+14,p.y+size*.5f);
        if(ImGui::IsItemHovered())draw->AddCircleFilled(center,12,IM_COL32(65,85,105,160));
        if(state==RecordState::Saved)draw->AddCircleFilled(center,5,IM_COL32(75,210,145,255));
        else if(state==RecordState::Missing)draw->AddCircle(center,5,IM_COL32(135,150,165,255),0,1.5f);
        else {draw->AddCircleFilled(center,8,IM_COL32(240,177,65,255));draw->AddText({center.x-3,center.y-10},IM_COL32(35,32,25,255),"!");}
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",state==RecordState::Saved?"Saved":state==RecordState::Missing?"Not saved":"Recalibration required");
        return click;
    }
    void drawCalPopup(const std::string& id){
        if(!ImGui::BeginPopup("cal-values"))return;
        deviceCalSeen=true;
        ImGui::Text("Cal · %s",id.c_str());ImGui::Separator();
        auto it=settings.cameras.find(id);
        if(it==settings.cameras.end())ImGui::TextDisabled("저장된 값 없음");
        else {const auto& c=it->second.calibration;
            if(deviceCalState(id)==RecordState::Stale)ImGui::TextColored({1,.7f,.3f,1},"현재 영상 조건과 다릅니다.");
            ImGui::Text("%d × %d   RMS %.4f px",c.imageSize.width,c.imageSize.height,c.rms);
            ImGui::SeparatorText("K");for(int r=0;r<3;++r)ImGui::Text("%10.4f  %10.4f  %10.4f",c.K.at<double>(r,0),c.K.at<double>(r,1),c.K.at<double>(r,2));
            ImGui::SeparatorText("D");auto d=c.distortion.reshape(1,1);for(int i=0;i<d.cols;++i)ImGui::Text("D%d  % .7f",i,d.at<double>(0,i));
        }
        if(ImGui::Button("Close"))ImGui::CloseCurrentPopup();
        {auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();deviceClosePoint={(a.x+b.x)*.5f,(a.y+b.y)*.5f};}ImGui::EndPopup();
    }
    void drawPosePopup(const std::string& id,bool world=false){
        if(!ImGui::BeginPopup(world?"world-values":"pose-values"))return;
        if(world)deviceWorldSeen=true;else devicePoseSeen=true;
        auto pose=world?deviceWorldPose(id):devicePose(id);ImGui::Text("%s · %s",world?"World":"Pose",id.c_str());ImGui::Separator();
        if(pose.state==RecordState::Missing)ImGui::TextDisabled("저장된 값 없음");
        else if(world&&pose.state==RecordState::Stale)ImGui::TextColored({1,.7f,.3f,1},"원점 재설정 필요");
        else {
            if(pose.state==RecordState::Stale)ImGui::TextColored({1,.7f,.3f,1},"보정값 변경 · 재보정 필요");
            ImGui::Text("Reference  %s",pose.reference.c_str());
            ImGui::Text("Position (mm)\nX %.3f   Y %.3f   Z %.3f",pose.center.at<double>(0)*1000,pose.center.at<double>(1)*1000,pose.center.at<double>(2)*1000);
            cv::Mat rv;cv::Rodrigues(pose.rotation,rv);rv*=180/CV_PI;
            ImGui::Text("Rotation vector (°)\nX %.3f   Y %.3f   Z %.3f",rv.at<double>(0),rv.at<double>(1),rv.at<double>(2));
            if(pose.rms)ImGui::Text("%s %.4f px",world?"Origin RMS":"RMS",*pose.rms);
            if(ImGui::TreeNode(world?"R · Camera → World":"R · Camera → Reference")){
                for(int r=0;r<3;++r)ImGui::Text("% .6f  % .6f  % .6f",pose.rotation.at<double>(r,0),pose.rotation.at<double>(r,1),pose.rotation.at<double>(r,2));ImGui::TreePop();}
        }
        if(ImGui::Button("Close"))ImGui::CloseCurrentPopup();
        {auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();deviceClosePoint={(a.x+b.x)*.5f,(a.y+b.y)*.5f};}ImGui::EndPopup();
    }
    void drawDevices(){
        ImGui::SetNextWindowSize({440,450},ImGuiCond_FirstUseEver);
        if(ImGui::Begin("장치 / 설정",&showDevices)){
            ImGui::BeginDisabled(reconnectTask.pending()||smoke||replay||sourceBusy());if(ImGui::Button("Scan"))scan();ImGui::EndDisabled();
            if(reconnectTask.active()){ImGui::SameLine();ImGui::TextDisabled("Connecting...");ImGui::SameLine();if(ImGui::Button("Cancel"))reconnectTask.cancel();}
            bool canConnect=false,canGrab=false,canStop=false,canDisconnect=false;size_t requested=0;
            if(ImGui::BeginTable("device-list",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_SizingStretchProp)){
                ImGui::TableSetupColumn("Camera",ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Status",ImGuiTableColumnFlags_WidthFixed,90);
                ImGui::TableSetupColumn("Cal",ImGuiTableColumnFlags_WidthFixed,30);
                ImGui::TableSetupColumn("Pose",ImGuiTableColumnFlags_WidthFixed,36);
                ImGui::TableSetupColumn("World",ImGuiTableColumnFlags_WidthFixed,46);ImGui::TableHeadersRow();
                for(size_t i=0;i<devices.size();++i){const auto& d=devices[i];int index=deviceCamera(d.serial);Camera* c=index<0?nullptr:cameras[index].get();
                    bool connected=c&&c->connected(),running=connected&&c->running();
                    ImGui::PushID(d.serial.c_str());ImGui::TableNextRow();ImGui::TableNextColumn();
                    std::string name=cameraName(d.serial,"Camera "+std::to_string(i+1));
                    bool selected=selectedDevices[i];ImGui::BeginDisabled(reconnectTask.pending());
                    if(ImGui::Checkbox("##selected",&selected))selectedDevices[i]=selected;ImGui::EndDisabled();ImGui::SameLine();
                    auto [edit,inserted]=cameraNameEdits.try_emplace(d.serial);if(inserted)snprintf(edit->second.data(),edit->second.size(),"%s",name.c_str());
                    ImGui::SetNextItemWidth(-1);ImGui::BeginDisabled(loadFailed||saveFuture.valid()||replay||sourceBusy());
                    bool enter=ImGui::InputText("##name",edit->second.data(),edit->second.size(),ImGuiInputTextFlags_EnterReturnsTrue);
                    if((enter||ImGui::IsItemDeactivatedAfterEdit())&&name!=edit->second.data())saveCameraName(d.serial,edit->second.data());ImGui::EndDisabled();
                    if(ImGui::IsItemHovered())ImGui::SetTooltip("이름 변경\n%s\n%s",d.serial.c_str(),d.model.c_str());
                    auto shortSerial=d.serial.size()>10?"..."+d.serial.substr(d.serial.size()-10):d.serial;
                    ImGui::TextDisabled("%s",shortSerial.c_str());
                    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s\n%s",d.serial.c_str(),d.model.c_str());
                    ImGui::TableNextColumn();auto error=connected?c->statistics().error:std::string{};
                    ImGui::TextColored(!error.empty()?ImVec4(1,.55f,.35f,1):running?ImVec4(.3f,.85f,.6f,1):ImVec4(.65f,.7f,.75f,1),
                        !error.empty()?"Error":replay?"Playback":running?"Grabbing":connected?"Connected":d.key.empty()?"Offline":"Ready");
                    if(!error.empty()&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",error.c_str());
                    ImGui::TableNextColumn();if(i==0){auto p=ImGui::GetCursorScreenPos();deviceCalPoint={p.x+14,p.y+ImGui::GetFrameHeight()*.5f};}
                    if(recordIcon("cal",deviceCalState(d.serial)))ImGui::OpenPopup("cal-values");drawCalPopup(d.serial);
                    ImGui::TableNextColumn();if(i==0){auto p=ImGui::GetCursorScreenPos();devicePosePoint={p.x+14,p.y+ImGui::GetFrameHeight()*.5f};}
                    if(recordIcon("pose",devicePose(d.serial).state))ImGui::OpenPopup("pose-values");drawPosePopup(d.serial);
                    ImGui::TableNextColumn();if(i==0){auto p=ImGui::GetCursorScreenPos();deviceWorldPoint={p.x+14,p.y+ImGui::GetFrameHeight()*.5f};}
                    if(recordIcon("world",deviceWorldPose(d.serial).state))ImGui::OpenPopup("world-values");drawPosePopup(d.serial,true);
                    if(selected){if(!connected&&!d.key.empty())++requested;canConnect|=!connected&&!d.key.empty();canGrab|=connected&&!running;canStop|=running;canDisconnect|=connected;}
                    ImGui::PopID();
                }ImGui::EndTable();
            }
            if(devices.empty())ImGui::TextDisabled("No cameras");
            bool limitExceeded=requested+std::count_if(cameras.begin(),cameras.end(),[](const auto& c){return c->connected();})>3;
            canConnect&=!limitExceeded;
            float width=std::max(60.f,(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x)*.5f);
            ImGui::BeginDisabled(reconnectTask.pending()||saveFuture.valid()||smoke||replay||sourceBusy());
            ImGui::BeginDisabled(!canConnect);if(ImGui::Button("Connect",{width,32}))connectSelected();ImGui::EndDisabled();
            if(limitExceeded&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("최대 3대까지 연결할 수 있습니다.");ImGui::SameLine();
            ImGui::BeginDisabled(!canDisconnect);if(ImGui::Button("Disconnect",{width,32}))controlSelectedDevices(2);ImGui::EndDisabled();
            ImGui::BeginDisabled(!canGrab);if(ImGui::Button("Grab",{width,32}))controlSelectedDevices(0);ImGui::EndDisabled();ImGui::SameLine();
            ImGui::BeginDisabled(!canStop);if(ImGui::Button("Stop",{width,32}))controlSelectedDevices(1);ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::BeginDisabled(loadFailed||saveFuture.valid()||smoke||replay||sourceBusy());bool autoConnect=settings.autoReconnect;
            if(ImGui::Checkbox("Auto Connect",&autoConnect))try{Settings next=settings;next.autoReconnect=autoConnect;writeSettings(profile,next);settings=std::move(next);}
                catch(const std::exception& e){deviceError=e.what();}ImGui::EndDisabled();
            if(!deviceError.empty())ImGui::TextWrapped("%s",deviceError.c_str());
            if(!layoutWarning.empty())ImGui::TextWrapped("%s",layoutWarning.c_str());
        }ImGui::End();
    }
    bool compatible(const Camera& c)const{
        auto it=settings.cameras.find(c.device.serial);if(it==settings.cameras.end())return false;
        return it->second.sourceKind=="live"&&it->second.geometry==geometry(c);
    }
    static std::map<std::string,std::string> geometry(const Camera& c){
        std::map<std::string,std::string> out;
        for(auto& [k,v]:c.settings)if(k=="Width"||k=="Height"||k=="OffsetX"||k=="OffsetY"||k.starts_with("Binning")||k.starts_with("Decimation")||k.starts_with("Reverse"))out[k]=v;
        return out;
    }
    bool videoSmoke=false;
    cv::Mat videoSmokeOriginal;
    std::array<CameraFrame,3> videoSmokeFrames;
    std::array<Texture,2> comparisonTextures;
    std::array<uint64_t,2> comparisonGenerations{};
    std::array<Clock::time_point,2> comparisonReceivedAt{};
    std::array<cv::Size,2> comparisonImageSizes{};
    std::array<CameraDisplayOptions,2> comparisonDisplay;
    std::array<BrightnessOptions,2> comparisonBrightness;
    StereoRectification comparisonMaps;
    std::string comparisonKey,comparisonError,comparisonGeometryKey,comparisonGuideKey,comparisonGuideError;
    StereoEpipolarGuides comparisonGuides;
    Clock::time_point comparisonUpdated{};
    bool comparisonDirty=true;
    bool displayConnected(int i)const{return i>=0&&i<int(cameras.size())&&((smoke&&videoSmoke)||cameras[i]->connected());}
    bool displayRunning(int i)const{return displayConnected(i)&&((smoke&&videoSmoke)||cameras[i]->running());}
    CameraFrame sourceFrame(int i){return smoke&&videoSmoke?videoSmokeFrames.at(i):cameras.at(i)->latest();}
    CameraFrame displayFrame(int i){
        if(pointFrameActive(i))return pointFrames[pointSide(i)];
        auto frame=roiCamera==i?roiFrame:displayedBall(i)?displayedBall(i)->frame:sourceFrame(i);
        if(replay&&replayTimes.size()==cameras.size()){auto latest=*std::max_element(replayTimes.begin(),replayTimes.end());frame.receivedAt=Clock::now()-std::chrono::nanoseconds(latest-replayTimes[i]);}
        return frame;
    }
    std::string displayCalibrationKey(int i)const{
        const auto& c=*cameras[i];auto it=settings.cameras.find(c.device.serial);
        return c.device.serial+"|"+(it==settings.cameras.end()?"missing":it->second.archive);
    }
    cv::Mat undistortDisplay(int i,const cv::Mat& image){
        const auto& c=*cameras[i];
        if(!compatible(c))throw std::runtime_error(cameraName(c.device.serial)+": 개별 보정 필요");
        const auto& cal=settings.cameras.at(c.device.serial).calibration;
        if(image.size()!=cal.imageSize)throw std::runtime_error("보정값과 영상 크기가 다릅니다.");
        auto& v=views[i];auto key=displayCalibrationKey(i);
        if(v.mapKey!=key||v.mapX.empty()){
            cv::initUndistortRectifyMap(cal.K,cal.distortion,cv::Mat(),cal.K,cal.imageSize,CV_32FC1,v.mapX,v.mapY);v.mapKey=key;}
        cv::Mat out;cv::remap(image,out,v.mapX,v.mapY,cv::INTER_LINEAR);return out;
    }
    void drawDisplayOptions(int i){
        auto& option=layout.cameraDisplay[cameras[i]->device.serial];auto before=option;
        bool narrow=ImGui::GetContentRegionAvail().x<620;
        ImGui::SetNextItemWidth(150);ImGui::Combo("View",&option.mode,"Raw\0Undistorted\0");
        if(!narrow)ImGui::SameLine();ImGui::SetNextItemWidth(narrow?std::max(80.f,ImGui::GetContentRegionAvail().x-105.f):110.f);
        ImGui::SliderFloat("Brightness",&option.brightness,-100,100,"%.0f",ImGuiSliderFlags_AlwaysClamp);
        if(!narrow)ImGui::SameLine();ImGui::SetNextItemWidth(narrow?std::max(80.f,ImGui::GetContentRegionAvail().x-105.f):110.f);
        ImGui::SliderFloat("Contrast",&option.contrast,.25f,3.f,"%.2f",ImGuiSliderFlags_AlwaysClamp);
        if(option!=before){views[i].generation=0;comparisonDirty=true;}
    }
    void drawCamera(size_t i){
        auto& c=*cameras[i];auto& v=views[i];if(!v.visible)return;
        std::string title="영상 / "+cameraName(c.device.serial)+"###영상 / "+c.device.serial;
        ImGui::SetNextWindowSize({740,580},ImGuiCond_FirstUseEver);
        if(processingSmokeActive||triangulationSmoke){ImGui::SetNextWindowPos({10.f+float(i%2)*452.f,35.f+float(i/2)*412.f});ImGui::SetNextWindowSize({445,405});}
        if(ImGui::Begin(title.c_str(),&v.visible)){
            auto frame=displayConnected(int(i))?displayFrame(int(i)):CameraFrame{};auto stats=c.statistics();
            ImGui::Text("%s  |  %.1f fps  |  %d × %d  |  Drop %llu",pointFrameActive(int(i))?"Frozen":replay?"Playback":displayRunning(int(i))?"Grabbing":displayConnected(int(i))?"Stopped":"Disconnected",stats.fps,
                frame.image.cols,frame.image.rows,(unsigned long long)stats.gaps);
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Frame %llu · Timestamp %llu\n수신 %llu · 오류 %llu%s",(unsigned long long)frame.id,(unsigned long long)frame.timestamp,(unsigned long long)stats.received,(unsigned long long)stats.invalid,replay?"\nFPS: 녹화 원본 속도":"");
            auto& options=layout.cameraDisplay[c.device.serial];auto light=liveBrightness(c.device.serial);
            if(v.brightness!=light){v.brightness=light;v.generation=0;}
            if(options.mode==1&&v.mapKey!=displayCalibrationKey(int(i)))v.generation=0;
            bool ready=!frame.image.empty();
            if(ready&&options.mode==1&&(!compatible(c)||frame.image.size()!=settings.cameras.at(c.device.serial).calibration.imageSize)){
                ready=false;v.error="현재 영상에 맞는 개별 보정이 필요합니다.";v.generation=0;}
            else if(ready&&(v.generation==0||(v.generation!=frame.generation&&Clock::now()-v.updated>std::chrono::milliseconds(70))))try{
                cv::Mat display=applySoftwareBrightness(roiCamera==int(i)?frame.image:ballStage(int(i),frame),light);
                if(options.mode==1)display=undistortDisplay(int(i),display);
                display=adjustDisplay(display,options.brightness,options.contrast);
                v.texture.update(display);v.generation=frame.generation;v.updated=Clock::now();v.error.clear();
            }catch(const std::exception& e){v.error=e.what();ready=false;}
            float reserve=ImGui::GetContentRegionAvail().x<620?142.f:64.f;
            ImGui::BeginChild("video-image",{0,std::max(50.f,ImGui::GetContentRegionAvail().y-reserve)});
            if(ready&&v.error.empty()){v.texture.draw();auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
                if(!pointFrameActive(int(i)))drawBallOverlay(int(i),v.generation,a,{b.x-a.x,b.y-a.y},frame.image.size(),options.mode);
                drawPointOverlay(int(i),a,{b.x-a.x,b.y-a.y},frame.image.size(),options.mode);}
            else ImGui::TextWrapped("%s",!v.error.empty()?v.error.c_str():displayRunning(int(i))?"프레임 대기 중":"장치 패널에서 Grab을 시작하세요.");
            ImGui::EndChild();ImGui::Separator();drawDisplayOptions(int(i));
            if(!stats.error.empty())ImGui::TextWrapped("%s",stats.error.c_str());
        }ImGui::End();
    }
    void comparisonDefaults(){
        auto& ids=layout.comparisonSerials;if(!ids[0].empty()&&!ids[1].empty())return;
        if(settings.rig&&settings.rig->serials.size()>=2){ids={settings.rig->serials.front(),settings.rig->serials.back()};return;}
        if(cameras.size()>=2)ids={cameras[0]->device.serial,cameras[1]->device.serial};
    }
    bool prepareComparison(const std::array<int,2>& index,const std::array<CameraFrame,2>& frames){
        try{
            for(int j=0;j<2;++j){
                if(!displayConnected(index[j]))throw std::runtime_error("두 카메라를 연결하세요.");
                if(!displayRunning(index[j])&&!pointFrameActive(index[j]))throw std::runtime_error("두 카메라의 Grab을 시작하세요.");
                if(frames[j].image.empty())throw std::runtime_error("프레임 대기 중");
                if(layout.comparisonMode&&(!compatible(*cameras[index[j]])||frames[j].image.size()!=settings.cameras.at(cameras[index[j]]->device.serial).calibration.imageSize))
                    throw std::runtime_error("현재 영상에 맞는 개별 보정이 필요합니다.");
            }
            if(index[0]==index[1])throw std::runtime_error("서로 다른 카메라를 선택하세요.");
            bool frozenPair=pointFrameActive(index[0])&&pointFrameActive(index[1]);
            if(!frozenPair&&!usableStereoTiming(seconds(frames[0].receivedAt),seconds(frames[1].receivedAt),seconds(Clock::now())))throw std::runtime_error("새 프레임 대기 · 두 카메라의 Grab을 확인하세요.");
            std::string key=std::to_string(layout.comparisonMode)+"|"+displayCalibrationKey(index[0])+"|"+displayCalibrationKey(index[1]);
            comparisonGuideError.clear();
            if(layout.comparisonMode==2||layout.comparisonLines)try{
                auto transform=savedStereoTransform(settings,layout.comparisonSerials[0],layout.comparisonSerials[1]);
                auto geometryKey=displayCalibrationKey(index[0])+"|"+displayCalibrationKey(index[1])+"|"+(settings.rig?settings.rig->archive:"")+"|"+(settings.stereo?settings.stereo->archive:"");
                const auto& left=settings.cameras.at(layout.comparisonSerials[0]).calibration;const auto& right=settings.cameras.at(layout.comparisonSerials[1]).calibration;
                if(geometryKey!=comparisonGeometryKey){comparisonMaps=makeStereoRectification(left,right,transform);comparisonGeometryKey=geometryKey;}
                if(layout.comparisonMode==2)key+="|"+geometryKey;
                if(layout.comparisonLines){auto guideKey=geometryKey+"|"+std::to_string(layout.comparisonMode)+"|"+std::to_string(layout.comparisonLineCount);
                    if(guideKey!=comparisonGuideKey){comparisonGuides=makeEpipolarGuides(left,right,comparisonMaps,layout.comparisonMode,layout.comparisonLineCount);comparisonGuideKey=guideKey;}}
            }catch(const std::exception& e){comparisonGuides={};comparisonGuideKey.clear();comparisonGuideError=e.what();if(layout.comparisonMode==2)throw;}

            if(key!=comparisonKey){comparisonKey=key;comparisonDirty=true;}
            bool changed=false;
            for(int j=0;j<2;++j){auto option=layout.cameraDisplay[layout.comparisonSerials[j]];auto light=liveBrightness(layout.comparisonSerials[j]);
                if(comparisonDisplay[j]!=option||comparisonBrightness[j]!=light){comparisonDisplay[j]=option;comparisonBrightness[j]=light;comparisonDirty=true;}
                changed|=comparisonGenerations[j]!=frames[j].generation||comparisonReceivedAt[j]!=frames[j].receivedAt;}
            if(comparisonDirty||(changed&&Clock::now()-comparisonUpdated>std::chrono::milliseconds(70))){
                std::array<cv::Mat,2> output;
                for(int j=0;j<2;++j){cv::Mat display=applySoftwareBrightness(ballStage(index[j],frames[j]),comparisonBrightness[j]);
                    if(layout.comparisonMode==1)display=undistortDisplay(index[j],display);
                    if(layout.comparisonMode==2)display=rectifiedFrame(display,comparisonMaps,j);
                    output[j]=adjustDisplay(display,comparisonDisplay[j].brightness,comparisonDisplay[j].contrast);}
                for(int j=0;j<2;++j){comparisonTextures[j].update(output[j]);comparisonGenerations[j]=frames[j].generation;comparisonReceivedAt[j]=frames[j].receivedAt;comparisonImageSizes[j]=output[j].size();}
                comparisonDirty=false;comparisonUpdated=Clock::now();
            }
            comparisonError.clear();return true;
        }catch(const std::exception& e){comparisonError=e.what();comparisonDirty=true;return false;}
    }
    void drawComparisonImage(int camera,Texture& texture,float height){
        auto available=ImGui::GetContentRegionAvail();ImVec2 box(std::max(1.f,available.x),std::max(30.f,height));
        auto start=ImGui::GetCursorScreenPos();ImGui::InvisibleButton("comparison-image",box);
        if(!texture.id)return;
        float scale=std::min(box.x/texture.size.width,box.y/texture.size.height);
        ImVec2 size(texture.size.width*scale,texture.size.height*scale),a(start.x+(box.x-size.x)*.5f,start.y+(box.y-size.y)*.5f),b(a.x+size.x,a.y+size.y);
        auto draw=ImGui::GetWindowDrawList();draw->AddImage(ImTextureRef(ImTextureID(texture.id)),a,b);
        int index=deviceCamera(layout.comparisonSerials[camera]);if(index>=0&&!pointFrameActive(index))drawBallOverlay(index,comparisonGenerations[camera],a,size,comparisonImageSizes[camera],layout.comparisonMode,camera);
        if(layout.comparisonLines&&comparisonGuideError.empty()){
            static const ImU32 colors[]={IM_COL32(255,100,80,255),IM_COL32(50,210,255,255),IM_COL32(255,210,40,255),IM_COL32(180,115,255,255),
                IM_COL32(65,235,130,255),IM_COL32(255,125,210,255),IM_COL32(255,160,50,255),IM_COL32(50,240,215,255),
                IM_COL32(150,180,255,255),IM_COL32(215,240,80,255),IM_COL32(245,170,160,255),IM_COL32(230,230,230,255)};
            auto shape=comparisonImageSizes[camera];draw->PushClipRect(a,b,true);
            for(size_t n=0;n<comparisonGuides[camera].size();++n){bool labeled=false;
                for(const auto& path:comparisonGuides[camera][n]){if(path.size()<2)continue;std::vector<ImVec2> points;
                    for(auto p:path)points.push_back({a.x+(p.x+.5f)*size.x/shape.width,a.y+(p.y+.5f)*size.y/shape.height});
                    draw->AddPolyline(points.data(),int(points.size()),IM_COL32(0,0,0,150),0,3.5f);draw->AddPolyline(points.data(),int(points.size()),colors[n%12],0,1.5f);
                    if(!labeled){auto anchor=points.front().x<points.back().x?points.front():points.back();
                        ImVec2 textAt{std::clamp(anchor.x+7,a.x+4,std::max(a.x+4,b.x-28)),std::clamp(anchor.y+3,a.y+2,std::max(a.y+2,b.y-22))};
                        auto label=std::to_string(n+1);draw->AddText({textAt.x+1,textAt.y+1},IM_COL32(0,0,0,255),label.c_str());draw->AddText(textAt,colors[n%12],label.c_str());labeled=true;}
                }
            }draw->PopClipRect();
        }
        if(index>=0)drawPointOverlay(index,a,size,comparisonImageSizes[camera],layout.comparisonMode,camera);
    }
    void drawComparison(){
        ImGui::SetNextWindowSize({1160,650},ImGuiCond_FirstUseEver);
        if(triangulationSmoke){ImGui::SetNextWindowPos({10,35});ImGui::SetNextWindowSize({905,405});}
        if(ImGui::Begin("Stereo Compare",&layout.comparisonVisible)){
            comparisonDefaults();std::array<int,2> index{deviceCamera(layout.comparisonSerials[0]),deviceCamera(layout.comparisonSerials[1])};
            std::array<CameraFrame,2> frames;for(int j=0;j<2;++j)if(displayConnected(index[j]))frames[j]=displayFrame(index[j]);
            bool ready=prepareComparison(index,frames);
            if(ready){auto a=cameras[index[0]]->statistics(),b=cameras[index[1]]->statistics();
                ImGui::Text("%s  |  %.1f / %.1f fps  |  %d × %d / %d × %d  |  Δt %.1f ms",pointFrameActive(index[0])&&pointFrameActive(index[1])?"Frozen":replay?"Playback":"Grabbing",a.fps,b.fps,comparisonImageSizes[0].width,comparisonImageSizes[0].height,comparisonImageSizes[1].width,comparisonImageSizes[1].height,
                    pointFrameActive(index[0])&&pointFrameActive(index[1])?pointSkewMs:std::abs(seconds(comparisonReceivedAt[0])-seconds(comparisonReceivedAt[1]))*1000);
                if(ImGui::IsItemHovered())ImGui::SetTooltip("Δt: PC 수신 시각 차이 · 노출 동기 오차가 아닙니다.");}
            else ImGui::TextDisabled("%s",comparisonError.c_str());
            float height=std::max(80.f,ImGui::GetContentRegionAvail().y-110.f);
            if(ImGui::BeginTable("comparison-pair",2,ImGuiTableFlags_SizingStretchSame)){
                for(int j=0;j<2;++j){ImGui::TableNextColumn();ImGui::PushID(j);ImGui::TextUnformatted(cameraName(layout.comparisonSerials[j]).c_str());
                    if(ready)drawComparisonImage(j,comparisonTextures[j],height);else ImGui::Dummy({1,height});ImGui::PopID();}ImGui::EndTable();}
            ImGui::Separator();ImGui::SetNextItemWidth(145);
            int mode=layout.comparisonMode-1;
            if(ImGui::Combo("View",&mode,"Undistorted\0Rectified\0")){layout.comparisonMode=mode+1;comparisonDirty=true;}
            auto sameLineIfFits=[](float width){
                if(ImGui::GetItemRectMax().x+ImGui::GetStyle().ItemSpacing.x+width<ImGui::GetCursorScreenPos().x+ImGui::GetContentRegionAvail().x)ImGui::SameLine();};
            for(int j=0;j<2;++j){sameLineIfFits(225);ImGui::PushID(j);ImGui::SetNextItemWidth(155);
                auto label=cameraName(layout.comparisonSerials[j]);if(ImGui::BeginCombo(j?"Right":"Left",label.c_str())){
                    for(const auto& c:cameras){ImGui::PushID(c->device.serial.c_str());if(ImGui::Selectable(cameraName(c->device.serial).c_str(),c->device.serial==layout.comparisonSerials[j])){
                        layout.comparisonSerials[j]=c->device.serial;comparisonDirty=true;}ImGui::PopID();}ImGui::EndCombo();}ImGui::PopID();}
            sameLineIfFits(220);ImGui::Checkbox("Lines",&layout.comparisonLines);if(ImGui::IsItemHovered())ImGui::SetTooltip("같은 색·번호는 좌우에서 대응하는 에피폴라 선입니다.");ImGui::SameLine();
            ImGui::BeginDisabled(!layout.comparisonLines);ImGui::SetNextItemWidth(100);ImGui::SliderInt("##line-count",&layout.comparisonLineCount,1,12,"%d lines",ImGuiSliderFlags_AlwaysClamp);ImGui::EndDisabled();
            if(ready&&layout.comparisonLines&&!comparisonGuideError.empty())ImGui::TextDisabled("Lines: %s",comparisonGuideError.c_str());
        }ImGui::End();
    }
    bool monoActive()const{return monoWorkflow||calTask.active()||!brightnessCamera.empty();}
    void startMonoCalibration(){try{
        if(calTask.pending()||importTask.pending()||saveFuture.valid()||loadFailed)return;
        if(!liveRunning())throw std::runtime_error("장치 패널에서 Grab을 시작하세요.");
        board.validate();autoOptions.validate();confirmed=true;persistBoard();
        if(result)resetSession();
        if(!samples.empty()&&sessionGeometry!=geometry(*cameras[cameraIndex]))throw std::runtime_error("영상 조건이 바뀌었습니다. 하단에서 샘플을 초기화하세요.");
        discardDetection();monoWorkflow=true;detectionEnabled=true;selectedSample=-1;
        autoCapture.start(autoOptions);message="보드를 보여주세요.";
        advanceMonoCalibration();
    }catch(const std::exception& e){monoWorkflow=false;detectionEnabled=false;autoCapture.stop();fail(e);}}
    void advanceMonoCalibration(){
        if(!monoWorkflow)return;
        if(!liveRunning()){cancelWork();message="Grab이 중단되었습니다.";return;}
        if(!samples.empty()&&sessionGeometry!=geometry(*cameras[cameraIndex])){cancelWork();message="영상 조건이 바뀌었습니다. 샘플을 초기화하세요.";return;}
        autoCapture.tick(seconds(Clock::now()));
        if(samples.size()>=size_t(autoOptions.target)){
            monoWorkflow=false;autoCapture.stop();detectionEnabled=false;detectTask.cancel();newDetection=false;
            auto copy=samples;calTask.start([copy](std::stop_token){return calibrate(copy);});message="계산 중...";
        }
    }
    const char* monoCaptureStatus()const{
        switch(autoCapture.status()){
            case CaptureStatus::Hold:return "보드를 고정하세요";
            case CaptureStatus::MoveBoard:return "보드 위치·기울기를 바꾸세요";
            case CaptureStatus::Cooldown:return "다음 자세로 이동하세요";
            case CaptureStatus::Stale:return "새 프레임 대기";
            default:return "보드 검출 대기";
        }
    }
    void drawMonoOptions(){
        bool locked=monoActive()||calTask.pending()||saveFuture.valid();
        bool clearRequested=false;int kind=int(board.pattern);bool changed=false;
        if(ImGui::BeginTable("mono-options-inline",3,ImGuiTableFlags_SizingStretchSame)){
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(locked);ImGui::SetNextItemWidth(-76);
            auto label=identity.empty()?std::string("Camera"):cameraName(identity);
            if(ImGui::BeginCombo("Camera",label.c_str())){
                for(int i=0;i<int(cameras.size());++i){ImGui::PushID(i);
                    ImGui::BeginDisabled(!cameras[i]->running());
                    if(ImGui::Selectable(cameraName(cameras[i]->device.serial).c_str(),cameraIndex==i)&&cameraIndex!=i)pendingMonoCamera=i;
                    ImGui::EndDisabled();ImGui::PopID();}
                ImGui::EndCombo();}
            ImGui::EndDisabled();
            ImGui::BeginDisabled(locked||!samples.empty());ImGui::SetNextItemWidth(-76);
            if(ImGui::Combo("Pattern",&kind,"Chessboard\0ChArUco\0")){board.pattern=Pattern(kind);changed=true;}
            ImGui::EndDisabled();
            auto light=savedBrightness(identity);
            ImGui::BeginDisabled(locked||!liveRunning()||loadFailed||result.has_value());
            if(ImGui::Button("Auto Brightness"))try{
                board.validate();confirmed=true;persistBoard();discardDetection();detectionEnabled=true;selectedSample=-1;
                brightnessCamera=identity;brightnessRequestedAt=Clock::now();message="밝기 맞춤 · 보드를 고정하세요.";
            }catch(const std::exception& e){fail(e);}
            ImGui::BeginDisabled(!light.fitted);if(ImGui::Checkbox("보정 중 밝기 적용",&light.enabled))try{applyBrightness(light);}catch(const std::exception& e){fail(e);}ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(locked||!samples.empty());
            ImGui::SetNextItemWidth(65);changed|=ImGui::InputInt("##mono-columns",&board.columns,0,0);
            if(ImGui::IsItemHovered())ImGui::SetTooltip(board.pattern==Pattern::Chessboard?"가로 내부 코너 수":"가로 칸 수");
            ImGui::SameLine();ImGui::TextUnformatted("×");ImGui::SameLine();ImGui::SetNextItemWidth(65);changed|=ImGui::InputInt("##mono-rows",&board.rows,0,0);
            if(ImGui::IsItemHovered())ImGui::SetTooltip(board.pattern==Pattern::Chessboard?"세로 내부 코너 수":"세로 칸 수");
            ImGui::SameLine();ImGui::TextUnformatted(board.pattern==Pattern::Chessboard?"내부 코너":"칸 수");
            ImGui::SetNextItemWidth(110);changed|=ImGui::InputDouble("한 칸 (mm)",&board.squareMm,0,0,"%.2f");
            if(board.pattern==Pattern::Charuco){ImGui::SetNextItemWidth(110);changed|=ImGui::InputDouble("마커 (mm)",&board.markerMm,0,0,"%.2f");changed|=ImGui::Checkbox("Legacy",&board.legacy);}
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(locked);ImGui::SetNextItemWidth(110);
            if(ImGui::InputInt("목표 샘플",&autoOptions.target))autoOptions.target=std::clamp(autoOptions.target,10,80);
            ImGui::SetNextItemWidth(110);if(ImGui::InputDouble("고정 시간 (s)",&autoOptions.holdSeconds,.1,0,"%.1f"))autoOptions.holdSeconds=std::clamp(autoOptions.holdSeconds,.3,3.);
            ImGui::SetNextItemWidth(110);if(ImGui::InputDouble("수집 간격 (s)",&autoOptions.intervalSeconds,.1,0,"%.1f"))autoOptions.intervalSeconds=std::clamp(autoOptions.intervalSeconds,.5,10.);
            ImGui::EndDisabled();
            ImGui::BeginDisabled(locked||samples.empty());clearRequested=ImGui::Button("Clear Samples");ImGui::EndDisabled();
            ImGui::EndTable();
        }
        if(changed){confirmed=false;discardDetection();}
        if(clearRequested)ImGui::OpenPopup("clear-mono-samples");
        if(ImGui::BeginPopupModal("clear-mono-samples",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextUnformatted("수집한 샘플과 미저장 결과를 지웁니다.");
            if(ImGui::Button("Clear")){resetSession();message="대기";ImGui::CloseCurrentPopup();}ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();ImGui::EndPopup();}
        if(pendingMonoCamera>=0){
            if(samples.empty()||exported){resetSession();cameraIndex=pendingMonoCamera;identity=cameras[cameraIndex]->device.serial;pendingMonoCamera=-1;message="대기";}
            else ImGui::OpenPopup("switch-mono-camera");}
        if(ImGui::BeginPopupModal("switch-mono-camera",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextUnformatted("샘플과 미저장 결과를 지우고 카메라를 변경합니다.");
            if(ImGui::Button("Change")){resetSession();cameraIndex=pendingMonoCamera;identity=cameras[cameraIndex]->device.serial;pendingMonoCamera=-1;message="대기";ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button("Cancel")){pendingMonoCamera=-1;ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
    }
    void drawCalibration(){
        if(!smoke){ImGuiWindowClass cls;cls.ViewportFlagsOverrideSet=ImGuiViewportFlags_NoAutoMerge|ImGuiViewportFlags_TopMost;ImGui::SetNextWindowClass(&cls);}
        ImGui::SetNextWindowSize({1120,820},ImGuiCond_FirstUseEver);
        bool expanded=ImGui::Begin("개별 보정###개별 카메라 캘리브레이션",&showCalibration,ImGuiWindowFlags_NoDocking);
        if(!showCalibration){cancelWork();ImGui::End();return;}
        if(!expanded){if(monoActive())cancelWork();ImGui::End();return;}
        if(!offline&&samples.empty()&&!monoActive()&&!cameras.empty())identity=cameras[cameraIndex]->device.serial;
        if(monoActive()&&!saveFuture.valid()&&ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)&&!ImGui::GetIO().WantTextInput&&ImGui::IsKeyPressed(ImGuiKey_Escape))cancelWork();
        float controlsHeight=32.f+ImGui::GetFrameHeightWithSpacing()*4.f+ImGui::GetTextLineHeightWithSpacing()*2.f+30.f;
        float height=std::max(180.f,ImGui::GetContentRegionAvail().y-controlsHeight);
        if(ImGui::BeginTable("mono-layout",2,ImGuiTableFlags_Resizable)){
            ImGui::TableSetupColumn("Video",ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Samples",ImGuiTableColumnFlags_WidthFixed,170);
            ImGui::TableNextColumn();ImGui::BeginChild("mono-images",{0,height});drawMonoPreview();ImGui::EndChild();
            ImGui::TableNextColumn();ImGui::BeginChild("mono-samples",{0,height});drawMonoSamples();ImGui::EndChild();ImGui::EndTable();}
        ImGui::Separator();
        if(monoActive()){if(ImGui::Button("Cancel",{170,32}))cancelWork();}
        else {ImGui::BeginDisabled(!liveRunning()||calTask.pending()||importTask.pending()||saveFuture.valid()||loadFailed);
            if(ImGui::Button("Start Calibration",{170,32})){
                if(result&&!exported)ImGui::OpenPopup("restart-mono");else startMonoCalibration();}
            ImGui::EndDisabled();}
        ImGui::SameLine();ImGui::BeginDisabled(!result||monoActive()||calTask.pending()||saveFuture.valid()||exported||loadFailed);
        if(ImGui::Button(exported?"Saved":"Save",{90,32})){
            if(settings.cameras.contains(identity))ImGui::OpenPopup("replace-mono");else saveResult();}
        ImGui::EndDisabled();ImGui::SameLine();
        if(saveFuture.valid())ImGui::TextDisabled("저장 중...");
        else if(calTask.active())ImGui::TextDisabled("계산 중...");
        else if(calTask.pending())ImGui::TextDisabled("작업 종료 중...");
        else if(monoWorkflow)ImGui::Text("%d / %d · %s",int(samples.size()),autoOptions.target,monoCaptureStatus());
        else if(!brightnessCamera.empty())ImGui::TextDisabled("밝기 맞춤 중...");
        else if(result)ImGui::Text("RMS %.4f px · %s",result->rms,exported?"저장됨":"저장 전");
        else ImGui::TextDisabled("%d / %d",int(samples.size()),autoOptions.target);
        if(result){ImGui::SameLine();if(ImGui::Button("K / D",{70,32}))ImGui::OpenPopup("mono-matrices");
            if(ImGui::BeginPopup("mono-matrices")){ImGui::TextUnformatted("K");for(int r=0;r<3;++r)ImGui::Text("%.4f  %.4f  %.4f",result->K.at<double>(r,0),result->K.at<double>(r,1),result->K.at<double>(r,2));
                ImGui::SeparatorText("D");auto d=result->distortion.reshape(1,1);for(int i=0;i<d.cols;++i)ImGui::Text("% .7f",d.at<double>(0,i));ImGui::EndPopup();}}
        if(ImGui::BeginPopupModal("restart-mono",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextUnformatted("미저장 결과를 지우고 다시 수집합니다.");
            if(ImGui::Button("Start")){startMonoCalibration();ImGui::CloseCurrentPopup();}ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();ImGui::EndPopup();}
        if(ImGui::BeginPopupModal("replace-mono",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextUnformatted("저장된 보정값을 새 결과로 교체합니다.");
            if(ImGui::Button("Save")){saveResult();ImGui::CloseCurrentPopup();}ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();ImGui::EndPopup();}
        ImGui::Separator();
        drawMonoOptions();
        if(!liveRunning()&&!smoke)ImGui::TextColored({1,.7f,.3f,1},"장치 패널에서 Grab을 시작하세요.");
        else ImGui::TextWrapped("%s",message.c_str());
        finishBrightnessFit();
        try{
            if(newDetection&&monoWorkflow&&autoCapture.running()){
                if(canAddDetected()){if(autoCapture.observe(detected,board,samples,seconds(detectedAt),seconds(Clock::now())))addDetected();}
                else autoCapture.observe(Observation{},board,samples,seconds(detectedAt),seconds(Clock::now()));}
            advanceMonoCalibration();
        }catch(const std::exception& e){cancelWork();fail(e);}
        newDetection=false;ImGui::End();scheduleDetection();
    }
    bool liveRunning()const{return (smoke&&monoSmokeLive)||(!offline&&cameraIndex>=0&&cameraIndex<int(cameras.size())&&cameras[cameraIndex]->running());}
    void applyBrightness(const BrightnessOptions& options){
        if(loadFailed||saveFuture.valid())throw std::runtime_error("설정 저장이 가능해진 뒤 다시 시도하세요.");
        Settings next=settings;next.softwareBrightness[identity]=options;
        writeSettings(profile,next);settings=std::move(next);
        discardDetection();selectedSample=-1;
    }
    void finishBrightnessFit(){
        if(brightnessCamera.empty())return;
        if(!liveRunning()||identity!=brightnessCamera||Clock::now()-brightnessRequestedAt>std::chrono::seconds(20)){
            brightnessCamera.clear();detectionEnabled=false;detectTask.cancel();message="밝기 맞춤을 완료하지 못했습니다. 영상과 보드 규격을 확인한 뒤 다시 눌러주세요.";return;
        }
        if(!newDetection||detectedAt<brightnessRequestedAt||Clock::now()-detectedAt>std::chrono::milliseconds(1500)||detected.source!="live:"+brightnessCamera||detected.imagePoints.empty())return;
        try{
            auto options=estimateBoardBrightness(detected);applyBrightness(options);
            detectionEnabled=false;message="밝기·대비 저장 완료";
        }catch(const std::exception& e){brightnessCamera.clear();detectionEnabled=false;detectTask.cancel();fail(e);}
    }
    bool canAddDetected()const{
        if(!showCalibration||!confirmed||identity.empty()||busy()||!brightnessCamera.empty()||samples.size()>=80||!liveRunning()||detected.imagePoints.empty()||Clock::now()-detectedAt>std::chrono::milliseconds(1500))return false;
        return std::none_of(samples.begin(),samples.end(),[&](const auto& s){return s.source==detected.source&&s.frameId==detected.frameId;});
    }
    void persistBoard(){
        if(loadFailed)throw std::runtime_error("Settings load failed; original file preserved.");
        Settings next=settings;next.board=board;next.boardConfirmed=confirmed;next.autoCapture=autoOptions;if(!smoke)writeSettings(profile,next);settings=std::move(next);
    }
    void scheduleDetection(){
        if(!showCalibration||!detectionEnabled||offline||!confirmed||cameras.empty()||busy()||detectTask.pending())return;
        auto& c=*cameras[cameraIndex];if(!c.running())return;auto f=c.latest();
        if(f.image.empty()||f.generation==lastGeneration||Clock::now()-f.receivedAt>std::chrono::milliseconds(1500))return;lastGeneration=f.generation;
        auto b=board;auto id=c.device.serial;
        auto brightness=liveBrightness(id);
        detectTask.start([f,b,id,brightness](std::stop_token){auto o=detectBoard(f.image,b,brightness);o.source="live:"+id;o.frameId=f.id;o.timestamp=f.timestamp;return DetectionResult{o,id,f.receivedAt};});
    }
    void addDetected(){try{
        if(!canAddDetected())return;
        validateSample(detected,samples);samples.push_back(detected);sessionGeometry=geometry(*cameras[cameraIndex]);selectedSample=-1;invalidate();message="샘플 추가 완료. 보드 자세를 바꾸세요.";
        lastSampleAdded=Clock::now();autoCapture.captured(seconds(lastSampleAdded),samples.size());
    }catch(const std::exception& e){fail(e);}}
    void beginImport(){try{
        auto paths=chooseImages();if(paths.empty())return;auto b=board;auto existing=samples;
        importTask.start([paths,b,existing](std::stop_token stop){ImportResult r;auto all=existing;
            for(auto& p:paths){if(stop.stop_requested())break;try{if(all.size()>=80)throw std::runtime_error("최대 80장입니다.");
                auto source=utf8(p);if(std::any_of(all.begin(),all.end(),[&](const auto& v){return v.source==source;}))throw std::runtime_error("중복 파일: "+source);
                auto o=detectBoard(readImage(p),b);o.source=source;validateSample(o,all);all.push_back(o);r.samples.push_back(o);
            }catch(const std::exception& e){r.errors.push_back(e.what());}}return r;});
        message="이미지 검출 중...";
    }catch(const std::exception& e){fail(e);}}
    void pollJobs(){
        using namespace std::chrono_literals;
        try{if(auto r=reconnectTask.poll()){
            updateDeviceList(std::move(r->devices));deviceError=r->warnings;
            cameras=std::move(r->cameras);views.resize(cameras.size());cameraIndex=0;
            selectStereoDefaults();
            for(size_t i=0;i<cameras.size();++i)if(auto it=layout.cameraWindows.find(cameras[i]->device.serial);it!=layout.cameraWindows.end())views[i].visible=it->second;
            message="자동 연결 "+std::to_string(cameras.size())+"대 · 수집 시작 요청 완료. "+r->warnings;
        }}catch(const std::exception& e){deviceError=std::string("자동 연결 실패: ")+e.what();}
        try{
            if(auto d=detectTask.poll();d&&d->identity==identity){detected=std::move(d->observation);detectedAt=d->capturedAt;newDetection=true;if(selectedSample<0)previewDirty=true;}
        }catch(const std::exception& e){fail(e);}
        try{if(auto completed=importTask.poll()){
            auto& r=*completed;samples.insert(samples.end(),r.samples.begin(),r.samples.end());invalidate();
            if(!samples.empty())selectedSample=int(samples.size())-1;
            message="추가 "+std::to_string(r.samples.size())+"장 / 제외 "+std::to_string(r.errors.size())+"장";
            if(!r.errors.empty())message+=" — "+r.errors.front();
        }}catch(const std::exception& e){fail(e);}
        try{if(auto completed=calTask.poll()){
            result=std::move(*completed);monoWorkflow=false;detectionEnabled=false;exported=false;previewDirty=true;selectedSample=samples.empty()?-1:int(samples.size())-1;detectTask.cancel();newDetection=false;
            message="계산 완료 · 저장 전";
        }}catch(const std::exception& e){fail(e);}
        if(saveFuture.valid()&&saveFuture.wait_for(0ms)==std::future_status::ready)try{
            settings=saveFuture.get();
            if(savingOrigin){originSaved=true;originMessage="원점 저장 완료";savingOrigin=false;}
            else if(savingStereo){stereoExported=true;stereoMessage="상대 위치 저장 완료";savingStereo=false;}
            else{exported=true;message="보정값 저장 완료";}
            for(auto& v:views)v.generation=0;
        }catch(const std::exception& e){if(savingOrigin){originMessage=e.what();savingOrigin=false;}else if(savingStereo){stereoMessage=e.what();savingStereo=false;}else fail(e);}
        pollStereo();pollOrigin();
    }
    CameraFrame monoFrame(){
        if(smoke&&monoSmokeLive)return monoSmokeFrame;
        return !cameras.empty()&&cameraIndex>=0&&cameraIndex<int(cameras.size())?cameras[cameraIndex]->latest():CameraFrame{};
    }
    void drawMonoPreview(){
        bool enhanced=calibrationBrightnessActive();
        if(previewBrightnessActive!=enhanced){previewBrightnessActive=enhanced;previewDirty=true;releaseThumbnails();}
        CameraFrame live;
        const Observation* sample=selectedSample>=0&&selectedSample<int(samples.size())?&samples[selectedSample]:nullptr;
        if(!sample)live=monoFrame();
        const Observation* detectionPreview=nullptr;
        // Keep overlay coordinates on the exact detection image even when Grab runs at 200 FPS.
        if(!sample&&!result&&detectionEnabled&&detected.source=="live:"+identity&&
            !detected.image.empty()&&Clock::now()-detectedAt<=std::chrono::milliseconds(1500))detectionPreview=&detected;
        cv::Mat input=sample?sample->image:detectionPreview?detectionPreview->image:live.image;
        if(input.empty()&&smoke)input=detected.image;
        if(!sample&&live.generation!=monoPreviewGeneration&&Clock::now()-monoPreviewUpdated>std::chrono::milliseconds(70))previewDirty=true;
        if(input.empty()){ImGui::TextDisabled(liveRunning()?"프레임 대기 중":"장치 패널에서 Grab을 시작하세요.");return;}
        if(previewDirty)try{
            auto light=enhanced?(sample?sample->softwareBrightness:savedBrightness(identity)):BrightnessOptions{};
            cv::Mat display=applySoftwareBrightness(input,light);
            if(result){
                preview.update(input);correctedPreview.update(undistortFrame(input,*result));
            }else{
                const Observation* corners=sample?sample:detectionPreview;
                if(!corners&&live.id==detected.frameId&&detected.source=="live:"+identity)corners=&detected;
                if(corners)display=drawBoardOverlay(display,*corners,board);
                preview.update(display);
            }
            monoPreviewGeneration=live.generation;monoPreviewUpdated=Clock::now();previewDirty=false;
        }catch(const std::exception& e){correctedPreview.release();fail(e);previewDirty=false;}
        bool compare=result.has_value();
        auto renderImage=[&](const char* label,Texture& texture){
            ImGui::TextUnformatted(label);float available=std::max(40.f,ImGui::GetContentRegionAvail().y-34.f);texture.draw(available);
        };
        if(compare&&ImGui::BeginTable("mono-before-after",2,ImGuiTableFlags_SizingStretchSame)){
            ImGui::TableNextColumn();renderImage("보정 전",preview);ImGui::TableNextColumn();renderImage("보정 후",correctedPreview);ImGui::EndTable();
        }else renderImage(sample?"Sample":"Live",preview);
        if(sample)ImGui::TextDisabled("Sample %02d   |   %d × %d",selectedSample+1,input.cols,input.rows);
        else {double fps=0;uint64_t gaps=0;if(!cameras.empty()){auto stats=cameras[cameraIndex]->statistics();fps=stats.fps;gaps=stats.gaps;}
            ImGui::TextDisabled("%s   |   %.1f fps   |   %d × %d   |   Drop %llu",liveRunning()?"Grabbing":"Stopped",fps,input.cols,input.rows,(unsigned long long)gaps);}
    }
    void drawMonoSamples(){
        ImGui::Text("Samples  %d",int(samples.size()));
        ImGui::BeginDisabled(monoActive());
        if(ImGui::Selectable("Live",selectedSample<0)){selectedSample=-1;previewDirty=true;}
        ImGui::EndDisabled();ImGui::Separator();
        thumbnails.resize(samples.size());
        int erase=-1;
        for(int i=0;i<int(samples.size());++i){ImGui::PushID(i);auto& texture=thumbnails[i];
            if(!texture.id)texture.update(samples[i].image,160);
            float width=std::max(20.f,ImGui::GetContentRegionAvail().x-8),scale=std::min(width/texture.size.width,105.f/texture.size.height);
            ImGui::BeginDisabled(monoActive());ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{3,3});
            ImGui::PushStyleColor(ImGuiCol_Button,selectedSample==i?ImVec4(.25f,.6f,.55f,1):ImVec4(.12f,.16f,.2f,1));
            if(ImGui::ImageButton("sample",ImTextureRef(ImTextureID(texture.id)),{texture.size.width*scale,texture.size.height*scale})){selectedSample=i;previewDirty=true;}
            ImGui::PopStyleColor();ImGui::PopStyleVar();ImGui::EndDisabled();
            if(!monoActive()&&!calTask.pending()&&!saveFuture.valid()&&ImGui::BeginPopupContextItem("sample-actions")){
                if(ImGui::MenuItem("Delete"))erase=i;ImGui::EndPopup();}
            if(result&&i<int(result->perViewErrors.size()))ImGui::Text("%02d  ·  %.3f px",i+1,result->perViewErrors[i]);else ImGui::Text("%02d",i+1);
            ImGui::PopID();
        }
        if(erase>=0){thumbnails[erase].release();thumbnails.erase(thumbnails.begin()+erase);samples.erase(samples.begin()+erase);
            selectedSample=samples.empty()?-1:std::min(erase,int(samples.size())-1);invalidate();message="샘플 삭제됨";}
    }
    void saveResult(){try{
        if(!result)return;autoCapture.stop();auto now=std::chrono::system_clock::now().time_since_epoch();
        auto folder=data/"calibration-captures"/std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        Settings next=settings;next.board=board;next.boardConfirmed=confirmed;next.autoCapture=autoOptions;
        SavedCamera c;c.identity=identity;c.sourceKind=offline?"offline":"live";c.archive=utf8(folder);c.board=board;c.calibration=*result;c.geometry=sessionGeometry;
        if(!offline)c.model=cameras[cameraIndex]->device.model;next.cameras[identity]=c;
        auto copy=samples;auto id=identity;auto b=board;auto r=*result;auto path=profile;
        saveFuture=std::async(std::launch::async,[next,folder,copy,id,b,r,path]{saveCalibration(utf8(folder),id,b,copy,r);writeSettings(path,next);return next;});
        message="원본 이미지와 보정값 저장 중...";
    }catch(const std::exception& e){fail(e);}}
    std::vector<int> stereoIndices()const{return stereoCount==3?std::vector<int>{stereoA,stereoB,stereoC}:std::vector<int>{stereoA,stereoB};}
    bool stereoIndicesValid()const{
        auto indices=stereoIndices();
        for(size_t i=0;i<indices.size();++i){if(indices[i]<0||indices[i]>=int(cameras.size()))return false;for(size_t j=0;j<i;++j)if(indices[j]==indices[i])return false;}
        return true;
    }
    std::vector<std::string> stereoSerials()const{
        std::vector<std::string> ids;if(stereoIndicesValid())for(int i:stereoIndices())ids.push_back(cameras[i]->device.serial);return ids;
    }
    bool stereoBrightnessActive()const{return showStereo&&stereoDetectionEnabled&&!stereoResult&&!stereoExported;}
    void releaseStereoThumbnails(){for(auto& group:stereoThumbnails)for(auto& t:group)t.release();stereoThumbnails.clear();}
    void cancelStereo(){
        stereoWorkflow=false;stereoDetectionEnabled=false;stereoCapture.stop();stereoDetectTask.cancel();stereoCalTask.cancel();
        stereoPreviewDirty=true;stereoMessage="취소됨";stereoWaitReason.clear();
    }
    void resetStereo(){
        cancelStereo();stereoSamples.clear();stereoDetected={};stereoResult.reset();stereoExported=false;
        stereoSelected=-1;stereoGenerations={};stereoPreviewGenerations={};stereoSessionSerials.clear();stereoSessionArchives.clear();releaseStereoThumbnails();
    }
    void selectStereoDefaults(){
        if(!stereoSamples.empty())return;
        std::vector<int> indices;
        for(const auto& serial:settings.stereoSerials)for(int i=0;i<int(cameras.size());++i)
            if(cameras[i]->connected()&&cameras[i]->device.serial==serial&&std::find(indices.begin(),indices.end(),i)==indices.end())indices.push_back(i);
        for(int i=0;i<int(cameras.size());++i)if(cameras[i]->connected()&&compatible(*cameras[i])&&std::find(indices.begin(),indices.end(),i)==indices.end())indices.push_back(i);
        for(int i=0;i<int(cameras.size());++i)if(cameras[i]->connected()&&std::find(indices.begin(),indices.end(),i)==indices.end())indices.push_back(i);
        stereoA=indices.empty()?0:indices[0];stereoB=indices.size()>1?indices[1]:1;stereoC=indices.size()>2?indices[2]:2;
    }
    std::string stereoReadiness()const{
        if(!stereoIndicesValid())return "선택한 수만큼 서로 다른 카메라를 연결·선택하세요.";
        for(int index:stereoIndices()){
            auto& c=*cameras[index];
            if(!compatible(c))return c.device.serial+": 현재 영상 조건에 맞는 개별 보정값을 먼저 저장하세요.";
            if(!c.running()&&!(smoke&&stereoSmokeLive))return cameraName(c.device.serial)+": 장치 패널에서 Grab을 시작하세요.";
        }
        return {};
    }
    void beginStereoCapture(){try{
        auto readiness=stereoReadiness();if(!readiness.empty())throw std::runtime_error(readiness);
        if(loadFailed||busy()||stereoCalTask.pending())throw std::runtime_error("진행 중 작업이 끝난 뒤 다시 시작하세요.");
        stereoBoard.validate();stereoAutoOptions.validate();
        if(stereoResult)resetStereo();
        if(!stereoSamples.empty()&&!stereoSessionCurrent())throw std::runtime_error("카메라 또는 개별 보정이 변경되었습니다. Clear Samples를 누르세요.");
        stereoConfirmed=true;
        Settings next=settings;next.board=stereoBoard;next.boardConfirmed=true;
        next.stereoSerials=stereoSerials();next.relativeCameraCount=stereoCount;
        if(!smoke)writeSettings(profile,next);settings=std::move(next);
        if(stereoSamples.empty()){stereoSessionSerials=stereoSerials();stereoSessionArchives.clear();for(const auto& id:stereoSessionSerials)stereoSessionArchives.push_back(settings.cameras.at(id).archive);}
        stereoDetectTask.cancel();stereoDetected={};stereoGenerations={};stereoWaitReason.clear();
        stereoResult.reset();stereoExported=false;stereoSelected=-1;stereoPreviewDirty=true;stereoDetectionEnabled=true;
        stereoWorkflow=true;stereoCapture.start(stereoCount,stereoAutoOptions);
        stereoMessage="모든 카메라에 보드를 보여주세요.";advanceStereoCalibration();
    }catch(const std::exception& e){cancelStereo();stereoMessage=e.what();}}
    bool stereoActive()const{return stereoWorkflow||stereoCalTask.active();}
    bool stereoSessionCurrent()const{
        if(stereoSessionSerials!=stereoSerials()||stereoSessionArchives.size()!=stereoSessionSerials.size())return false;
        for(size_t i=0;i<stereoSessionSerials.size();++i){auto it=settings.cameras.find(stereoSessionSerials[i]);if(it==settings.cameras.end()||it->second.archive!=stereoSessionArchives[i])return false;}
        return true;
    }
    void advanceStereoCalibration(){
        if(!stereoWorkflow)return;
        auto readiness=stereoReadiness();
        if(!readiness.empty()||!stereoSessionCurrent()){cancelStereo();stereoMessage=readiness.empty()?"개별 보정이 변경되었습니다. Clear Samples를 누르세요.":readiness;return;}
        stereoCapture.tick(seconds(Clock::now()));
        if(stereoSamples.size()>=size_t(stereoAutoOptions.target))computeStereo();
    }
    CameraFrame stereoFrame(int side){
        if(smoke&&stereoSmokeLive)return stereoSmokeFrames[side];
        int index=side==0?stereoA:side==1?stereoB:stereoC;
        if(index>=0&&index<int(cameras.size())){auto f=cameras[index]->latest();if(!f.image.empty()||!smoke)return f;}
        if(smoke&&side<int(stereoDetected.sample.cameras.size()))return {stereoDetected.sample.cameras[side].image,stereoDetected.sample.cameras[side].frameId,0,1,Clock::now()};
        return {};
    }
    void scheduleStereoDetection(){
        if(!showStereo||!stereoDetectionEnabled||busy()||stereoDetectTask.pending()||!stereoReadiness().empty())return;
        auto indices=stereoIndices();std::vector<CameraFrame> frames;std::vector<double> times;std::vector<BrightnessOptions> lights;
        for(size_t i=0;i<indices.size();++i){
            auto f=stereoFrame(int(i));
            if(f.image.empty()||seconds(Clock::now())-seconds(f.receivedAt)>1.5){stereoWaitReason=cameraName(cameras[indices[i]]->device.serial)+": 새 프레임 대기";return;}
            if(f.generation==stereoGenerations[i])return;
            times.push_back(seconds(f.receivedAt));frames.push_back(f);lights.push_back(liveBrightness(cameras[indices[i]]->device.serial));
        }
        if(!usableRigTiming(times,seconds(Clock::now()))){stereoWaitReason="카메라 수신 시각 차이가 100 ms를 넘습니다.";return;}
        stereoWaitReason.clear();
        for(size_t i=0;i<frames.size();++i)stereoGenerations[i]=frames[i].generation;
        auto boardCopy=stereoBoard;auto ids=stereoSerials();
        stereoDetectTask.start([frames,boardCopy,ids,lights,times](std::stop_token stop){
            auto started=Clock::now();StereoDetection d;d.serials=ids;auto [lo,hi]=std::minmax_element(times.begin(),times.end());d.sample.receiptSkewMs=(*hi-*lo)*1000;
            for(size_t i=0;i<frames.size();++i){
                if(stop.stop_requested())return d;
                auto o=detectBoard(frames[i].image,boardCopy,lights[i]);o.source="live:"+ids[i];o.frameId=frames[i].id;o.timestamp=frames[i].timestamp;
                d.sample.cameras.push_back(std::move(o));d.receivedAt.push_back(frames[i].receivedAt);
            }
            d.detectionMs=std::chrono::duration<double,std::milli>(Clock::now()-started).count();return d;
        });
    }
    void pollStereo(){
        try{if(auto d=stereoDetectTask.poll();d&&stereoIndicesValid()&&d->serials==stereoSerials()){
            stereoDetected=std::move(*d);if(stereoSelected<0)stereoPreviewDirty=true;
            if(stereoWorkflow&&stereoCapture.running()){
                double now=seconds(Clock::now());std::vector<double> times;for(auto t:stereoDetected.receivedAt)times.push_back(seconds(t));
                if(stereoCapture.observe(stereoDetected.sample,stereoBoard,stereoSamples,times,now)){
                    stereoSamples.push_back(stereoDetected.sample);stereoCapture.captured(now,stereoSamples.size());
                    stereoMessage="샘플 "+std::to_string(stereoSamples.size())+"묶음 추가 · 모든 화면에 보이도록 자세를 바꾸세요.";
                }
            }
        }}catch(const std::exception& e){cancelStereo();stereoMessage=e.what();}
        try{if(auto r=stereoCalTask.poll()){
            stereoResult=std::move(*r);stereoWorkflow=false;stereoExported=false;stereoDetectionEnabled=false;stereoPreviewDirty=true;stereoSelected=stereoSamples.empty()?-1:int(stereoSamples.size())-1;
            stereoMessage="계산 완료 · 저장 전";
        }}catch(const std::exception& e){stereoMessage=e.what();}
    }
    void computeStereo(){try{
        if(stereoCalTask.pending()||saveFuture.valid())return;
        auto readiness=stereoReadiness();if(!readiness.empty())throw std::runtime_error(readiness);
        if(!stereoSessionCurrent())throw std::runtime_error("개별 보정이 변경되었습니다. Clear Samples를 누르세요.");
        cancelStereo();stereoResult.reset();stereoExported=false;
        auto groups=stereoSamples;auto b=stereoBoard;std::vector<Calibration> intrinsics;
        for(int index:stereoIndices())intrinsics.push_back(settings.cameras.at(cameras[index]->device.serial).calibration);
        stereoCalTask.start([groups,b,intrinsics](std::stop_token stop){return calibrateRig(groups,b,intrinsics,stop);});
        stereoMessage="계산 중...";
    }catch(const std::exception& e){cancelStereo();stereoMessage=e.what();}}
    void saveStereo(){try{
        if(!stereoResult||!stereoIndicesValid()||loadFailed||busy()||stereoCalTask.pending())return;
        if(!stereoSessionCurrent())throw std::runtime_error("카메라 또는 개별 보정이 변경되었습니다. 다시 보정하세요.");
        auto now=std::chrono::system_clock::now().time_since_epoch();
        auto folder=data/"rig-captures"/std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        Settings::SavedRig saved;saved.serials=stereoSerials();std::vector<Calibration> intrinsics;
        for(const auto& id:saved.serials){auto& c=settings.cameras.at(id);saved.intrinsicArchives.push_back(c.archive);intrinsics.push_back(c.calibration);}
        saved.board=stereoBoard;saved.archive=utf8(folder);saved.calibration=*stereoResult;saved.validate();
        Settings next=settings;next.rig=saved;auto groups=stereoSamples;auto path=profile;
        saveFuture=std::async(std::launch::async,[next,saved,groups,intrinsics,path]{
            saveRigArchive(saved.archive,saved.serials,saved.board,groups,intrinsics,saved.calibration);
            writeSettings(path,next);return next;
        });savingStereo=true;stereoMessage="저장 중...";
    }catch(const std::exception& e){stereoMessage=e.what();}}
    void drawRigResult(const RigCalibration& r){
        ImGui::TextUnformatted("A 기준 위치 · X 오른쪽 / Y 아래 / Z 전방 · 단위 mm");
        for(size_t i=0;i<r.fromReference.size();++i){const auto& edge=r.fromReference[i];cv::Mat c=edge.secondCenterInFirst()*1000;
            ImGui::Text("%c: X %.2f / Y %.2f / Z %.2f | A와 거리 %.2f mm | RMS %.3f px",int('B'+i),c.at<double>(0),c.at<double>(1),c.at<double>(2),cv::norm(edge.T)*1000,edge.rms);
        }
        if(ImGui::TreeNode("상세")){
            for(size_t i=0;i<r.fromReference.size();++i){ImGui::PushID(int(i));ImGui::SeparatorText(i==0?"A → B":"A → C");drawStereoResult(r.fromReference[i],char('B'+i));ImGui::PopID();}
            ImGui::TreePop();
        }
    }
    void drawStereoResult(const StereoCalibration& r,char target='B'){
        cv::Mat center=r.secondCenterInFirst()*1000,rotation;cv::Rodrigues(r.R,rotation);
        ImGui::Text("카메라 간 거리: %.2f mm",cv::norm(r.T)*1000);
        ImGui::Text("A 기준 %c 위치 · X %.2f / Y %.2f / Z %.2f mm",target,center.at<double>(0),center.at<double>(1),center.at<double>(2));
        ImGui::Text("상대 회전각: %.3f 도 | 재투영 RMS: %.4f px",cv::norm(rotation)*180/CV_PI,r.rms);
        ImGui::TextWrapped("A 원점 · X 오른쪽 / Y 아래 / Z 렌즈 앞쪽. 위치는 %c 카메라의 광학 중심입니다.",target);
        if(r.rms>2)ImGui::TextColored({1,.65f,.2f,1},"오차가 큽니다. 쌍별 오차와 개별 보정, 보드 움직임을 확인하세요.");
        if(ImGui::TreeNode("R / T 변환 행렬")){
            ImGui::Text("X_%c = R × X_A + T (단위 m)",target);
            for(int i=0;i<3;++i)ImGui::Text("% .6f  % .6f  % .6f | T % .6f",r.R.at<double>(i,0),r.R.at<double>(i,1),r.R.at<double>(i,2),r.T.at<double>(i));
            ImGui::TreePop();
        }
    }
    void drawStereoOptions(){
        bool locked=stereoActive()||stereoCalTask.pending()||busy();bool changed=false,clearRequested=false;
        if(ImGui::BeginTable("stereo-options-inline",3,ImGuiTableFlags_SizingStretchSame)){
            ImGui::TableNextColumn();ImGui::BeginDisabled(locked||!stereoSamples.empty());
            int countIndex=stereoCount-2;ImGui::SetNextItemWidth(-85);
            if(ImGui::Combo("Cameras",&countIndex,"2\0" "3\0")){resetStereo();stereoCount=countIndex+2;}
            for(int side=0;side<stereoCount;++side){
                int& index=side==0?stereoA:side==1?stereoB:stereoC;
                auto label=index>=0&&index<int(cameras.size())?cameraName(cameras[index]->device.serial):std::string("Camera");
                ImGui::SetNextItemWidth(-85);
                if(ImGui::BeginCombo(side==0?"A (Ref)":side==1?"B":"C",label.c_str())){
                    for(int i=0;i<int(cameras.size());++i){ImGui::PushID(i);ImGui::BeginDisabled(!cameras[i]->running()&&!(smoke&&stereoSmokeLive));
                        if(ImGui::Selectable(cameraName(cameras[i]->device.serial).c_str(),index==i)&&index!=i){resetStereo();index=i;}
                        ImGui::EndDisabled();ImGui::PopID();}ImGui::EndCombo();}
            }
            ImGui::EndDisabled();ImGui::TableNextColumn();ImGui::BeginDisabled(locked||!stereoSamples.empty());
            int type=int(stereoBoard.pattern);ImGui::SetNextItemWidth(-85);
            if(ImGui::Combo("Pattern",&type,"Chessboard\0ChArUco\0")){stereoBoard.pattern=Pattern(type);changed=true;}
            ImGui::SetNextItemWidth(60);changed|=ImGui::InputInt("##stereo-columns",&stereoBoard.columns,0,0);ImGui::SameLine();ImGui::TextUnformatted("×");ImGui::SameLine();
            ImGui::SetNextItemWidth(60);changed|=ImGui::InputInt("##stereo-rows",&stereoBoard.rows,0,0);ImGui::SameLine();ImGui::TextUnformatted(stereoBoard.pattern==Pattern::Chessboard?"내부 코너":"칸 수");
            ImGui::SetNextItemWidth(110);changed|=ImGui::InputDouble("한 칸 (mm)",&stereoBoard.squareMm,0,0,"%.2f");
            if(stereoBoard.pattern==Pattern::Charuco){ImGui::SetNextItemWidth(85);changed|=ImGui::InputDouble("마커 (mm)",&stereoBoard.markerMm,0,0,"%.2f");ImGui::SameLine();changed|=ImGui::Checkbox("Legacy",&stereoBoard.legacy);}
            ImGui::EndDisabled();ImGui::TableNextColumn();ImGui::BeginDisabled(locked);
            ImGui::SetNextItemWidth(110);if(ImGui::InputInt("목표 샘플",&stereoAutoOptions.target))stereoAutoOptions.target=std::clamp(stereoAutoOptions.target,10,80);
            ImGui::SetNextItemWidth(110);if(ImGui::InputDouble("고정 시간 (s)",&stereoAutoOptions.holdSeconds,.1,0,"%.1f"))stereoAutoOptions.holdSeconds=std::clamp(stereoAutoOptions.holdSeconds,.3,3.);
            ImGui::SetNextItemWidth(110);if(ImGui::InputDouble("수집 간격 (s)",&stereoAutoOptions.intervalSeconds,.1,0,"%.1f"))stereoAutoOptions.intervalSeconds=std::clamp(stereoAutoOptions.intervalSeconds,.5,10.);
            ImGui::EndDisabled();ImGui::BeginDisabled(locked||stereoSamples.empty());clearRequested=ImGui::Button("Clear Samples");ImGui::EndDisabled();ImGui::EndTable();
        }
        if(changed){stereoConfirmed=false;resetStereo();}
        if(clearRequested)ImGui::OpenPopup("clear-stereo-samples");
        if(ImGui::BeginPopupModal("clear-stereo-samples",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextUnformatted("수집한 샘플과 미저장 결과를 지웁니다.");
            if(ImGui::Button("Clear")){resetStereo();stereoMessage="대기";ImGui::CloseCurrentPopup();}ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();ImGui::EndPopup();}
    }
    void drawStereo(){
        if(!smoke){ImGuiWindowClass cls;cls.ViewportFlagsOverrideSet=ImGuiViewportFlags_NoAutoMerge|ImGuiViewportFlags_TopMost;ImGui::SetNextWindowClass(&cls);}
        ImGui::SetNextWindowSize({1240,900},ImGuiCond_FirstUseEver);
        if(smoke&&stereoSmokeLive)ImGui::SetNextWindowCollapsed(stereoSmokeFolded);
        bool expanded=ImGui::Begin("상대 위치 보정###스테레오 상대 위치 보정",&showStereo,ImGuiWindowFlags_NoDocking);
        if(!showStereo){cancelStereo();ImGui::End();return;}
        if(!expanded){ImGui::End();advanceStereoCalibration();scheduleStereoDetection();return;}
        if(stereoActive()&&!saveFuture.valid()&&ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)&&!ImGui::GetIO().WantTextInput&&ImGui::IsKeyPressed(ImGuiKey_Escape))cancelStereo();
        float controlsHeight=32.f+ImGui::GetFrameHeightWithSpacing()*4.f+ImGui::GetTextLineHeightWithSpacing()*2.f+30.f;
        float height=std::max(180.f,ImGui::GetContentRegionAvail().y-controlsHeight);
        if(ImGui::BeginTable("stereo-layout",2,ImGuiTableFlags_Resizable)){
            ImGui::TableSetupColumn("Video",ImGuiTableColumnFlags_WidthStretch);ImGui::TableSetupColumn("Samples",ImGuiTableColumnFlags_WidthFixed,230);
            ImGui::TableNextColumn();ImGui::BeginChild("stereo-images",{0,height});drawStereoImages();ImGui::EndChild();
            ImGui::TableNextColumn();ImGui::BeginChild("stereo-samples",{0,height});drawStereoSamples();ImGui::EndChild();ImGui::EndTable();}
        ImGui::Separator();
        auto readiness=stereoReadiness();
        if(stereoActive()){if(ImGui::Button("Cancel",{170,32}))cancelStereo();}
        else {ImGui::BeginDisabled(!readiness.empty()||busy()||stereoCalTask.pending()||loadFailed);
            if(ImGui::Button("Start Calibration",{170,32})){if(stereoResult&&!stereoExported)ImGui::OpenPopup("restart-stereo");else beginStereoCapture();}ImGui::EndDisabled();}
        ImGui::SameLine();ImGui::BeginDisabled(!stereoResult||stereoActive()||busy()||stereoCalTask.pending()||loadFailed||stereoExported);
        if(ImGui::Button(stereoExported?"Saved":"Save",{90,32})){if(settings.rig||settings.stereo)ImGui::OpenPopup("replace-stereo");else saveStereo();}ImGui::EndDisabled();ImGui::SameLine();
        if(saveFuture.valid())ImGui::TextDisabled("저장 중...");
        else if(stereoCalTask.active())ImGui::TextDisabled("계산 중...");
        else if(stereoCalTask.pending())ImGui::TextDisabled("작업 종료 중...");
        else if(stereoWorkflow){ImGui::Text("Auto  %d / %d",int(stereoSamples.size()),stereoAutoOptions.target);ImGui::SameLine();
            ImGui::ProgressBar(stereoCapture.progress(),{130,0},"Hold");}
        else if(stereoResult){double worst=0;for(const auto& edge:stereoResult->fromReference)worst=std::max(worst,edge.rms);ImGui::Text("RMS %.4f px · %s",worst,stereoExported?"저장됨":"저장 전");}
        else ImGui::TextDisabled("%d / %d",int(stereoSamples.size()),stereoAutoOptions.target);
        bool storedRig=settings.rig&&settings.rig->serials==stereoSerials()&&settings.rig->matches(settings.cameras);
        bool storedPair=stereoCount==2&&settings.stereo&&stereoIndicesValid()&&settings.stereo->first==cameras[stereoA]->device.serial&&settings.stereo->second==cameras[stereoB]->device.serial&&settings.stereo->matches(settings.cameras);
        if(stereoResult||storedRig||storedPair){ImGui::SameLine();if(ImGui::Button("R / T",{70,32}))ImGui::OpenPopup("stereo-matrices");
            if(ImGui::BeginPopup("stereo-matrices")){if(stereoResult)drawRigResult(*stereoResult);else if(storedRig)drawRigResult(settings.rig->calibration);else drawStereoResult(settings.stereo->calibration);ImGui::EndPopup();}}
        if(ImGui::BeginPopupModal("restart-stereo",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextUnformatted("미저장 결과를 지우고 다시 수집합니다.");if(ImGui::Button("Start")){beginStereoCapture();ImGui::CloseCurrentPopup();}ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();ImGui::EndPopup();}
        if(ImGui::BeginPopupModal("replace-stereo",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextUnformatted("저장된 상대 위치를 새 결과로 교체합니다.");if(ImGui::Button("Save")){saveStereo();ImGui::CloseCurrentPopup();}ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();ImGui::EndPopup();}
        ImGui::Separator();drawStereoOptions();
        if(!readiness.empty()&&!smoke)ImGui::TextColored({1,.7f,.3f,1},"%s",readiness.c_str());
        else if(stereoWorkflow){ImGui::TextWrapped("%s",stereoWaitReason.empty()?"선택한 모든 카메라에서 보드를 검출하고 고정하면 자동 수집합니다.":stereoWaitReason.c_str());
            if(stereoDetected.detectionMs>1000)ImGui::TextColored({1,.7f,.3f,1},"검출 %.0f ms · 검출이 느려 고정 시간이 초기화될 수 있습니다.",stereoDetected.detectionMs);}
        else ImGui::TextWrapped("%s",stereoMessage.c_str());
        advanceStereoCalibration();ImGui::End();scheduleStereoDetection();
    }
    std::string stereoCameraCaptureStatus(int side)const{
        if(side>=int(stereoDetected.sample.cameras.size()))return "보드 검출 중...";
        if(side>=int(stereoDetected.receivedAt.size())||seconds(Clock::now())-seconds(stereoDetected.receivedAt[side])>1.5)return "검출 결과 지연 · 새 결과 대기";
        const auto& cameraDetection=stereoDetected.sample.cameras[side];auto count=cameraDetection.imagePoints.size();
        if(count<6||(stereoBoard.pattern==Pattern::Chessboard&&count!=size_t(stereoBoard.columns*stereoBoard.rows)))return "보드 미검출 · 코너 수와 전체 보드 확인";
        std::string state;
        switch(stereoCapture.status(side)){
            case CaptureStatus::Hold:state="고정 "+std::to_string(int(stereoCapture.progress(side)*100))+"%";break;
            case CaptureStatus::MoveBoard:state="기존 자세 · 위치/각도 변경";break;
            case CaptureStatus::Cooldown:state="다음 자세로 이동";break;
            case CaptureStatus::Stale:state="새 프레임 대기";break;
            default:state="다른 카메라 검출/수신 대기";break;
        }
        return "코너 "+std::to_string(count)+" · "+state;
    }
    void drawStereoImages(){
        bool enhanced=stereoBrightnessActive();
        if(stereoPreviewEnhanced!=enhanced){stereoPreviewEnhanced=enhanced;stereoPreviewDirty=true;}
        const RigSample* group=stereoSelected>=0&&stereoSelected<int(stereoSamples.size())?&stereoSamples[stereoSelected]:nullptr;
        if(ImGui::BeginTable("stereo-previews",stereoCount,ImGuiTableFlags_SizingStretchSame)){
            for(int side=0;side<stereoCount;++side){ImGui::TableNextColumn();int index=side==0?stereoA:side==1?stereoB:stereoC;
                bool valid=index>=0&&index<int(cameras.size());std::string name=valid?cameraName(cameras[index]->device.serial):"Camera "+std::string(1,char('A'+side));
                ImGui::TextUnformatted(name.c_str());
                if(stereoWorkflow)ImGui::TextWrapped("%s",stereoCameraCaptureStatus(side).c_str());
                auto f=group?CameraFrame{}:stereoFrame(side);const Observation* observation=group&&side<int(group->cameras.size())?&group->cameras[side]:nullptr;
                // Show the completed detection on its own frame. At high FPS the newest live frame
                // almost never has the same ID, which previously hid all detected corners.
                if(!group&&stereoWorkflow&&side<int(stereoDetected.sample.cameras.size())&&side<int(stereoDetected.receivedAt.size())&&
                    seconds(Clock::now())-seconds(stereoDetected.receivedAt[side])<=1.5)observation=&stereoDetected.sample.cameras[side];
                cv::Mat input=observation?observation->image:f.image;
                if(!input.empty()){
                    if(stereoPreviewDirty||(!group&&stereoPreviewGenerations[side]!=f.generation))try{
                        auto light=enhanced?(observation?observation->softwareBrightness:valid?savedBrightness(cameras[index]->device.serial):BrightnessOptions{}):BrightnessOptions{};
                        auto display=applySoftwareBrightness(input,light);const Observation* corners=observation;
                        if(!corners&&side<int(stereoDetected.sample.cameras.size())&&stereoDetected.sample.cameras[side].frameId==f.id)corners=&stereoDetected.sample.cameras[side];
                        if(!stereoResult&&corners)display=drawBoardOverlay(display,*corners,stereoBoard);
                        stereoPreview[side].update(display);stereoPreviewGenerations[side]=f.generation;
                    }catch(const std::exception& e){stereoMessage=e.what();}
                    stereoPreview[side].draw(std::max(40.f,ImGui::GetContentRegionAvail().y-38));
                    if(group)ImGui::TextDisabled("Sample %02d | %d × %d",stereoSelected+1,input.cols,input.rows);
                    else {double fps=valid?cameras[index]->statistics().fps:0;bool running=(smoke&&stereoSmokeLive)||(valid&&cameras[index]->running());ImGui::TextDisabled("%s | %.1f fps | %d × %d",running?"Grabbing":"Stopped",fps,input.cols,input.rows);}
                }else ImGui::TextDisabled(valid&&cameras[index]->running()?"프레임 대기 중":"Grab을 시작하세요.");
            }ImGui::EndTable();stereoPreviewDirty=false;
        }
    }
    void drawStereoSamples(){
        ImGui::Text("Samples  %d",int(stereoSamples.size()));ImGui::BeginDisabled(stereoActive());
        if(ImGui::Selectable("Live",stereoSelected<0)){stereoSelected=-1;stereoPreviewDirty=true;}ImGui::EndDisabled();ImGui::Separator();
        stereoThumbnails.resize(stereoSamples.size());int erase=-1;
        for(int n=0;n<int(stereoSamples.size());++n){ImGui::PushID(n);const auto& group=stereoSamples[n];
            float width=std::max(20.f,(ImGui::GetContentRegionAvail().x-(stereoCount-1)*ImGui::GetStyle().ItemSpacing.x)/stereoCount-6);
            for(int side=0;side<stereoCount&&side<int(group.cameras.size());++side){auto& texture=stereoThumbnails[n][side];if(!texture.id)texture.update(group.cameras[side].image,160);
                if(side)ImGui::SameLine();float scale=std::min(width/texture.size.width,70.f/texture.size.height);
                ImGui::BeginDisabled(stereoActive());ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{3,3});ImGui::PushStyleColor(ImGuiCol_Button,stereoSelected==n?ImVec4(.25f,.6f,.55f,1):ImVec4(.12f,.16f,.2f,1));
                if(ImGui::ImageButton(side==0?"A":side==1?"B":"C",ImTextureRef(ImTextureID(texture.id)),{texture.size.width*scale,texture.size.height*scale})){stereoSelected=n;stereoPreviewDirty=true;}
                ImGui::PopStyleColor();ImGui::PopStyleVar();ImGui::EndDisabled();
                if(!stereoActive()&&!stereoCalTask.pending()&&!saveFuture.valid()&&ImGui::BeginPopupContextItem(side==0?"sample-A":side==1?"sample-B":"sample-C")){if(ImGui::MenuItem("Delete"))erase=n;ImGui::EndPopup();}
            }
            if(stereoResult){double worst=0;for(const auto& edge:stereoResult->fromReference)if(n<int(edge.perPairErrors.size()))worst=std::max(worst,edge.perPairErrors[n]);ImGui::Text("%02d · %.3f px",n+1,worst);}else ImGui::Text("%02d · %.1f ms",n+1,group.receiptSkewMs);
            ImGui::PopID();
        }
        if(erase>=0){stereoSamples.erase(stereoSamples.begin()+erase);releaseStereoThumbnails();stereoSelected=stereoSamples.empty()?-1:std::min(erase,int(stereoSamples.size())-1);stereoResult.reset();stereoExported=false;stereoPreviewDirty=true;stereoMessage="샘플 삭제됨";}
    }
    void createDemo(){
        // UI smoke data is isolated from the real profile, and explicitly labelled synthetic.
        auto image=cv::Mat(720,1000,CV_8UC3,cv::Scalar(220,220,220));int unit=65;
        for(int y=0;y<7;++y)for(int x=0;x<10;++x)cv::rectangle(image,{150+x*unit,110+y*unit,unit,unit},(x+y)%2?cv::Scalar(255,255,255):cv::Scalar(0,0,0),cv::FILLED);
        detected=detectBoard(image,board);detected.source="SYNTHETIC UI TEST";message="합성 테스트 화면 · 실제 카메라 데이터가 아닙니다.";initialGuide=false;showCalibration=true;
        for(int i=0;i<5;++i){
            cv::Mat shifted;cv::Mat transform=(cv::Mat_<double>(2,3)<<1,0,i*12,0,1,-i*10);
            cv::warpAffine(image,shifted,transform,image.size(),cv::INTER_LINEAR,cv::BORDER_CONSTANT,cv::Scalar(220,220,220));
            auto sample=detectBoard(shifted,board);sample.source="SYNTHETIC UI TEST "+std::to_string(i+1);samples.push_back(std::move(sample));
        }
    }
};
static void style(){
    ImGui::StyleColorsDark();auto& s=ImGui::GetStyle();s.WindowRounding=7;s.FrameRounding=5;s.GrabRounding=4;s.WindowPadding={16,14};s.FramePadding={9,6};s.ItemSpacing={9,9};
    s.Colors[ImGuiCol_WindowBg]={.065f,.085f,.11f,1};s.Colors[ImGuiCol_Header]={.12f,.28f,.32f,1};
    s.Colors[ImGuiCol_Button]={.10f,.33f,.37f,1};s.Colors[ImGuiCol_ButtonHovered]={.13f,.46f,.48f,1};
    s.Colors[ImGuiCol_CheckMark]={.27f,.87f,.76f,1};
}
static int runApplication(int argc,char** argv){
    if(argc>1&&std::string(argv[1])=="--data-directory"){std::cout<<utf8(dataDirectory(false))<<'\n';return 0;}
    if(argc>1&&std::string(argv[1])=="--list-devices"){
        try{for(auto& d:Camera::enumerate())std::cout<<d.serial<<" | "<<d.model<<"\n";return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    bool hiddenStereo=argc>1&&std::string(argv[1])=="--smoke-stereo-hidden";
    bool hiddenRectangle=argc>1&&std::string(argv[1])=="--smoke-rectangle-hidden";
    bool hiddenOrigin=hiddenRectangle||(argc>1&&std::string(argv[1])=="--smoke-origin-hidden");
    bool savedScene=argc>1&&std::string(argv[1])=="--smoke-scene-saved-hidden";
    bool hiddenScene=savedScene||(argc>1&&std::string(argv[1])=="--smoke-scene-hidden");
    bool hiddenDevices=argc>1&&std::string(argv[1])=="--smoke-devices-hidden";
    bool hiddenMono=argc>1&&std::string(argv[1])=="--smoke-mono-hidden";
    bool hiddenVideo=argc>1&&std::string(argv[1])=="--smoke-video-hidden";
    bool hiddenProcessing=argc>1&&std::string(argv[1])=="--smoke-processing-hidden";
    bool hiddenTriangulation=argc>1&&std::string(argv[1])=="--smoke-triangulation-hidden";
    bool smoke=hiddenTriangulation||hiddenProcessing||hiddenVideo||hiddenMono||hiddenDevices||hiddenStereo||hiddenOrigin||hiddenScene||(argc>1&&std::string(argv[1])=="--smoke");
    if(argc>1&&std::string(argv[1]).starts_with("--smoke")&&!smoke)throw std::runtime_error("Unknown smoke mode; camera/profile startup was not run.");
    UiLayout layout;bool hasLayout=false,layoutWritable=true;std::string layoutWarning;
    if(!smoke)try{
        auto path=dataDirectory(false)/"ui-layout.json";hasLayout=fs::exists(path);layout=loadUiLayout(path);
    }catch(const std::exception& e){hasLayout=false;layoutWritable=false;layoutWarning=std::string("저장된 화면 배치를 읽지 못해 기본 배치로 시작합니다. 원본 파일은 유지됩니다: ")+e.what();}
    if(!glfwInit())return 1;
    std::vector<WindowRect> workAreas;int monitorCount=0;auto monitors=glfwGetMonitors(&monitorCount);
    for(int i=0;i<monitorCount;++i){WindowRect area;glfwGetMonitorWorkarea(monitors[i],&area.x,&area.y,&area.width,&area.height);if(area.width>0&&area.height>0)workAreas.push_back(area);}
    if(hasLayout)layout.mainWindow=fitToWorkArea(layout.mainWindow,workAreas);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    if(hiddenTriangulation||hiddenProcessing||hiddenVideo||hiddenMono||hiddenDevices||hiddenStereo||hiddenOrigin||hiddenScene)glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    auto window=glfwCreateWindow(layout.mainWindow.width,layout.mainWindow.height,"Boccia | Stereo Calibration",nullptr,nullptr);if(!window){glfwTerminate();return 2;}
    if(hasLayout){glfwSetWindowPos(window,layout.mainWindow.x,layout.mainWindow.y);if(layout.maximized)glfwMaximizeWindow(window);}
    glfwMakeContextCurrent(window);glfwSwapInterval(1);if(!gladLoadGL((GLADloadfunc)glfwGetProcAddress))return 3;
    IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();io.ConfigFlags|=ImGuiConfigFlags_DockingEnable|ImGuiConfigFlags_ViewportsEnable;
    if(hiddenTriangulation||hiddenProcessing||hiddenVideo||hiddenMono||hiddenDevices||hiddenStereo||hiddenOrigin||hiddenScene)io.ConfigFlags&=~ImGuiConfigFlags_ViewportsEnable;
    // Store ImGui's complete docking/window state in our UTF-8 JSON, alongside
    // native window geometry and panel visibility, independent of calibration saves.
    io.IniFilename=nullptr;io.ConfigViewportsNoAutoMerge=false;
    if(hasLayout&&!layout.imgui.empty())ImGui::LoadIniSettingsFromMemory(layout.imgui.data(),layout.imgui.size());
    io.ConfigViewportsNoDefaultParent=true;
    if(fs::exists("C:/Windows/Fonts/malgun.ttf"))io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/malgun.ttf",18,nullptr,io.Fonts->GetGlyphRangesKorean());
    style();ImGui_ImplGlfw_InitForOpenGL(window,true);ImGui_ImplOpenGL3_Init("#version 330");
    {Application app(smoke,window,std::move(layout),hasLayout,layoutWritable);app.layoutWarning=layoutWarning;int frame=0;
        if(hiddenDevices)app.prepareDeviceSmoke();
        if(hiddenMono)app.prepareMonoSmoke();
        if(hiddenVideo)app.prepareVideoSmoke();
        if(hiddenProcessing)app.prepareProcessingSmoke();
        if(hiddenTriangulation)app.prepareTriangulationSmoke();
        auto monoSmokeStarted=Clock::now();bool monoDone=false,stereoDone=false,processingDone=false;
        if(hiddenStereo)app.prepareStereoWorkflowSmoke();
        if(hiddenRectangle)app.prepareRectangleOriginSmoke();else if(hiddenOrigin)app.prepareOriginSmoke();
        if(savedScene)app.prepareSavedSceneSmoke();else if(hiddenScene)app.prepareSceneSmoke();
        while(!glfwWindowShouldClose(window)){
            glfwPollEvents();ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();
            if(hiddenScene&&!savedScene)app.sceneInputSmoke(frame);
            if(hiddenDevices)app.deviceInputSmoke(frame);
            if(hiddenMono)app.monoInputSmoke(frame);
            if(hiddenVideo)app.videoInputSmoke(frame);
            if(hiddenTriangulation)app.triangulationInputSmoke(frame);
            if(hiddenProcessing)processingDone=app.processingInputSmoke();
            if(hiddenStereo)app.stereoInputSmoke(frame);
            if(hiddenRectangle)app.rectangleOriginInputSmoke(frame);
            ImGui::NewFrame();app.draw();ImGui::Render();
            int w,h;glfwGetFramebufferSize(window,&w,&h);glViewport(0,0,w,h);glClearColor(.04f,.055f,.07f,1);glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if(hiddenStereo&&app.stereoSmokeCollected&&frame==app.stereoSmokeResumeFrame+12){
                cv::Mat pixels(h,w,CV_8UC3);glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,w,h,GL_BGR,GL_UNSIGNED_BYTE,pixels.data);cv::flip(pixels,pixels,0);
                cv::imwrite(utf8(dataDirectory(true)/"ui-stereo-grid-smoke.png"),pixels);
            }
            if(hiddenTriangulation&&(frame==26||frame==60)){cv::Mat pixels(h,w,CV_8UC3);glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,w,h,GL_BGR,GL_UNSIGNED_BYTE,pixels.data);cv::flip(pixels,pixels,0);
                fs::create_directories(dataDirectory(true));cv::imwrite(utf8(dataDirectory(true)/(frame==26?"ui-triangulation-views-smoke.png":"ui-triangulation-smoke.png")),pixels);}
            monoDone=hiddenMono&&frame>35&&app.monoSmokePassed();
            stereoDone=hiddenStereo&&frame>30&&app.stereoSmokePassed();
            if(hiddenProcessing&&(app.processingSmokeStep==7||app.processingSmokeStep==8||processingDone)){cv::Mat pixels(h,w,CV_8UC3);glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,w,h,GL_BGR,GL_UNSIGNED_BYTE,pixels.data);cv::flip(pixels,pixels,0);cv::imwrite(utf8(dataDirectory(true)/(processingDone?"ui-processing-complete-smoke.png":app.processingSmokeStep==8?"ui-processing-roi-smoke.png":"ui-processing-smoke.png")),pixels);}
            if(smoke && ((hiddenRectangle&&frame==14)||(hiddenStereo&&(frame==8||frame==18||stereoDone))||(hiddenVideo&&frame==10)||(!hiddenMono&&frame==40)||(hiddenDevices&&(frame==10||frame==20||frame==54))||(hiddenMono&&(frame==10||frame==18||monoDone)))){cv::Mat pixels(h,w,CV_8UC3);glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,w,h,GL_BGR,GL_UNSIGNED_BYTE,pixels.data);cv::flip(pixels,pixels,0);
                fs::create_directories(dataDirectory(true));cv::imwrite(utf8(dataDirectory(true)/(hiddenVideo?(frame==10?"ui-video-smoke.png":"ui-comparison-smoke.png"):hiddenMono?(monoDone?"ui-mono-result-smoke.png":frame==10?"ui-mono-live-smoke.png":"ui-mono-collect-smoke.png"):hiddenDevices?(frame==10?"ui-devices-smoke.png":frame==20?"ui-devices-cal-smoke.png":frame==54?"ui-devices-world-smoke.png":"ui-devices-pose-smoke.png"):savedScene?"ui-scene-saved-smoke.png":hiddenScene?"ui-scene-smoke.png":hiddenOrigin?(hiddenRectangle?(frame==14?"ui-origin-rectangle-pick-smoke.png":"ui-origin-rectangle-smoke.png"):"ui-origin-smoke.png"):hiddenStereo?(stereoDone?"ui-stereo-result-smoke.png":frame==8?"ui-stereo-live-smoke.png":"ui-stereo-smoke.png"):"ui-smoke.png")),pixels);
                if(hiddenScene&&(!app.sceneSmokePassed()||glGetError()!=GL_NO_ERROR))throw std::runtime_error("3D hidden rendering failed.");}
            if(io.ConfigFlags&ImGuiConfigFlags_ViewportsEnable){auto context=glfwGetCurrentContext();ImGui::UpdatePlatformWindows();ImGui::RenderPlatformWindowsDefault();glfwMakeContextCurrent(context);}
            glfwSwapBuffers(window);app.saveLayout();++frame;
            if(hiddenMono){if(monoDone){std::cout<<"PASS: mono live preview, automatic collection-to-solve, cancel/resume, before-after and raw preservation\n";glfwSetWindowShouldClose(window,true);}
                else if(Clock::now()-monoSmokeStarted>std::chrono::seconds(15))throw std::runtime_error("Mono UI smoke timed out.");}
            else if(hiddenStereo){if(stereoDone){std::cout<<"PASS: stereo live preview, automatic collection-to-solve, cancel/resume and raw preservation\n";glfwSetWindowShouldClose(window,true);}
                else if(Clock::now()-monoSmokeStarted>std::chrono::seconds(15))throw std::runtime_error("Stereo UI smoke timed out.");}
            else if(hiddenProcessing){if(processingDone)glfwSetWindowShouldClose(window,true);else if(Clock::now()-monoSmokeStarted>std::chrono::seconds(30))throw std::runtime_error("Processing UI smoke timed out at "+std::to_string(app.processingSmokeStep));}
            else if(smoke&&frame>(hiddenTriangulation?66:hiddenVideo?62:55))glfwSetWindowShouldClose(window,true);
        }
        app.saveLayout(true);
        app.releaseTextures();
    }
    ImGui_ImplOpenGL3_Shutdown();ImGui_ImplGlfw_Shutdown();ImGui::DestroyContext();glfwDestroyWindow(window);glfwTerminate();return 0;
}

int main(int argc,char** argv){
    try{return runApplication(argc,argv);}
    catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
