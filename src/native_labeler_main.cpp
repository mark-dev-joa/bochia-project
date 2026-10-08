#include "native_labeler_app.hpp"
#include "native_labeler_theme.hpp"
#include "labeler_selection.hpp"
#include "labeler_util.hpp"
#include <chrono>
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <windows.h>
#include <shellapi.h>
#include <iostream>

using namespace boccia::labeler;
int main() {
    int argc{}; auto args=CommandLineToArgvW(GetCommandLineW(),&argc);
    Configuration config; auto root=fromUtf8(BOCCIA_LABELER_ROOT);
    // The engine code stays in the repository; the labels and exports live in data/ beside this program.
    wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);const auto home=std::filesystem::path(module).parent_path()/"data";
    config.source=root/"data/imports";const auto nativeProject=std::filesystem::absolute(home/"labels/project.json");config.project=nativeProject;config.exports=home/"exports";config.root=root;
    // The EfficientSAM files ship beside the program; a build run from the repository finds the repository's copy.
    config.models=std::filesystem::exists(home.parent_path()/"models/efficientsam")?home.parent_path()/"models/efficientsam":root/"data/models/efficientsam";
    bool hidden=false,selfTest=false,stageTest=false,fold=false,shortcuts=false,buildSelectionOnly=false,checkOldEngine=false;std::filesystem::path capture;std::wstring mode=L"overview";int recording=5,frame=0,requestedWidth=1440,requestedHeight=850,selectCamera=-1,selectRank=-1,checkStep=0,display=-1;bool propose=false;
    for(int i=1;i<argc;++i){std::wstring argument=args[i];
        auto value=[&](){if(i+1>=argc)throw std::runtime_error("Missing option value");return std::wstring(args[++i]);};
        if(argument==L"--source")config.source=value();else if(argument==L"--project")config.project=value();else if(argument==L"--export-root")config.exports=value();
        else if(argument==L"--smoke-hidden")hidden=true;else if(argument==L"--build-selection")buildSelectionOnly=true;else if(argument==L"--check-old-engine")checkOldEngine=true;
        else if(argument==L"--capture")capture=value();else if(argument==L"--view")mode=value();else if(argument==L"--recording")recording=std::stoi(value());else if(argument==L"--frame")frame=std::stoi(value());
        else if(argument==L"--display")display=std::stoi(value());else if(argument==L"--propose")propose=true;else if(argument==L"--width")requestedWidth=std::stoi(value());else if(argument==L"--height")requestedHeight=std::stoi(value());
        else if(argument==L"--select"){auto text=value();auto colon=text.find(L':');selectCamera=std::stoi(text.substr(0,colon));selectRank=std::stoi(text.substr(colon+1));}else if(argument==L"--fold")fold=true;else if(argument==L"--shortcuts")shortcuts=true;else if(argument==L"--self-test")selfTest=true;else if(argument==L"--stage-test")stageTest=true;
    }LocalFree(args);config.rememberUi=!hidden;
    // The engine and the screens get the same full paths with one kind of separator.
    for(auto* path:{&config.source,&config.project,&config.exports,&config.models})*path=std::filesystem::absolute(*path);
    // Builds before 2026-10-08 left a Python engine on port 8767 that holds the same labels in memory. Two writers would
    // overwrite each other, so the app stops here until that engine is gone. Hidden checks run on copies and skip this.
    if(checkOldEngine||!hidden){
        if(localPortListening(8767)){
            const wchar_t* text=L"이전 빌드가 띄운 Python 라벨 엔진이 포트 8767에 아직 떠 있습니다.\n이 빌드는 엔진 없이 라벨 파일을 직접 열기 때문에, 그 엔진을 먼저 종료해야 합니다.\n\nPowerShell:\nGet-NetTCPConnection -LocalPort 8767 -State Listen | ForEach-Object { Stop-Process -Id $_.OwningProcess }";
            if(checkOldEngine)std::cout<<"OLD ENGINE on 8767\n";else MessageBoxW(nullptr,text,L"Boccia Labeler",MB_OK|MB_ICONWARNING);
            return 5;
        }
        if(checkOldEngine){std::cout<<"no old engine\n";return 0;}
    }
    // First run in this place: bring the labels over once. The native project worked on before this layout comes
    // first, then the web labeler's. Each program keeps its own store, so their engines never overwrite each other.
    try {if(config.project==nativeProject&&!std::filesystem::exists(nativeProject)){
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
    if(!glfwInit())return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    if(hidden)glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    auto window=glfwCreateWindow(requestedWidth,requestedHeight,"Boccia Lab - Native Labeler",nullptr,nullptr);if(!window){glfwTerminate();return 2;}glfwSetWindowSizeLimits(window,1280,720,GLFW_DONT_CARE,GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);glfwSwapInterval(1);if(!gladLoadGL((GLADloadfunc)glfwGetProcAddress)){glfwDestroyWindow(window);glfwTerminate();return 3;}
    IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
    theme::loadFonts();theme::apply();
    ImGui_ImplGlfw_InitForOpenGL(window,true);ImGui_ImplOpenGL3_Init("#version 330");int result=0;
    try {Application application(config);int stage=0,settled=0;bool closing=false;auto start=std::chrono::steady_clock::now();
        while(!glfwWindowShouldClose(window)){
            glfwPollEvents();application.tick();if(!hidden&&glfwWindowShouldClose(window)&&!application.canClose()){glfwSetWindowShouldClose(window,false);closing=true;}
            if(hidden&&application.ready()&&!application.busy()){
                if(stage==0){application.selectSceneForCapture(recording,frame);stage=1;}
                else if(stage==1){
                    if(mode==L"editor"||mode==L"focus"){if(selectCamera>=0)application.selectForCapture(selectCamera,selectRank);else application.showEditor(0);}
                    else if(mode==L"remove"){if(selectCamera>=0)application.askRemoveForCapture(selectCamera,selectRank);}
                    if(fold)application.foldForCapture();if(shortcuts)application.showShortcutsForCapture();if(display>=0)application.showViewForCapture(display);if(propose)application.proposeForCapture();
                    if(mode==L"prepare")application.showPage(0);else if(mode==L"split")application.showPage(2);else if(mode==L"export")application.showPage(3);
                    stage=2;}
                else if(stage==2&&selfTest&&application.idle()){if(!application.selfCheckStep(checkStep++)){std::cout<<"PASS: select, zoom, save before leaving a scene, one-pixel edge correction, delete and undo, outline target, Tab key, scene confirm and the next scene fetched ahead\n";selfTest=false;}}
                else if(stage==2&&stageTest&&application.idle()){if(!application.stageCheckStep(checkStep++)){std::cout<<"PASS: split plan, grouping, confirm, export and export history\n";stageTest=false;}}
            }
            ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();application.draw();
            if(closing){ImGui::OpenPopup("종료 전 작업 보관");if(ImGui::BeginPopupModal("종료 전 작업 보관",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){ImGui::TextUnformatted("저장 또는 처리가 끝나지 않았습니다.");ImGui::TextUnformatted("작업을 계속하거나 변경을 recovery 폴더에 보관하고 종료할 수 있습니다.");if(ImGui::Button("계속 작업")){closing=false;ImGui::CloseCurrentPopup();}ImGui::SameLine();ImGui::BeginDisabled(application.busy());if(ImGui::Button("변경 보관하고 종료")){application.preserveRecovery();glfwSetWindowShouldClose(window,true);closing=false;ImGui::CloseCurrentPopup();}ImGui::EndDisabled();ImGui::EndPopup();}}
            ImGui::Render();
            int width,height;glfwGetFramebufferSize(window,&width,&height);glViewport(0,0,width,height);glClearColor(.051f,.059f,.071f,1);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if(hidden&&stage==2&&!selfTest&&!stageTest&&application.settled()&&(application.canClose()||propose)&&++settled>=6){if(!capture.empty()){std::filesystem::create_directories(capture.parent_path());cv::Mat pixels(height,width,CV_8UC3);glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,width,height,GL_BGR,GL_UNSIGNED_BYTE,pixels.data);cv::flip(pixels,pixels,0);auto name=capture.u8string();if(!cv::imwrite(std::string(name.begin(),name.end()),pixels))throw std::runtime_error("Screenshot failed");}
                if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("OpenGL smoke failed");std::cout<<"PASS: native UI loaded real scene; view rendered without GL errors\n";glfwSetWindowShouldClose(window,true);}
            if(hidden&&(!application.error().empty()||std::chrono::steady_clock::now()-start>std::chrono::seconds(50)))throw std::runtime_error(application.error().empty()?"Native UI timed out":application.error());
            glfwSwapBuffers(window);
        }
    }catch(const std::exception& error){std::cerr<<error.what()<<"\n";result=4;}
    ImGui_ImplOpenGL3_Shutdown();ImGui_ImplGlfw_Shutdown();ImGui::DestroyContext();glfwDestroyWindow(window);glfwTerminate();return result;
}
