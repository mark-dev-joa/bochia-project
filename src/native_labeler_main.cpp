#include "native_labeler_app.hpp"
#include "native_labeler_theme.hpp"
#include "labeler_selection.hpp"
#include "labeler_util.hpp"
#include "model_paths.hpp"
#include "yolo_detector.hpp"
#include "labeler_assist.hpp"
#include "labeler_log.hpp"
#include "build_info.hpp"
#include <chrono>
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <fstream>

using namespace boccia::labeler;
// boccia_labeler.exe --version prints the build time and git hash: into a pipe when there is one, else onto the console it was started from.
static void sayVersion(){
    if(!GetStdHandle(STD_OUTPUT_HANDLE)&&AttachConsole(ATTACH_PARENT_PROCESS)){FILE* out{};freopen_s(&out,"CONOUT$","w",stdout);}
    std::cout<<boccia::versionString()<<"\n"<<std::flush;}
int main() {
    int argc{}; auto args=CommandLineToArgvW(GetCommandLineW(),&argc);
    for(int i=1;i<argc;++i)if(std::wstring(args[i])==L"--version"){sayVersion();LocalFree(args);return 0;}
    Configuration config; auto root=fromUtf8(BOCCIA_LABELER_ROOT);
    // The engine code stays in the repository; the labels and exports live in data/ beside this program.
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);const auto home=std::filesystem::path(module).parent_path()/"data";
    std::filesystem::path workspace, createProjectAt, switchTest, importUiTest, createUiTest;std::vector<std::filesystem::path> importFiles;std::string projectName="New Project";bool importPreview=false,projectInfo=false,explicitProject=false,importUiPreview=false,closeUiTest=false,importUiCancel=false;int captureFrames=6;bool explicitSource=false;
    bool deviceCheck=false,logWanted=false,applyPrevious=false;std::wstring hubAddress,hubWorker,hubToken;int hubWait=0;std::filesystem::path recentForCapture;std::wstring importState,batchDrawer,exportState,circlesState;bool settingsCapture=false;
    config.source=root/"data/imports";const auto nativeProject=std::filesystem::absolute(home/"labels/project.json");config.project=nativeProject;config.exports=home/"exports";config.root=root;
    // The EfficientSAM files: beside the program, in the models bin/release shares, or the repository's copy.
    config.models=boccia::findModel("efficientsam",home.parent_path(),root/"data");if(config.models.empty())config.models=root/"data/models/efficientsam";
    yoloProgramFolders(home.parent_path(),root/"data");   // the CUDA runtime for drafts on the GPU: beside the program or the release's cuda (model_paths.hpp)
    bool hidden=false,selfTest=false,stageTest=false,fold=false,shortcuts=false,buildSelectionOnly=false,checkOldEngine=false;std::filesystem::path capture,steps;int stepFrames=-1,stepsDone=0,missingTrial=0,missingApply=0,occludeTrial=0,occludeApply=0,timeoutSeconds=50;bool missingScript=false,propagate=false;std::wstring errorText,batchSplit;std::wstring mode=L"overview";int recording=5,frame=0,requestedWidth=1440,requestedHeight=850,selectCamera=-1,selectRank=-1,checkStep=0,display=-1;bool propose=false,prompt=false;
    for(int i=1;i<argc;++i){std::wstring argument=args[i];
        auto value=[&](){if(i+1>=argc)throw std::runtime_error("Missing option value");return std::wstring(args[++i]);};
        if(argument==L"--source"){config.source=value();explicitSource=true;}else if(argument==L"--project"){config.project=value();explicitProject=true;}else if(argument==L"--export-root")config.exports=value();
        else if(argument==L"--workspace")workspace=value();else if(argument==L"--create-project")createProjectAt=value();else if(argument==L"--project-name")projectName=utf8(std::filesystem::path(value()));else if(argument==L"--import")importFiles.emplace_back(value());else if(argument==L"--import-preview")importPreview=true;else if(argument==L"--project-info")projectInfo=true;else if(argument==L"--project-switch-test")switchTest=value();
        else if(argument==L"--import-ui-test")importUiTest=value();else if(argument==L"--import-ui-preview")importUiPreview=true;else if(argument==L"--import-ui-cancel")importUiCancel=true;
        else if(argument==L"--create-ui-test")createUiTest=value();else if(argument==L"--close-ui-test")closeUiTest=true;else if(argument==L"--capture-after-frames")captureFrames=std::clamp(std::stoi(value()),6,180);
        else if(argument==L"--recent")recentForCapture=value();else if(argument==L"--import-state")importState=value();else if(argument==L"--batch-drawer")batchDrawer=value();else if(argument==L"--circles")circlesState=value();else if(argument==L"--export-state")exportState=value();else if(argument==L"--project-settings")settingsCapture=true;
        else if(argument==L"--smoke-hidden")hidden=true;else if(argument==L"--build-selection")buildSelectionOnly=true;else if(argument==L"--check-old-engine")checkOldEngine=true;
        else if(argument==L"--capture")capture=value();else if(argument==L"--capture-steps")steps=value();else if(argument==L"--view")mode=value();else if(argument==L"--recording")recording=std::stoi(value());else if(argument==L"--frame")frame=std::stoi(value());
        else if(argument==L"--display")display=std::stoi(value());else if(argument==L"--propose")propose=true;else if(argument==L"--prompt")prompt=true;else if(argument==L"--missing-trial")missingTrial=std::stoi(value());else if(argument==L"--missing-apply")missingApply=std::stoi(value());else if(argument==L"--missing-script")missingScript=true;else if(argument==L"--propagate")propagate=true;else if(argument==L"--error")errorText=value();else if(argument==L"--timeout")timeoutSeconds=std::stoi(value());else if(argument==L"--batch-split")batchSplit=value();else if(argument==L"--occlude-trial")occludeTrial=std::stoi(value());else if(argument==L"--occlude-apply")occludeApply=std::stoi(value());else if(argument==L"--width")requestedWidth=std::stoi(value());else if(argument==L"--height")requestedHeight=std::stoi(value());
        else if(argument==L"--select"){auto text=value();auto colon=text.find(L':');selectCamera=std::stoi(text.substr(0,colon));selectRank=std::stoi(text.substr(colon+1));}else if(argument==L"--fold")fold=true;else if(argument==L"--shortcuts")shortcuts=true;else if(argument==L"--self-test")selfTest=true;else if(argument==L"--device")setModelsOnGpu(value()==L"gpu");else if(argument==L"--device-check")deviceCheck=true;else if(argument==L"--apply-previous")applyPrevious=true;else if(argument==L"--log")logWanted=true;else if(argument==L"--hub"){hubAddress=value();logWanted=true;}else if(argument==L"--worker")hubWorker=value();else if(argument==L"--token")hubToken=value();else if(argument==L"--hub-wait")hubWait=std::stoi(value());else if(argument==L"--stage-test")stageTest=true;
    }LocalFree(args);config.rememberUi=!hidden;
    if(!hidden||logWanted){openLog(home.parent_path()/"logs");   // a hidden check writes one only with --log
       const auto runtime=yoloCudaRuntime();
        logLine("시작 · "+utf8(home.parent_path())+" · CUDA "+(runtime.empty()?std::string("없음"):utf8(runtime))+" · 버전 "+boccia::versionString());}
    if(deviceCheck){   // where SAM and YOLO run on this PC and how fast, printed for a remote check: boccia_labeler.exe --device-check | Out-Host
        setModelsOnGpu(true);Json report={{"program",utf8(home.parent_path())},{"cuda_runtime",utf8(yoloCudaRuntime())},
            {"cuda_provider_dll",std::filesystem::is_regular_file(home.parent_path()/"onnxruntime_providers_cuda.dll")}};
        using Clock=std::chrono::steady_clock;const auto ms=[](Clock::time_point from){return std::chrono::duration<double,std::milli>(Clock::now()-from).count();};
        const cv::Mat image(960,1280,CV_8UC3,cv::Scalar(90,90,90));
        try{BallAssist probe(config.models);auto started=Clock::now();
            probe.segment(image,"probe-0",Json::array({{{"x",640},{"y",480},{"label",1}}}),20);report["sam_first_ms"]=ms(started);started=Clock::now();
            for(int i=1;i<=5;++i)probe.segment(image,"probe-"+std::to_string(i),Json::array({{{"x",600+10*i},{"y",480},{"label",1}}}),20);
            report["sam_ms"]=ms(started)/5;report["sam"]=probe.status().value("device","?");}
        catch(const std::exception& error){report["sam_error"]=error.what();}
        try{const auto model=boccia::findModel("boccia/yolo26n-20261012-1280x960-u8.onnx",home.parent_path(),root/"data");
            if(model.empty())throw std::runtime_error("yolo26n-20261012-1280x960-u8.onnx 없음");
            auto started=Clock::now();const auto detector=yoloDetectorFor(model,true);report["yolo_first_ms"]=ms(started);started=Clock::now();
            for(int i=0;i<5;++i)detector->detect(image,.25f);report["yolo_ms"]=ms(started)/5;report["yolo"]=detector->provider();}
        catch(const std::exception& error){report["yolo_error"]=error.what();}
        report["gpu_problem"]=gpuProblem();std::cout<<report.dump(2)<<std::endl;return 0;
    }
    try {
        if(!createProjectAt.empty()){createWorkspace(createProjectAt,projectName);workspace=createProjectAt;}
        if(!workspace.empty()){
            workspace=workspaceRoot(workspace);readWorkspace(workspace);config.source=workspace/"source";config.project=workspace/"labels/project.json";config.exports=workspace/"exports";explicitSource=true;
            if(!importFiles.empty()){
                const auto lease=acquireLabelLease(config.project);const auto plan=planImport(workspace,importFiles);const auto result=importPreview?plan.summary():commitImport(plan);
                std::cout<<result.dump()<<"\n";return 0;
            }
            if(projectInfo||!createProjectAt.empty()){Catalog catalog(config.source);std::cout<<Json{{"project",readWorkspace(workspace)},{"recordings",catalog.metadata()},{"scenes",sceneIndex(catalog)},{"images",catalog.totalFrames()}}.dump()<<"\n";return 0;}
        } else if(!importFiles.empty()||projectInfo)throw std::runtime_error("--workspace 프로젝트 폴더를 지정하세요.");
    }catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 4;}
    config.startScreen=!explicitSource&&!explicitProject;
    config.training=hidden&&(stageTest||!batchSplit.empty()||mode==L"split"||mode==L"export");
    // The engine and the screens get the same full paths with one kind of separator.
    for(auto* path:{&config.source,&config.project,&config.exports,&config.models})*path=std::filesystem::absolute(*path);
    // Builds before 2026-10-08 left a Python engine on port 8767 that holds the same labels in memory. Two writers would
    // overwrite each other, so the app stops here until that engine is gone. Hidden checks run on copies and skip this.
    if(checkOldEngine||(!hidden&&!config.startScreen&&workspace.empty())){
        if(localPortListening(8767)){
            const wchar_t* text=L"이전 빌드가 띄운 Python 라벨 엔진이 포트 8767에 아직 떠 있습니다.\n이 빌드는 엔진 없이 라벨 파일을 직접 열기 때문에, 그 엔진을 먼저 종료해야 합니다.\n\nPowerShell:\nGet-NetTCPConnection -LocalPort 8767 -State Listen | ForEach-Object { Stop-Process -Id $_.OwningProcess }";
            if(checkOldEngine)std::cout<<"OLD ENGINE on 8767\n";else MessageBoxW(nullptr,text,L"Boccia Labeler",MB_OK|MB_ICONWARNING);
            return 5;
        }
        if(checkOldEngine){std::cout<<"no old engine\n";return 0;}
    }
    // First run in this place: bring the labels over once. The native project worked on before this layout comes
    // first, then the web labeler's. Each program keeps its own store, so their engines never overwrite each other.
    try {if(!config.startScreen&&config.project==nativeProject&&!std::filesystem::exists(nativeProject)){
        for(const auto& original:{root/"output/native-labeler/project.json",root/"output/ball-labeler/project.json"}){
            if(!std::filesystem::exists(original))continue;
            std::filesystem::create_directories(nativeProject.parent_path());
            for(const char* suffix:{"-selection.json","-auto-status.json","-native-ui.json"}){
                const auto from=original.parent_path()/(original.stem().string()+suffix);
                if(std::filesystem::exists(from))std::filesystem::copy_file(from,nativeProject.parent_path()/(nativeProject.stem().string()+suffix),std::filesystem::copy_options::skip_existing);
            }
            std::filesystem::copy_file(original,nativeProject,std::filesystem::copy_options::skip_existing);
            std::cerr<<"라벨을 가져왔습니다: "<<original.string()<<"\n";break;
        }
    }}catch(const std::exception& error){std::cerr<<"기존 라벨 가져오기 실패: "<<error.what()<<"\n";return 4;}
    // Without a window: picks the representative frames of the project again and writes the sidecar, like tools/filter_labeler_frames.py.
    if(buildSelectionOnly){try{
        const auto started=std::chrono::steady_clock::now();Catalog catalog(config.source);Project project(catalog,config.project,config.exports);
        Json result=buildSelection(catalog,project.state,[](int done,int total,const std::string& camera){if(done%200==0)std::cout<<done<<"/"<<total<<" "<<camera<<"\n";});
        result["elapsed_seconds"]=std::round(std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()*100)/100;
        Json reasons=Json::object();for(const auto& [key,entry]:result["frames"].items()){const std::string reason=entry["reason"];reasons[reason]=reasons.value(reason,0)+1;}
        result["reasons"]=reasons;const auto output=selectionPath(project.path);atomicJson(output,result,result["frames"].size()<128);
        std::cout<<Json{{"path",utf8(output)},{"total",result["total"]},{"selected",result["selected"]},{"excluded",result["excluded"]},{"reasons",reasons},{"elapsed_seconds",result["elapsed_seconds"]}}.dump()<<"\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 4;}}
    // A native window owns one project file. Workers each receive a separate project.
    std::shared_ptr<void> projectLease;
    if(!hidden&&!config.startScreen){
        std::filesystem::create_directories(config.project.parent_path());
        const auto leasePath=std::filesystem::path(config.project.native()+L".lock");
        HANDLE lease=CreateFileW(leasePath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(lease==INVALID_HANDLE_VALUE){
            MessageBoxW(nullptr,L"이 라벨 파일을 다른 라벨러가 사용 중이거나 저장 폴더에 쓸 수 없습니다.",L"Boccia Labeler",MB_OK|MB_ICONWARNING);
            return 5;
        }
        projectLease=std::shared_ptr<void>(lease,[](void* handle){CloseHandle(handle);});
    }
    if(!glfwInit())return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    if(hidden)glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    auto window=glfwCreateWindow(requestedWidth,requestedHeight,"Boccia Labeler",nullptr,nullptr);if(!window){glfwTerminate();return 2;}glfwSetWindowSizeLimits(window,1280,720,GLFW_DONT_CARE,GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);glfwSwapInterval(1);if(!gladLoadGL((GLADloadfunc)glfwGetProcAddress)){glfwDestroyWindow(window);glfwTerminate();return 3;}
    IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
    theme::loadFonts();theme::apply();
    ImGui_ImplGlfw_InitForOpenGL(window,true);ImGui_ImplOpenGL3_Init("#version 330");int result=0;
    config.lease=std::move(projectLease);
    try {Application application(std::move(config));if(applyPrevious)application.applyPreviousForCapture(true);if(!hubAddress.empty())application.hubForCheck(utf8(std::filesystem::path(hubAddress)),utf8(std::filesystem::path(hubWorker)),utf8(std::filesystem::path(hubToken)),hubWait);int stage=0,settled=0,switchStep=0,importUiStep=0,createUiStep=0;bool closing=false,stopAndClose=false,stoppingExport=false;auto start=std::chrono::steady_clock::now();
        if(!recentForCapture.empty())application.recentForCapture(recentForCapture);
        glfwSetWindowUserPointer(window,&application);glfwSetDropCallback(window,[](GLFWwindow* target,int count,const char** files){std::vector<std::filesystem::path> paths;for(int i=0;i<count;++i)paths.push_back(fromUtf8(files[i]));static_cast<Application*>(glfwGetWindowUserPointer(target))->droppedFiles(std::move(paths));});
        while(!glfwWindowShouldClose(window)){
            glfwPollEvents();application.tick();if(!hidden&&glfwWindowShouldClose(window)&&!application.canClose()){glfwSetWindowShouldClose(window,false);closing=true;}
            if(hidden&&!createUiTest.empty()&&application.idle()){
                if(createUiStep==0){application.showNewProjectForCapture(createUiTest.parent_path(),utf8(createUiTest.filename()));createUiStep=1;}
                else if(createUiStep==1){application.createProjectForCapture();createUiStep=2;}
                else if(application.projectOpened()){std::cout<<"PASS: project created at selected location and opened before workflow menus\n";createUiTest.clear();stage=2;}
            }
            if(hidden&&closeUiTest&&application.projectOpened()&&application.idle()){application.closeProjectForCapture();if(!application.projectOpened()){closeUiTest=false;stage=2;}}
            if(hidden&&importUiCancel&&application.importDraftingForTest()){application.cancelImport();importUiCancel=false;std::cout<<"cancelled the import during its drafts"<<std::endl;}
            if(hidden&&!importUiTest.empty()&&application.idle()){
                if(importUiStep++==0)application.droppedFiles({importUiTest});
                else {if(!importUiPreview)application.commitPlannedImport();std::cout<<"PASS: dropped folder preflight"<<(importUiPreview?" preview":" and copy import")<<"\n";importUiTest.clear();stage=2;}
            }
            if(hidden&&!switchTest.empty()&&application.ready()&&application.idle()){
                application.projectUiCheck(switchStep++,workspace,switchTest);
                if(switchStep>=3){std::cout<<"PASS: save before switching; independent project leases; previous project restored\n";switchTest.clear();stage=2;}
            }
            if(hidden&&createUiTest.empty()&&importUiTest.empty()&&(mode==L"prepare"||mode==L"data"||mode==L"welcome"||mode==L"new-project"||mode==L"worker-dialog")&&!application.busy()&&stage==0){application.showPage(0);if(mode==L"new-project")application.showNewProjectForCapture();if(mode==L"worker-dialog")application.workerDialogForCapture();if(!importState.empty())application.importStateForCapture(utf8(std::filesystem::path(importState)));if(settingsCapture)application.projectSettingsForCapture();stage=2;}
            if(hidden&&createUiTest.empty()&&importUiTest.empty()&&switchTest.empty()&&application.ready()&&!application.busy()){
                if(stage==0){application.selectSceneForCapture(recording,frame);stage=1;}
                else if(stage==1){
                    if(mode==L"editor"||mode==L"focus"){if(selectCamera>=0)application.selectForCapture(selectCamera,selectRank);else application.showEditor(0);}
                    if(fold)application.foldForCapture();if(shortcuts)application.showShortcutsForCapture();if(display>=0)application.showViewForCapture(display);if(propose)application.proposeForCapture();if(prompt)application.confirmForCapture();if(missingTrial>0)application.missingTrialForCapture(missingTrial);if(missingApply>0)application.missingApplyForCapture(missingApply);if(propagate)application.propagateForCapture();if(!errorText.empty())application.showErrorForCapture(utf8(errorText));if(occludeTrial>0)application.occlusionTrialForCapture(occludeTrial);if(occludeApply>0)application.occlusionApplyForCapture(occludeApply);
                    if(mode==L"prepare"||mode==L"data")application.showPage(0);else if(mode==L"split")application.showPage(3);else if(mode==L"export")application.showPage(4);else if(mode==L"exports")application.showPage(2);else application.showPage(1);
                    if(!exportState.empty())application.exportStateForCapture(utf8(std::filesystem::path(exportState)));
                    if(!batchDrawer.empty())application.batchDrawerForCapture(utf8(std::filesystem::path(batchDrawer)));
                    if(!circlesState.empty())application.circlesForCapture(utf8(std::filesystem::path(circlesState)));
                    stage=2;}
                else if(stage==2&&selfTest&&application.idle()){if(!application.selfCheckStep(checkStep++)){std::cout<<"PASS: select, zoom, save before leaving a scene, one-pixel edge correction, delete and undo, outline target, Tab key, scene confirm and the next scene fetched ahead\n";selfTest=false;}}
                else if(stage==2&&stageTest&&application.idle()){if(!application.stageCheckStep(checkStep++)){std::cout<<"PASS: split plan, grouping, confirm, export and export history\n";stageTest=false;}}
                else if(stage==2&&missingScript&&application.settled()){if(!application.missingScriptStep(checkStep++)){std::cout<<"PASS: missing-ball trials, their assembly and the apply of one\n";missingScript=false;}}
                else if(stage==2&&!batchSplit.empty()&&application.idle()){if(!application.batchSplitStep(checkStep++,utf8(batchSplit))){std::cout<<"PASS: batch split and export\n";batchSplit.clear();}}
            }
            ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();application.draw();
            // Closing while a save or a job runs: keep working, or keep the changes in recovery/ and close. During an import
            // or an export the choice is to stop it: the import is rolled back, the unfinished export folder removed, then the
            // window closes.
            if(closing&&!ImGui::IsPopupOpen("종료 전 작업 보관"))ImGui::OpenPopup("종료 전 작업 보관");
            if(stopAndClose&&!application.importing()&&!application.exporting()&&!application.busy()){
                if(stoppingExport&&!application.canClose())application.preserveRecovery();   // edits made while the export ran
                glfwSetWindowShouldClose(window,true);stopAndClose=closing=false;}
            {bool dismiss=false;
             if(theme::beginDialog("종료 전 작업 보관","종료 전 작업 보관",480,&dismiss)){
                 const bool exporting=stoppingExport||(application.exporting()&&!application.importing()),importing=!exporting&&(application.importing()||stopAndClose);
                 ImGui::TextUnformatted(importing?"임포트 중입니다.":exporting?"내보내는 중입니다.":"저장이나 처리가 끝나지 않았습니다.");
                 theme::dialogFooter(stopAndClose?"중단하는 중":"");
                 if(importing||exporting){if(theme::footerButton("중단하고 종료",nullptr,true,!stopAndClose)){if(exporting){application.stopRunningExport();stoppingExport=true;}else application.cancelImport();stopAndClose=true;}}
                 else if(theme::footerButton("변경 보관하고 종료",nullptr,true,!application.busy())){application.preserveRecovery();glfwSetWindowShouldClose(window,true);closing=false;ImGui::CloseCurrentPopup();}
                 if(!stopAndClose&&(theme::footerButton("계속 작업","Esc",false)||dismiss)){closing=false;ImGui::CloseCurrentPopup();}
                 theme::endDialog();
             }}
            ImGui::Render();
            int width,height;glfwGetFramebufferSize(window,&width,&height);glViewport(0,0,width,height);glClearColor(.051f,.059f,.071f,1);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if(hidden&&stage==2&&!selfTest&&!stageTest&&!missingScript&&batchSplit.empty()&&application.settled()&&(application.canClose()||propose)&&stepFrames<0&&application.hubSettled()&&++settled>=captureFrames){if(!steps.empty()){application.moveOnForCapture();stepFrames=0;}else{if(!capture.empty()){std::filesystem::create_directories(capture.parent_path());cv::Mat pixels(height,width,CV_8UC3);glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,width,height,GL_BGR,GL_UNSIGNED_BYTE,pixels.data);cv::flip(pixels,pixels,0);auto name=capture.u8string();if(!cv::imwrite(std::string(name.begin(),name.end()),pixels))throw std::runtime_error("Screenshot failed");}
                if(mode==L"new-project"){const auto size=application.creationPopupSizeForCapture();std::cout<<"NEW_PROJECT_POPUP "<<size.x<<" "<<size.y<<"\n";}
                if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("OpenGL smoke failed");std::cout<<"PASS: native UI loaded real scene; view rendered without GL errors\n";glfwSetWindowShouldClose(window,true);}}
            // --capture-steps: every frame while the scene changes, until the view has settled for ten frames.
            if(stepFrames>=0){std::filesystem::create_directories(steps);cv::Mat pixels(height,width,CV_8UC3);glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,width,height,GL_BGR,GL_UNSIGNED_BYTE,pixels.data);cv::flip(pixels,pixels,0);
                char name[16];std::snprintf(name,sizeof name,"%03d.png",stepFrames);auto file=(steps/name).u8string();cv::imwrite(std::string(file.begin(),file.end()),pixels);
                if(application.settled()&&application.canClose())++stepsDone;else stepsDone=0;
                if(++stepFrames>=300||stepsDone>=10){std::cout<<"PASS: captured "<<stepFrames<<" frames of the change of scene\n";glfwSetWindowShouldClose(window,true);}}
            if(hidden&&(!application.error().empty()||std::chrono::steady_clock::now()-start>std::chrono::seconds(timeoutSeconds)))throw std::runtime_error(application.error().empty()?"Native UI timed out":application.error());
            glfwSwapBuffers(window);
        }
    }catch(const std::exception& error){std::cerr<<error.what()<<"\n";result=4;}
    ImGui_ImplOpenGL3_Shutdown();ImGui_ImplGlfw_Shutdown();ImGui::DestroyContext();glfwDestroyWindow(window);glfwTerminate();return result;
}
