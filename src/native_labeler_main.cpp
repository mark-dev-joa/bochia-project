#include "native_labeler_app.hpp"
#include "native_labeler_theme.hpp"
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
    config.source=root/"data/imports";const auto nativeProject=std::filesystem::absolute(root/"output/native-labeler/project.json");config.project=nativeProject;config.exports=root/"output/native-labeler/exports";
    bool hidden=false,selfTest=false,stageTest=false;std::filesystem::path capture;std::wstring mode=L"overview";int recording=5,frame=0,requestedWidth=1440,requestedHeight=850,selectCamera=-1,selectRank=-1,checkStep=0;
    for(int i=1;i<argc;++i){std::wstring argument=args[i];
        auto value=[&](){if(i+1>=argc)throw std::runtime_error("Missing option value");return std::wstring(args[++i]);};
        if(argument==L"--port")config.port=std::stoi(value());else if(argument==L"--source")config.source=value();else if(argument==L"--project")config.project=value();else if(argument==L"--export-root")config.exports=value();
        else if(argument==L"--no-start-server")config.startServer=false;else if(argument==L"--smoke-hidden")hidden=true;
        else if(argument==L"--capture")capture=value();else if(argument==L"--view")mode=value();else if(argument==L"--recording")recording=std::stoi(value());else if(argument==L"--frame")frame=std::stoi(value());
        else if(argument==L"--width")requestedWidth=std::stoi(value());else if(argument==L"--height")requestedHeight=std::stoi(value());
        else if(argument==L"--select"){auto text=value();auto colon=text.find(L':');selectCamera=std::stoi(text.substr(0,colon));selectRank=std::stoi(text.substr(colon+1));}else if(argument==L"--self-test")selfTest=true;else if(argument==L"--stage-test")stageTest=true;
    }LocalFree(args);config.rememberUi=!hidden;
    // The engine and the screens get the same full paths with one kind of separator.
    for(auto* path:{&config.source,&config.project,&config.exports})*path=std::filesystem::absolute(*path);
    // Copy the existing reviewed work once; the browser and native processes use
    // separate stores so their in-memory caches cannot overwrite each other.
    try {if(config.project==nativeProject&&!std::filesystem::exists(nativeProject)){
        auto original=root/"output/ball-labeler/project.json";
        if(std::filesystem::exists(original)){
            std::filesystem::create_directories(nativeProject.parent_path());
            auto selection=original.parent_path()/"project-selection.json";
            if(std::filesystem::exists(selection))std::filesystem::copy_file(selection,nativeProject.parent_path()/"project-selection.json",std::filesystem::copy_options::skip_existing);
            std::filesystem::copy_file(original,nativeProject,std::filesystem::copy_options::skip_existing);
        }
    }}catch(const std::exception& error){std::cerr<<"기존 라벨 가져오기 실패: "<<error.what()<<"\n";return 4;}
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
                    else if(mode==L"prepare")application.showPage(0);else if(mode==L"split")application.showPage(2);else if(mode==L"export")application.showPage(3);
                    stage=2;}
                else if(stage==2&&selfTest&&application.canClose()&&application.settled()){if(!application.selfCheckStep(checkStep++)){std::cout<<"PASS: select, zoom, one-pixel edge correction, ball cycling and scene confirm\n";selfTest=false;}}
                else if(stage==2&&stageTest&&application.canClose()&&application.settled()){if(!application.stageCheckStep(checkStep++)){std::cout<<"PASS: split plan, grouping, confirm, export and export history\n";stageTest=false;}}
            }
            ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();application.draw();
            if(closing){ImGui::OpenPopup("종료 전 작업 보관");if(ImGui::BeginPopupModal("종료 전 작업 보관",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){ImGui::TextUnformatted("저장 또는 처리가 끝나지 않았습니다.");ImGui::TextUnformatted("작업을 계속하거나 변경을 recovery 폴더에 보관하고 종료할 수 있습니다.");if(ImGui::Button("계속 작업")){closing=false;ImGui::CloseCurrentPopup();}ImGui::SameLine();ImGui::BeginDisabled(application.busy());if(ImGui::Button("변경 보관하고 종료")){application.preserveRecovery();glfwSetWindowShouldClose(window,true);closing=false;ImGui::CloseCurrentPopup();}ImGui::EndDisabled();ImGui::EndPopup();}}
            ImGui::Render();
            int width,height;glfwGetFramebufferSize(window,&width,&height);glViewport(0,0,width,height);glClearColor(.051f,.059f,.071f,1);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if(hidden&&stage==2&&!selfTest&&!stageTest&&application.settled()&&application.canClose()&&++settled>=6){if(!capture.empty()){std::filesystem::create_directories(capture.parent_path());cv::Mat pixels(height,width,CV_8UC3);glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,width,height,GL_BGR,GL_UNSIGNED_BYTE,pixels.data);cv::flip(pixels,pixels,0);auto name=capture.u8string();if(!cv::imwrite(std::string(name.begin(),name.end()),pixels))throw std::runtime_error("Screenshot failed");}
                if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("OpenGL smoke failed");std::cout<<"PASS: native UI loaded real scene; view rendered without GL errors\n";glfwSetWindowShouldClose(window,true);}
            if(hidden&&(!application.error().empty()||std::chrono::steady_clock::now()-start>std::chrono::seconds(50)))throw std::runtime_error(application.error().empty()?"Native UI timed out":application.error());
            glfwSwapBuffers(window);
        }
    }catch(const std::exception& error){std::cerr<<error.what()<<"\n";result=4;}
    ImGui_ImplOpenGL3_Shutdown();ImGui_ImplGlfw_Shutdown();ImGui::DestroyContext();glfwDestroyWindow(window);glfwTerminate();return result;
}
