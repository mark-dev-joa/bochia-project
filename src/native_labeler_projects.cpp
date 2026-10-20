#include "native_labeler_app.hpp"
#include "native_labeler_theme.hpp"
#include "labeler_util.hpp"
#include "labeler_detector.hpp"
#include "model_paths.hpp"
#include "labeler_sam.hpp"
#include "labeler_log.hpp"
#include "yolo_detector.hpp"
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>

namespace boccia::labeler {
namespace fs = std::filesystem;
namespace {
std::vector<fs::path> pick(const wchar_t* title, bool folders, bool multiple = false, const COMDLG_FILTERSPEC* filter = nullptr) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr; std::vector<fs::path> paths;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0; dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | (folders ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST) | (multiple ? FOS_ALLOWMULTISELECT : 0));
        dialog->SetTitle(title);
        const COMDLG_FILTERSPEC images[] = {{L"이미지", L"*.jpg;*.jpeg;*.png;*.bmp;*.tif;*.tiff"}};
        if (!folders) dialog->SetFileTypes(1, filter ? filter : images);
        if (SUCCEEDED(dialog->Show(static_cast<HWND>(ImGui::GetWindowViewport()->PlatformHandleRaw)))) {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(dialog->GetResults(&items))) {
                DWORD count = 0; items->GetCount(&count);
                for (DWORD i = 0; i < count; ++i) {
                    IShellItem* item = nullptr; if (FAILED(items->GetItemAt(i, &item))) continue;
                    PWSTR path = nullptr; if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { paths.emplace_back(path); CoTaskMemFree(path); } item->Release();
                }
                items->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return paths;
}
fs::path recentFile() { wchar_t location[32768]{}; GetEnvironmentVariableW(L"LOCALAPPDATA", location, 32768); return fs::path(location) / "Boccia/labeler-projects.json"; }
// The GPU can run here: the CUDA build of ONNX Runtime beside the program and a CUDA runtime it finds (yolo_detector.hpp).
bool gpuReady() {
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, 32768);
    return !yoloCudaRuntime().empty() && fs::is_regular_file(fs::path(module).parent_path() / "onnxruntime_providers_cuda.dll");
}
fs::path defaultProjectLocation() {
    PWSTR documents=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&documents)))throw std::runtime_error("기본 문서 폴더를 찾지 못했습니다. 저장 위치를 선택하세요.");
    fs::path result=fs::path(documents)/"Boccia Projects";CoTaskMemFree(documents);
    // Documents kept by OneDrive (2026-10-19): the label file is written on every confirmation and the sync client holds
    // it each time, so the projects go on the local disk instead.
    if(utf8(result).find("OneDrive")!=std::string::npos)result=fs::path(L"C:\\Boccia Projects");
    return result;
}
void setLocation(char* buffer,size_t capacity,const fs::path& path){const auto value=utf8(path);if(value.size()>=capacity)throw std::runtime_error("저장 위치 경로가 너무 깁니다.");std::copy(value.begin(),value.end(),buffer);buffer[value.size()]=0;}
std::string sizeLabel(uint64_t bytes) {
    char text[64]{};
    if (bytes >= 1024ull * 1024 * 1024) std::snprintf(text, sizeof text, "%.2f GB", double(bytes) / double(1024ull * 1024 * 1024));
    else if (bytes >= 1024ull * 1024) std::snprintf(text, sizeof text, "%.1f MB", double(bytes) / double(1024 * 1024));
    else std::snprintf(text, sizeof text, "%.0f KB", std::max(1.0, double(bytes) / 1024));
    return text;
}
// A path or name that keeps its end when it does not fit: the last folder names the project.
std::string tailFit(const std::string& value, float width, float size, ImFont* font) {
    if (theme::extent(value, size, font).x <= width) return value;
    for (size_t start = 1; start < value.size(); ++start) {
        if ((value[start] & 0xC0) == 0x80) continue;   // inside a UTF-8 character
        const std::string tail = "…" + value.substr(start);
        if (theme::extent(tail, size, font).x <= width) return tail;
    }
    return "…";
}
// The detector a new project drafts with when nothing else is chosen: the newest full-frame YOLO model in the first models
// folder that has one (beside the program, bin/release/models, then the repository). ROI, fp16 and raw exports are for the
// tracker and are skipped.
fs::path defaultDraftModel(const fs::path& repository) {
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, 32768);
    for (const auto& root : boccia::modelRoots(fs::path(module).parent_path(), repository / "data"))
        if (auto best = boccia::latestDetector(root / "boccia"); !best.empty()) return best;
    return {};
}
// "오늘 21:16" for today, otherwise the month and day.
std::string whenOpened(int64_t seconds) {
    if (seconds <= 0) return {};
    const time_t stamp = time_t(seconds), now = std::time(nullptr); tm local{}, today{}; localtime_s(&local, &stamp); localtime_s(&today, &now);
    char text[16]{};
    if (local.tm_year == today.tm_year && local.tm_yday == today.tm_yday) { std::strftime(text, sizeof text, "%H:%M", &local); return std::string("오늘 ") + text; }
    std::strftime(text, sizeof text, "%m-%d", &local); return text;
}
}
bool Application::imageScene() const {
    if (!ready()) return false;
    const auto& scene = catalog_.at("scenes").at(scene_);
    return scene.value("source_type", "") == "images" || (frames_.size() == 1 && frames_[0].member.value("camera", "") == "images");
}
bool Application::prepareProjectChange(std::function<void()> after) {
    if (!guard()) return false;
    if (const int slot = dirtySlot(); slot >= 0) { save(std::move(after), slot); return false; }
    if (!settled() || autoStatus_.value("job", Json::object()).value("status", "") == "running" || exportStatus_.value("status", "") == "running") { message_ = "진행 중인 작업이 끝난 뒤 프로젝트를 전환하거나 자료를 추가하세요."; return false; }
    return true;
}
void Application::resetProjectViews() {
    if (ahead_.valid()) { try { ahead_.get(); } catch (...) {} }
    if (thumbTask_.valid()) { try { thumbTask_.get(); } catch (...) {} }
    releaseFrames();
    for (auto& [key, id] : thumbs_) if (id) glDeleteTextures(1, &id);
    thumbs_.clear(); thumbWanted_.clear(); rows_.clear(); history_.clear(); lastView_.clear(); picked_.clear();
    catalog_ = nullptr; engine_.reset(); groups_ = splits_ = Json::object();
    autoStatus_ = colorStatus_ = circleStatus_ = missingStatus_ = propagationStatus_ = occlusionStatus_ = exportStatus_ = Json::object();
    scene_ = camera_ = 0; selected_ = -1; filter_ = 0; editor_ = false; planEdited_ = false; backing_ = false; stale_ = false;
    aheadScene_ = -1; aheadAuto_ = -1; generation_++; queued_ = nullptr; thumbWanted_.clear();
    batchTools_ = false; batchScope_ = 0; importPreviewWanted_ = false; importPlan_.reset();
    exportChosenInit_ = false; exportAsk_ = false; exportWatching_ = false; exportChosen_.clear(); exportFolder_[0] = 0;
    nextAsk_=missingAsk_=occludeAsk_=false;
    proposal_ = nullptr; polygon_.clear(); points_ = Json::array(); dragging_ = false; rowsDirty_ = aheadDirty_ = true;
}
// The recent list keeps when a project was last open and how far its review was, for the start screen.
void Application::rememberProject(const fs::path& path) {
    if (!config_.rememberUi) return;
    Json next = Json::array(); const auto target = utf8(path);
    const std::string name = workspace_.empty() ? std::string("기존 라벨 작업") : readWorkspace(workspace_).value("name", "프로젝트");
    Json entry = {{"path", target}, {"name", name}, {"opened", int64_t(std::time(nullptr))}, {"legacy", workspace_.empty()}};
    if (catalog_.is_object()) { entry["targets"] = catalog_.value("target_images", 0); entry["reviewed"] = catalog_.value("reviewed_target_images", 0); }
    next.push_back(entry);
    for (const auto& item : recentProjects_) if (item.value("path", "") != target && next.size() < 12) next.push_back(item);
    fs::create_directories(recentFile().parent_path()); atomicJson(recentFile(), next, true); recentProjects_ = std::move(next);
}
void Application::recentForCapture(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    auto list = Json::parse(input);
    if (!list.is_array()) throw std::runtime_error("최근 프로젝트 목록 형식이 올바르지 않습니다.");
    recentProjects_ = std::move(list);
}
void Application::activateProject(JobResult result) {
    if (projectOpened()) { try { rememberProject(workspace_.empty() ? config_.project : workspace_); } catch (...) {} }
    rememberPosition(); resetProjectViews(); config_ = std::move(*result.configuration); engine_ = std::move(result.engine);
    workspace_ = fs::is_regular_file(config_.source.parent_path() / "boccia-project.json") ? config_.source.parent_path() : fs::path{};
    workspaceName_ = workspace_.empty() ? "" : readWorkspace(workspace_).value("name", "프로젝트");
    updateCatalog(result.data.at("catalog")); autoStatus_ = result.data.at("auto");
    circleMargin_ = float(catalog_.value("circle_margin", .5)); page_ = Page::Data;
    message_ = "프로젝트를 열었습니다."; error_.clear(); importSummary_ = nullptr;
    try { rememberProject(workspace_.empty() ? config_.project : workspace_); } catch (const std::exception& error) { message_ += std::string(" 최근 목록 저장 실패: ") + error.what(); }
    restorePosition();
}
void Application::openProject(const fs::path& given) {
    if (!prepareProjectChange([this, given] { openProject(given); })) return;
    logLine("프로젝트 열기 · " + utf8(given));
    const auto previous = config_;
    launch("프로젝트 여는 중", [previous, given] {
        Configuration next = previous; next.startScreen = false; next.lease.reset();
        auto root = workspaceRoot(given);
        if (!fs::is_regular_file(root / "boccia-project.json") && fs::is_regular_file(root.parent_path() / "boccia-project.json")) root = root.parent_path();
        if (fs::is_regular_file(root / "boccia-project.json")) { readWorkspace(root); next.source = root / "source"; next.project = root / "labels/project.json"; next.exports = root / "exports"; }
        else {
            if(localPortListening(8767))throw std::runtime_error("이전 Python 라벨 엔진이 실행 중입니다. 기존 라벨 작업을 열기 전에 이전 엔진을 종료하세요.");
            const auto labels = fs::is_regular_file(given) ? given : root / "labels/project.json";
            std::ifstream input(labels, std::ios::binary); if (!input) throw std::runtime_error("프로젝트 폴더 또는 프로젝트 파일을 선택하세요.");
            const auto state = Json::parse(input); auto source = fromUtf8(state.at("source").get<std::string>());
            next.source = fs::absolute(source.is_absolute() ? source : labels.parent_path() / source); next.project = fs::absolute(labels); next.exports = labels.parent_path().parent_path() / "exports";
        }
        if (!previous.project.empty() && fs::weakly_canonical(next.project) == fs::weakly_canonical(previous.project)) next.lease = previous.lease;
        else next.lease = acquireLabelLease(next.project);
        if (const auto folder = next.source.parent_path(); fs::is_regular_file(folder / "boccia-project.json") && (!pendingImports(folder).empty() || fs::exists(folder / "pending")))
            rollbackImports(folder);   // an import a crash interrupted is undone before the labels open
        auto engine = std::make_shared<Engine>(EnginePaths{next.source, next.project, next.exports, next.models, next.root});
        JobResult result{{{"catalog", engine->nativeCatalog(false)}, {"auto", engine->autoStatus()}}, {}};
        result.engine = std::move(engine); result.configuration = std::make_shared<Configuration>(std::move(next)); return result;
    }, [this](JobResult result) { activateProject(std::move(result)); });
}
void Application::chooseProject() { const auto paths = pick(L"Boccia Labeler 프로젝트 폴더 열기", true); if (!paths.empty()) openProject(paths.front()); }
void Application::beginCreateProject(){
    projectName_[0]=0;createProjectError_.clear();creationPopupSize_={};deviceGpu_=modelsOnGpu();
    const auto model=defaultDraftModel(config_.root);draftSeeds_=model.empty()?1:0;draftModel_[0]=0;
    if(!model.empty()){try{setLocation(draftModel_,sizeof draftModel_,model);}catch(...){draftSeeds_=1;}}
    if(!projectLocation_[0]){try{setLocation(projectLocation_,sizeof projectLocation_,defaultProjectLocation());}catch(const std::exception& error){createProjectError_=error.what();}}
    createProjectWanted_=true;
}
void Application::showNewProjectForCapture(const fs::path& parent,const std::string& name){
    beginCreateProject();if(!parent.empty())setLocation(projectLocation_,sizeof projectLocation_,parent);
    if(name.size()>=sizeof projectName_)throw std::runtime_error("프로젝트 이름이 너무 깁니다.");std::copy(name.begin(),name.end(),projectName_);projectName_[name.size()]=0;
}
void Application::chooseProjectLocation(){const auto paths=pick(L"프로젝트 저장 위치 선택",true);if(!paths.empty()){try{setLocation(projectLocation_,sizeof projectLocation_,paths.front());createProjectError_.clear();}catch(const std::exception& error){createProjectError_=error.what();}}}
void Application::closeProject(){
    if(!prepareProjectChange([this]{closeProject();}))return;
    logLine("프로젝트 닫기 · "+(workspace_.empty()?std::string("기존 라벨 작업"):workspaceName_));
    try{rememberProject(workspace_.empty()?config_.project:workspace_);}catch(...){}
    rememberPosition();resetProjectViews();config_.lease.reset();config_.source.clear();config_.project.clear();config_.exports.clear();config_.startScreen=true;
    workspace_.clear();workspaceName_.clear();importSummary_=nullptr;page_=Page::Data;message_.clear();
}
void Application::createProject() {
    if (!prepareProjectChange([this] { createProject(); })) return;
    const std::string name = projectName_;
    const auto component = fromUtf8(name);
    if (name.empty() || component.filename() != component || name == "." || name == ".." || name.find_first_of("<>:\"/\\|?*") != std::string::npos || name.back() == '.' || name.back() == ' ') { createProjectError_ = "프로젝트 이름을 입력하고 사용할 수 없는 문자를 확인하세요."; return; }
    const auto location=fromUtf8(projectLocation_);
    if(location.empty()||!location.is_absolute()){createProjectError_="저장 위치를 선택하거나 전체 폴더 경로를 입력하세요.";return;}
    const auto folder = location / component;
    try{if(fs::exists(folder)&&(!fs::is_directory(folder)||!fs::is_empty(folder))){createProjectError_="같은 이름의 폴더가 있습니다. 프로젝트 이름이나 저장 위치를 바꾸세요.";return;}}
    catch(const std::exception& error){createProjectError_=error.what();return;}
    const std::string seeds = draftSeeds_ == 0 ? "yolo" : "circles";
    const fs::path model = draftSeeds_ == 0 ? fromUtf8(draftModel_) : fs::path{};
    if (seeds == "yolo" && model.empty()) { createProjectError_ = "YOLO 모델 파일을 고르거나 원형 검출을 선택하세요."; return; }
    try { saveDevice(deviceGpu_); } catch (const std::exception& error) { createProjectError_ = error.what(); return; }
    createProjectError_.clear();
    // The dialog stays open until the project exists: a model that is not a detector is reported in it.
    launch("프로젝트 만드는 중", [folder, name, seeds, model] {
        if (seeds == "yolo") if (const auto problem = detectorProblem(model); !problem.empty()) return JobResult{{{"error", problem}}, {}};
        try { createWorkspace(folder, name, seeds, model); } catch (const std::exception& error) { return JobResult{{{"error", std::string(error.what())}}, {}}; }
        return JobResult{{{"folder", utf8(folder)}}, {}};
    }, [this](JobResult result) {
        if (result.data.contains("error")) { createProjectError_ = result.data.at("error").get<std::string>(); return; }
        createProjectWanted_ = false; openProject(fromUtf8(result.data.at("folder").get<std::string>()));
    });
}
void Application::chooseDraftModel() {
    const COMDLG_FILTERSPEC filter[] = {{L"ONNX 모델", L"*.onnx"}};
    const auto paths = pick(L"YOLO 모델 파일 선택", false, false, filter);
    if (paths.empty()) return;
    try { setLocation(draftModel_, sizeof draftModel_, paths.front()); draftSeeds_ = 0; } catch (const std::exception& error) { createProjectError_ = settingsError_ = error.what(); }
}
// The 초안 후보 field: the trained model with its file, or circles found in the image without a model.
void Application::draftField(float width) {
    theme::fieldLabel("초안 후보");
    constexpr float radioWidth = 98, browse = 92;
    if (theme::radio("YOLO 모델##draftYolo", draftSeeds_ == 0, radioWidth - 22)) draftSeeds_ = 0;
    ImGui::SameLine(0, 8);
    {   // The box names the model file; the tooltip gives where it is, and a click opens the file dialog like the button.
        const ImVec2 at = ImGui::GetCursorScreenPos(); const float boxWidth = width - radioWidth - 8 - browse - 8;
        const bool clicked = ImGui::InvisibleButton("##draftModelBox", {boxWidth, 36}), hovered = ImGui::IsItemHovered();
        if (clicked && !busy()) later_ = [this] { chooseDraftModel(); };
        auto draw = ImGui::GetWindowDrawList(); const bool on = draftSeeds_ == 0, chosen = draftModel_[0] != 0;
        draw->AddRectFilled(at, {at.x + boxWidth, at.y + 36}, theme::bg0, 7); draw->AddRect(at, {at.x + boxWidth, at.y + 36}, hovered ? theme::text3 : theme::line2, 7);
        theme::icon(draw, theme::Icon::File, {at.x + 18, at.y + 18}, 13, on ? theme::text2 : theme::text3);
        const std::string file = chosen ? utf8(fromUtf8(draftModel_).filename()) : "모델 파일을 고르세요";
        theme::write(draw, {at.x + 32, at.y + 18 - theme::caption / 2}, on && chosen ? theme::text : theme::text3, tailFit(file, boxWidth - 44, theme::caption, theme::mono), theme::caption, theme::mono);
        if (hovered && chosen) ImGui::SetTooltip("%s", utf8(fromUtf8(draftModel_).make_preferred()).c_str());
    }
    ImGui::SameLine(0, 8);
    if (theme::button("찾아보기##draftModel", {browse, 36}, false, theme::Icon::None, !busy())) later_ = [this] { chooseDraftModel(); };
    if (theme::radio("원형 검출##draftCircles", draftSeeds_ == 1, radioWidth - 22)) draftSeeds_ = 1;
    ImGui::SameLine(0, 8);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    theme::write(ImGui::GetWindowDrawList(), {at.x + 4, at.y + 18 - theme::caption / 2}, theme::text3, "허프 변환 · 모델 없이", theme::caption);
    ImGui::Dummy({width - radioWidth - 8, 36});
}
// 실행 장치 (2026-10-15 user request: drafts on the GPU): where drafts and SAM run on this PC.
void Application::deviceField(float width) {
    theme::fieldLabel("실행 장치");
    if (theme::radio("GPU##deviceGpu", deviceGpu_, 76)) deviceGpu_ = true;
    ImGui::SameLine(0, 16);
    if (theme::radio("CPU##deviceCpu", !deviceGpu_, 76)) deviceGpu_ = false;
    ImGui::SameLine(0, 16);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const std::string problem = deviceGpu_ && modelsOnGpu() ? gpuProblem() : std::string();
    if (deviceGpu_ && (!gpuReady() || !problem.empty())) {
        const char* text = gpuReady() ? "GPU 실패 · CPU로 실행" : "CUDA 없음 · CPU로 실행";
        theme::write(ImGui::GetWindowDrawList(), {at.x, at.y + 18 - theme::caption / 2}, theme::warn, text, theme::caption);
        if (!problem.empty() && ImGui::IsMouseHoveringRect({at.x, at.y}, {at.x + 200, at.y + 36})) ImGui::SetTooltip("%s", problem.c_str());
    }
    ImGui::Dummy({std::max(1.f, width - (at.x - ImGui::GetWindowPos().x - ImGui::GetStyle().WindowPadding.x)), 36});
}
void Application::loadDevice() {
    settings_ = loadLabelerSettings();   // labeler_settings.hpp: the 실행 장치 and the hub, kept for the PC
    setModelsOnGpu(settings_.gpu);
    logLine(std::string("실행 장치 · ") + (settings_.gpu ? "GPU" : "CPU") + (settings_.gpu && !gpuReady() ? " (CUDA 없음)" : ""));
}
void Application::saveDevice(bool gpu) {
    auto settings = loadLabelerSettings();   // the file as it is now: a hidden check must not drop the hub's part
    settings.gpu = gpu;
    saveLabelerSettings(settings);
    settings_.gpu = gpu;
    setModelsOnGpu(gpu);
    logLine(std::string("실행 장치 바꿈 · ") + (gpu ? "GPU" : "CPU"));
}
void Application::beginProjectSettings() {
    if (workspace_.empty()) return;
    settingsError_.clear(); draftModel_[0] = 0; draftSeeds_ = 1; deviceGpu_ = modelsOnGpu();
    try {
        const auto settings = draftSettings(workspace_);
        draftSeeds_ = settings.seeds == "yolo" ? 0 : 1;
        if (!settings.model.empty()) setLocation(draftModel_, sizeof draftModel_, settings.model);
    } catch (const std::exception& error) { settingsError_ = error.what(); }
    // With circles chosen, the YOLO field still shows a model to switch to.
    if (!draftModel_[0]) { const auto model = defaultDraftModel(config_.root); if (!model.empty()) { try { setLocation(draftModel_, sizeof draftModel_, model); } catch (...) {} } }
    settingsWanted_ = true;
}
void Application::saveProjectSettings() {
    if (workspace_.empty() || busy()) return;
    const std::string seeds = draftSeeds_ == 0 ? "yolo" : "circles";
    const fs::path model = draftSeeds_ == 0 ? fromUtf8(draftModel_) : fs::path{};
    if (seeds == "yolo" && model.empty()) { settingsError_ = "YOLO 모델 파일을 고르거나 원형 검출을 선택하세요."; return; }
    try { saveDevice(deviceGpu_); } catch (const std::exception& error) { settingsError_ = error.what(); return; }
    settingsError_.clear();
    const auto root = workspace_;
    launch("프로젝트 설정 저장", [root, seeds, model] {
        if (seeds == "yolo") if (const auto problem = detectorProblem(model); !problem.empty()) return JobResult{{{"error", problem}}, {}};
        try { return JobResult{{{"seeds", setDraftSettings(root, seeds, model).seeds}}, {}}; }
        catch (const std::exception& error) { return JobResult{{{"error", std::string(error.what())}}, {}}; }
    }, [this](JobResult result) {
        if (result.data.contains("error")) { settingsError_ = result.data.at("error").get<std::string>(); return; }
        settingsWanted_ = false;
        message_ = result.data.at("seeds") == "yolo" ? "초안 후보를 YOLO 모델로 정했습니다." : "초안 후보를 원형 검출로 정했습니다.";
    });
}
void Application::importData() { const auto paths = pick(L"임포트할 이미지 선택", false, true); if (!paths.empty()) importPaths(paths); }
void Application::chooseImportFolder() { const auto paths = pick(L"임포트할 촬영·이미지 폴더 선택", true, true); if (!paths.empty()) importPaths(paths); }
void Application::droppedFiles(std::vector<fs::path> files) {
    try {
    if (files.size() == 1 && fs::is_regular_file(workspaceRoot(files.front()) / "boccia-project.json")) { openProject(files.front()); return; }
    importPaths(std::move(files));
    } catch(const std::exception& error) { error_ = error.what(); }
}
void Application::importPaths(std::vector<fs::path> files) {
    if (workspace_.empty()) { message_ = "자료를 임포트할 프로젝트를 먼저 만들거나 여세요."; page_ = Page::Data; return; }
    if (!prepareProjectChange([this, files] { importPaths(files); })) return;
    // The dialog opens at once and shows what is being checked; the list follows when the check is done.
    const auto root = workspace_; auto status = std::make_shared<ImportStatus>(); importProgress_ = status; page_ = Page::Data;
    importPlan_.reset(); importSummary_ = nullptr; importPreviewWanted_ = true;
    { std::lock_guard guard(status->lock); status->phase = "자료 확인 중"; }
    const auto progress = [status](const std::string& phase, uint64_t done, uint64_t total) { std::lock_guard guard(status->lock); status->phase = phase; status->done = done; status->total = total; };
    auto plan = std::make_shared<ImportPlan>();
    launch("자료 확인 중", [root, files, status, progress, plan] {
        try { *plan = planImport(root, files, progress, [status] { return status->cancelled.load(); }); }
        catch (const std::exception& error) { return JobResult{{{status->cancelled ? "cancelled" : "error", std::string(error.what())}}, {}}; }
        return JobResult{plan->summary(), {}};
    }, [this, plan](JobResult result) {
        importProgress_.reset();
        if (result.data.contains("cancelled")) { importPreviewWanted_ = false; message_ = "임포트를 중단했습니다. 프로젝트는 그대로입니다."; return; }
        if (result.data.contains("error")) { importPreviewWanted_ = false; error_ = result.data.at("error").get<std::string>(); return; }
        importPlan_ = plan; importChosen_.assign(plan->items.size(), 1); importSummary_ = result.data; message_ = "임포트할 자료와 용량을 확인하세요.";
    });
}
namespace {
constexpr double guessedSecondsPerImage = .3;   // drafts, filter and circles of one image before any has been timed
}
// The import runs in one job: the copy of the chosen items, then for each item its drafts, the repeated-frame filter and
// the circle fit, on an engine of its own. The open engine stays idle meanwhile and is replaced at the end. A cancel or
// a failure rolls the whole import back and reopens the project.
void Application::commitPlannedImport() {
    if (!importPlan_ || busy()) return;
    auto plan = std::make_shared<ImportPlan>(*importPlan_);
    plan->items = Json::array(); plan->bytes = 0;
    for (size_t i = 0; i < importPlan_->items.size(); ++i) if (i < importChosen_.size() && importChosen_[i]) {
        plan->items.push_back(importPlan_->items[i]); plan->bytes += importPlan_->items[i].value("bytes", uint64_t(0));
    }
    if (plan->items.empty()) return;
    try {   // the drafts need the project's model; without it nothing is copied
        const auto settings = draftSettings(workspace_);
        if (settings.seeds == "yolo" && !fs::is_regular_file(settings.model)) throw std::runtime_error("초안 모델 파일이 없습니다. 프로젝트 설정에서 다시 고르세요.");
    } catch (const std::exception& error) { importPreviewWanted_ = false; importPlan_.reset(); error_ = error.what(); later_ = [this] { beginProjectSettings(); }; return; }
    auto status = std::make_shared<ImportStatus>(); importProgress_ = status; status->items = plan->items;
    int captures = 0;
    for (const auto& item : plan->base.at("imports")) captures += item.at("kind") == "capture";
    for (const auto& item : plan->items) {
        status->ids.push_back(item.at("id").get<std::string>()); status->frames.push_back(item.value("images", uint64_t(0)));
        status->titles.push_back(item.at("kind") == "capture" ? captureTitle(++captures) : item.at("name").get<std::string>());
    }
    status->states.assign(plan->items.size(), 0); status->reasons.assign(plan->items.size(), std::string());
    { std::lock_guard guard(status->lock); status->phase = "원본 복사"; status->total = plan->bytes; status->item = 0; }
    const auto copied = [status](const std::string& phase, uint64_t done, uint64_t total) {
        std::lock_guard guard(status->lock);
        const auto now = std::chrono::steady_clock::now();
        if (status->copyStart == std::chrono::steady_clock::time_point{}) status->copyStart = now;
        status->phase = phase.substr(0, phase.find(" · ")); status->done = done; status->total = total;
        const double seconds = std::chrono::duration<double>(now - status->copyStart).count();
        uint64_t images = 0; for (const auto count : status->frames) images += count;
        status->remaining = seconds > 1 && done > 0 ? double(total - std::min(total, done)) * seconds / double(done) + double(images) * guessedSecondsPerImage : -1;
    };
    const auto started = [status](size_t item) {
        std::lock_guard guard(status->lock);
        for (auto& state : status->states) if (state == 1) state = 4;
        status->item = int(item); status->states[item] = 1;
    };
    const auto finished = [status](const std::string& id, const std::string& stage, uint64_t done, uint64_t total) {
        std::lock_guard guard(status->lock);
        const auto at = std::find(status->ids.begin(), status->ids.end(), id);
        if (at == status->ids.end()) return;
        const auto index = size_t(at - status->ids.begin()); const auto now = std::chrono::steady_clock::now();
        if (status->pipelineStart == std::chrono::steady_clock::time_point{}) status->pipelineStart = now;
        status->item = int(index); status->phase = stage; status->done = done; status->total = total;
        if (stage == "완료") { if (status->states[index] != 3) status->finishedFrames += status->frames[index]; status->states[index] = 3; }
        else status->states[index] = 2;
        // Seconds per image so far, over the items done and the drafts of this one; the rest of this item counts as drafted
        // once its drafts are, so the filter and the circles slow the estimate a little rather than stall it.
        const uint64_t current = stage == "초안 생성" ? std::min(done, status->frames[index]) : stage == "완료" ? 0 : status->frames[index];
        const uint64_t passed = status->finishedFrames + current;
        const double seconds = std::chrono::duration<double>(now - status->pipelineStart).count();
        const double perImage = passed >= 8 && seconds > 1 ? seconds / double(passed) : guessedSecondsPerImage;
        uint64_t left = 0;
        for (size_t i = 0; i < status->states.size(); ++i) if (status->states[i] != 3) left += status->frames[i];
        status->remaining = double(left - std::min(left, current)) * perImage;
    };
    const auto config = config_; const auto root = workspace_;
    {   // the log: what goes in
        std::string items;
        for (size_t i = 0; i < plan->items.size(); ++i) {
            const auto& item = plan->items[i];
            items += (i ? ", " : "") + status->titles[i] + " (" + item.value("name", "") + ", " + std::to_string(item.value("images", 0)) + "장, " + sizeLabel(item.value("bytes", uint64_t(0))) + ")";
        }
        logLine("임포트 시작 · " + utf8(root) + " · " + items);
    }
    launch("자료 임포트", [plan, status, copied, started, finished, config, root] {
        const auto cancelled = [status] { return status->cancelled.load(); };
        Json summary;
        const auto copyStarted = std::chrono::steady_clock::now();
        const auto seconds = [](std::chrono::steady_clock::time_point from) { char text[32]; std::snprintf(text, sizeof text, "%.1f초", std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count()); return std::string(text); };
        try { summary = commitImport(*plan, copied, cancelled, true, started); }
        catch (const std::exception& error) {
            logLine(std::string(status->cancelled ? "임포트 복사 중단 · " : "임포트 복사 실패 · ") + error.what());
            return JobResult{{{status->cancelled ? "cancelled" : "error", std::string(error.what())}}, {}};
        }
        logLine("임포트 복사 끝 · " + seconds(copyStarted));
        { std::lock_guard guard(status->lock); for (auto& state : status->states) if (state == 1) state = 4; }
        // Each item on its own (2026-10-15 user request: of several imports the one that fails is left out and the others
        // go on): a failed item is rolled back and the next one gets a new engine; 중단 rolls back the items not done.
        const EnginePaths paths{config.source, config.project, config.exports, config.models, config.root};
        std::shared_ptr<Engine> engine;
        std::vector<std::string> done; Json failures = Json::object(); std::string firstFailure;
        for (size_t i = 0; i < status->ids.size() && !status->cancelled; ++i) {
            const auto& id = status->ids[i];
            const auto itemStarted = std::chrono::steady_clock::now();
            try {
                if (!engine) engine = std::make_shared<Engine>(paths);
                engine->finishImports({id}, finished, cancelled);
                done.push_back(id);
                logLine("초안 끝 · " + status->titles[i] + " · " + seconds(itemStarted));
            } catch (const std::exception& error) {
                if (status->cancelled) { logLine("초안 중단 · " + status->titles[i]); break; }
                std::string message = error.what();
                engine.reset();   // its jobs stop and its writes end before the rollback edits the label file
                try { rollbackImports(root, {id}); } catch (const std::exception& failure) { message += std::string(" 되돌리지 못했습니다: ") + failure.what(); }
                logLine("초안 실패 · " + status->titles[i] + " · " + message + " · 이 항목만 되돌림");
                if (firstFailure.empty()) firstFailure = message;
                failures[id] = message;
                std::lock_guard guard(status->lock); status->states[i] = 5; status->reasons[i] = message;
            }
        }
        const bool stopped = status->cancelled;
        std::vector<std::string> undone;
        for (const auto& id : status->ids) if (std::find(done.begin(), done.end(), id) == done.end() && !failures.contains(id)) undone.push_back(id);
        if (!undone.empty()) {
            engine.reset();
            try { rollbackImports(root, undone); } catch (const std::exception& failure) { if (firstFailure.empty()) firstFailure = std::string("되돌리지 못했습니다: ") + failure.what(); }
        }
        logLine("임포트 끝 · 완료 " + std::to_string(done.size()) + "개 · 실패 " + std::to_string(failures.size()) + "개 · 되돌림 " + std::to_string(undone.size()) + "개" + (stopped ? " · 중단" : ""));
        if (done.empty()) return JobResult{{{stopped ? "cancelled" : "error", stopped ? std::string() : firstFailure}, {"rolled_back", true}, {"failures", failures}}, {}};
        try {
            if (!engine) engine = std::make_shared<Engine>(paths);
            JobResult result{{{"catalog", engine->nativeCatalog(false)}, {"auto", engine->autoStatus()}, {"summary", summary}, {"failures", failures}, {"stopped", stopped}}, {}};
            result.engine = std::move(engine);
            return result;
        } catch (const std::exception& error) { return JobResult{{{"error", std::string(error.what())}}, {}}; }
    }, [this](JobResult result) {
        // The dialog stays with how the import ended until 확인 (2026-10-15: with drafts on the GPU it closed by itself as soon
        // as it was done, and a failure looked the same); resetProjectViews would close it, so it is put back.
        const auto status = importProgress_; const auto plan = importPlan_;
        const auto keepDialog = [this, status, plan] { importProgress_ = status; importPlan_ = plan; importPreviewWanted_ = true; };
        const Json failures = result.data.value("failures", Json::object());
        const bool failed = result.data.contains("error"), nothing = failed || result.data.contains("cancelled");
        const bool stopped = result.data.contains("cancelled") || result.data.value("stopped", false);
        if (status) {
            std::lock_guard guard(status->lock);
            status->finished = true;
            status->outcome = failed ? "failed" : stopped ? "stopped" : failures.empty() ? "done" : "partial";
            if (failed) status->reason = result.data.at("error").get<std::string>();
        }
        if (nothing) {
            const std::string note = "임포트를 중단했습니다. 프로젝트는 그대로입니다.";
            if (result.data.value("rolled_back", false)) { rememberPosition(); resetProjectViews(); connect(stopped ? note : ""); }
            if (failed) error_ = result.data.at("error").get<std::string>(); else message_ = note;
            keepDialog();
            return;
        }
        rememberPosition(); importSummary_ = result.data.at("summary"); resetProjectViews();
        engine_ = std::move(result.engine); updateCatalog(result.data.at("catalog")); autoStatus_ = result.data.at("auto");
        circleMargin_ = float(catalog_.value("circle_margin", .5)); page_ = Page::Data;
        message_ = failures.empty() && !stopped ? "임포트를 마쳤습니다." : "일부만 임포트했습니다.";
        restorePosition();
        keepDialog();
    });
}
void Application::importStateForCapture(const std::string& state) {
    auto status = std::make_shared<ImportStatus>(); importProgress_ = status; importPreviewWanted_ = true;
    if (state == "copying" || state == "drafting") {
        auto plan = std::make_shared<ImportPlan>();
        plan->items = Json::array({{{"id", "a"}, {"name", "capture-1790943423420"}, {"kind", "capture"}, {"images", 420}, {"cameras", 3}},
                                   {{"id", "b"}, {"name", "capture-1790943437968"}, {"kind", "capture"}, {"images", 546}, {"cameras", 3}},
                                   {{"id", "c"}, {"name", "capture-1790943454638"}, {"kind", "capture"}, {"images", 1122}, {"cameras", 3}},
                                   {{"id", "d"}, {"name", "photos"}, {"kind", "images"}, {"images", 48}, {"folder", "D:/field/2026-10-02/photos"}}});
        plan->bytes = 2'300'000'000ull; importPlan_ = plan; importChosen_.assign(4, 1); status->items = plan->items;
        status->ids = {"a", "b", "c", "d"}; status->titles = {captureTitle(1), captureTitle(2), captureTitle(3), "photos"}; status->frames = {420, 546, 1122, 48};
        if (state == "copying") { status->states = {4, 1, 0, 0}; status->item = 1; status->phase = "원본 복사"; status->done = 968'000'000ull; status->total = plan->bytes; status->remaining = 780; }
        else { status->states = {3, 3, 2, 0}; status->item = 2; status->phase = "초안 생성"; status->done = 706; status->total = 1122; status->remaining = 125; }
    } else { importPlan_.reset(); status->phase = "촬영 확인 · capture-1790943454638"; status->done = 3; status->total = 9; }
}
float Application::projectControls() {
    if (!projectOpened()) return 0;
    const std::string name = workspace_.empty() ? std::string("기존 라벨 작업") : workspaceName_, current = utf8(workspace_.empty() ? config_.project : workspace_);
    auto draw = ImGui::GetWindowDrawList(); const ImVec2 at = ImGui::GetCursorScreenPos();
    const float nameWidth = std::min(220.f, theme::extent(name, theme::base, theme::bold).x), width = 34 + nameWidth + 32, height = 30;
    const ImVec2 end{at.x + width, at.y + height};
    ImGui::BeginDisabled(busy());
    const bool clicked = ImGui::InvisibleButton("##projectButton", {width, height}), hovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    if (clicked) ImGui::OpenPopup("##projectMenu");
    if (hovered) ImGui::SetTooltip("%s", current.c_str());
    draw->AddRectFilled(at, end, hovered ? theme::bg3 : theme::bg2, 7); draw->AddRect(at, end, theme::line2, 7);
    draw->AddRectFilled({at.x + 6, at.y + 5}, {at.x + 26, at.y + 25}, theme::accentSoft, 5);
    theme::icon(draw, theme::Icon::Folder, {at.x + 16, at.y + 15}, 12, theme::accent);
    theme::write(draw, {at.x + 34, at.y + 15 - theme::base / 2}, theme::text, tailFit(name, nameWidth, theme::base, theme::bold), theme::base, theme::bold);
    theme::icon(draw, theme::Icon::Down, {end.x - 16, at.y + 15}, 9, theme::text3);
    ImGui::SetNextWindowPos({at.x, end.y + 4});
    ImGui::SetNextWindowSizeConstraints({240, 0}, {FLT_MAX, FLT_MAX});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6, 6}); ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8, 4}); ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8);
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::line2));
    if (ImGui::BeginPopup("##projectMenu")) {
        if (ImGui::MenuItem("새 프로젝트")) later_ = [this] { beginCreateProject(); };
        if (ImGui::MenuItem("프로젝트 열기")) later_ = [this] { chooseProject(); };
        const bool others = std::any_of(recentProjects_.begin(), recentProjects_.end(), [&](const Json& item) { return item.value("path", "") != current; });
        if (ImGui::BeginMenu("최근 프로젝트", others)) {
            for (const auto& recent : recentProjects_) {
                const auto path = recent.value("path", ""), label = recent.value("name", "프로젝트");
                if (path == current) continue;
                if (ImGui::MenuItem((label + "##" + path).c_str())) later_ = [this, path] { openProject(fromUtf8(path)); };
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("프로젝트 설정", nullptr, false, !workspace_.empty())) later_ = [this] { beginProjectSettings(); };
        if (ImGui::MenuItem("작업자")) later_ = [this] { openWorkerDialog(); };
        if (ImGui::MenuItem("프로젝트 닫기")) later_ = [this] { closeProject(); };
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(); ImGui::PopStyleVar(3);
    return width;
}
// Before a project is open: the mark, the two ways in and the recent projects (design/labeler-v3 welcome.png).
void Application::welcomePage(){
    auto draw=ImGui::GetWindowDrawList();
    const ImVec2 origin=ImGui::GetCursorScreenPos(),available=ImGui::GetContentRegionAvail();
    draw->AddRectFilled(origin,{origin.x+available.x,origin.y+available.y},theme::bg0);
    const float width=std::min(620.f,available.x-48),rowHeight=62;
    const int count=int(recentProjects_.size()),shown=std::min(count,6);
    const float listHeight=shown*rowHeight+2,total=40+28+44+(shown?28+24+listHeight:0)+40;
    const float x=origin.x+std::floor((available.x-width)/2);
    float y=origin.y+std::max(32.f,std::floor((available.y-total)/2));
    draw->AddRectFilled({x,y},{x+40,y+40},theme::accent,12);   // the mark, as on the program icon
    draw->AddCircleFilled({x+20,y+20},8,theme::rgb(0x1b64da));draw->AddCircleFilled({x+20,y+20},4,theme::rgb(0xffffff));
    theme::write(draw,{x+54,y+20-11},theme::text,"Boccia Labeler",22,theme::bold);
    y+=40+28;
    const float half=std::floor((width-10)/2);
    ImGui::SetCursorScreenPos({x,y});
    if(theme::button("새 프로젝트",{half,44},true,theme::Icon::Plus,!busy()))later_=[this]{beginCreateProject();};
    ImGui::SetCursorScreenPos({x+width-half,y});
    if(theme::button("폴더에서 열기",{half,44},false,theme::Icon::Folder,!busy()))later_=[this]{chooseProject();};
    y+=44;
    if(shown){
        y+=28;
        theme::write(draw,{x,y},theme::text2,"최근 프로젝트",theme::caption,theme::bold);
        const std::string number=std::to_string(count)+"개";
        theme::write(draw,{x+width-theme::extent(number,theme::caption).x,y},theme::text3,number,theme::caption);
        y+=24;
        ImGui::SetCursorScreenPos({x,y});
        ImGui::PushStyleColor(ImGuiCol_ChildBg,ImGui::ColorConvertU32ToFloat4(theme::bg1));ImGui::PushStyleColor(ImGuiCol_Border,ImGui::ColorConvertU32ToFloat4(theme::line));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,10);ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
        ImGui::BeginChild("##recentProjects",{width,listHeight},ImGuiChildFlags_Borders,count>6?0:ImGuiWindowFlags_NoScrollbar);
        auto list=ImGui::GetWindowDrawList();
        for(int i=0;i<count;++i){
            const auto& recent=recentProjects_[i];
            const auto path=recent.value("path",""),name=recent.value("name","프로젝트");
            const ImVec2 row=ImGui::GetCursorScreenPos();const float rowWidth=ImGui::GetContentRegionAvail().x;
            ImGui::PushID(i);ImGui::BeginDisabled(busy());
            if(ImGui::InvisibleButton("##recent",{rowWidth,rowHeight}))later_=[this,path]{openProject(fromUtf8(path));};
            const bool hovered=ImGui::IsItemHovered();ImGui::EndDisabled();ImGui::PopID();
            if(hovered){list->AddRectFilled(row,{row.x+rowWidth,row.y+rowHeight},theme::bg2);ImGui::SetTooltip("%s",path.c_str());}
            if(i+1<count)list->AddLine({row.x,row.y+rowHeight-.5f},{row.x+rowWidth,row.y+rowHeight-.5f},theme::line);
            list->AddRectFilled({row.x+14,row.y+15},{row.x+46,row.y+47},theme::bg3,8);
            theme::icon(list,theme::Icon::Folder,{row.x+30,row.y+31},15,theme::text2);
            theme::icon(list,theme::Icon::Right,{row.x+rowWidth-20,row.y+31},10,theme::text3);
            // The review progress as a pill, and when the project was last open.
            const bool legacy=recent.value("legacy",false);const int targets=recent.value("targets",-1),reviewed=recent.value("reviewed",0);
            std::string pill;ImU32 ink=theme::text2,fill=0;
            if(legacy){pill="이전 형식";if(targets>=0)pill+=" · 검수 "+grouped(reviewed)+" / "+grouped(targets);ink=theme::hold;fill=theme::holdSoft;}
            else if(targets>0){pill="검수 "+grouped(reviewed)+" / "+grouped(targets);ink=theme::ok;fill=theme::okSoft;}
            else if(targets==0)pill="자료 없음";
            const std::string when=whenOpened(recent.value("opened",int64_t(0)));
            const float edge=row.x+rowWidth-38;float used=0;
            if(!pill.empty()){
                const float pw=theme::extent(pill,theme::caption).x+18;used=pw;
                const ImVec2 a{edge-pw,row.y+11},b{edge,row.y+31};
                if(fill)list->AddRectFilled(a,b,fill,10);else list->AddRect(a,b,theme::line2,10);
                theme::write(list,{a.x+9,a.y+10-theme::caption/2},ink,pill,theme::caption);
            }
            if(!when.empty()){const std::string text="마지막 작업 "+when;const float tw=theme::extent(text,theme::tiny).x;used=std::max(used,tw);theme::write(list,{edge-tw,row.y+37},theme::text3,text,theme::tiny);}
            const float room=edge-used-16-(row.x+58);
            theme::write(list,{row.x+58,row.y+13},theme::text,tailFit(name,room,theme::base,theme::bold),theme::base,theme::bold);
            theme::write(list,{row.x+58,row.y+35},theme::text3,tailFit(path,room,theme::caption,theme::mono),theme::caption,theme::mono);
        }
        ImGui::EndChild();ImGui::PopStyleVar(2);ImGui::PopStyleColor(2);
        y+=listHeight;
    }
    y+=18;
    if(busy()){theme::spinner(draw,{x+7,y+8},6,theme::accent);theme::write(draw,{x+22,y+8-theme::caption/2},theme::text2,busyLabel_,theme::caption);}
    else{theme::icon(draw,theme::Icon::Download,{x+7,y+8},13,theme::text3);theme::write(draw,{x+22,y+8-theme::caption/2},theme::text3,"프로젝트 폴더를 끌어 놓아 열기",theme::caption);}
    ImGui::SetCursorScreenPos(origin);ImGui::Dummy(available);
}
void Application::projectPopups() {
    // New project: the name, where it goes and the folder that will be made (design/labeler-v3 newproject.png).
    if (createProjectWanted_ && !ImGui::IsPopupOpen("새 프로젝트")) ImGui::OpenPopup("새 프로젝트");
    bool closeCreation = false;
    if (theme::beginDialog("새 프로젝트", "새 프로젝트", 600, &closeCreation)) {
        if (!createProjectWanted_ || closeCreation) { createProjectWanted_ = false; ImGui::CloseCurrentPopup(); }
        creationPopupSize_ = ImGui::GetWindowSize();
        const float full = ImGui::GetContentRegionAvail().x;
        theme::fieldLabel("프로젝트 이름");
        theme::textField("##projectName", projectName_, sizeof projectName_, full, ImGui::IsWindowAppearing());
        theme::fieldLabel("저장 위치");
        theme::textField("##projectLocation", projectLocation_, sizeof projectLocation_, full - 100, false, theme::mono);
        ImGui::SameLine(0, 8);
        if (theme::button("찾아보기##projectLocation", {92, 36}, false, theme::Icon::None, !busy())) later_ = [this] { chooseProjectLocation(); };
        draftField(full);
        deviceField(full);
        ImGui::Dummy({0, 2});
        auto draw = ImGui::GetWindowDrawList(); const ImVec2 at = ImGui::GetCursorScreenPos();
        draw->AddRectFilled(at, {at.x + full, at.y + 56}, theme::bg2, 8); draw->AddRect(at, {at.x + full, at.y + 56}, theme::line, 8);
        theme::icon(draw, theme::Icon::Folder, {at.x + 22, at.y + 28}, 14, theme::text3);
        theme::write(draw, {at.x + 40, at.y + 10}, theme::text2, "만들 폴더", theme::caption);
        const std::string destination = projectName_[0] ? utf8(fromUtf8(projectLocation_) / fromUtf8(projectName_)) : "—";
        theme::write(draw, {at.x + 40, at.y + 30}, projectName_[0] ? theme::text : theme::text3, tailFit(destination, full - 56, theme::caption, theme::mono), theme::caption, theme::mono);
        ImGui::Dummy({full, 56});
        if (!createProjectError_.empty()) {
            const ImVec2 line = ImGui::GetCursorScreenPos();
            theme::warning(draw, {line.x + 7, line.y + 9}, 13, theme::warn);
            ImGui::SetCursorScreenPos({line.x + 22, line.y});
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::warn)); ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + full - 22);
            ImGui::TextUnformatted(createProjectError_.c_str());
            ImGui::PopTextWrapPos(); ImGui::PopStyleColor();
        }
        theme::dialogFooter();
        const bool ready = !busy() && projectName_[0] && projectLocation_[0];
        const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        if (theme::footerButton("프로젝트 만들기", "Enter", true, ready) || (ready && enter)) later_ = [this] { createProject(); };
        if (theme::footerButton("취소", "Esc", false, !busy())) { createProjectWanted_ = false; ImGui::CloseCurrentPopup(); }
        theme::endDialog();
    }
    // Project settings: how drafts find their ball candidates, changeable after the project exists.
    if (settingsWanted_ && !ImGui::IsPopupOpen("프로젝트 설정")) ImGui::OpenPopup("프로젝트 설정");
    bool closeSettings = false;
    if (theme::beginDialog("프로젝트 설정", "프로젝트 설정", 600, &closeSettings)) {
        if (!settingsWanted_ || (closeSettings && !busy())) { settingsWanted_ = false; ImGui::CloseCurrentPopup(); }
        const float full = ImGui::GetContentRegionAvail().x;
        draftField(full);
        const ImVec2 note = ImGui::GetCursorScreenPos();
        theme::write(ImGui::GetWindowDrawList(), note, theme::text3, "다음 임포트와 초안 다시 만들기부터 적용", theme::caption);
        ImGui::Dummy({full, 22});
        deviceField(full);
        ImGui::Dummy({full, 6});
        if (!settingsError_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::warn)); ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + full);
            ImGui::TextUnformatted(settingsError_.c_str()); ImGui::PopTextWrapPos(); ImGui::PopStyleColor();
        }
        theme::dialogFooter();
        const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        if (theme::footerButton("저장", "Enter", true, !busy()) || (enter && !busy())) later_ = [this] { saveProjectSettings(); };
        if (theme::footerButton("취소", "Esc", false, !busy())) { settingsWanted_ = false; ImGui::CloseCurrentPopup(); }
        theme::endDialog();
    }
    // Import: the dialog opens as soon as files are chosen. It shows the check, then the list to confirm, then the copy and
    // each item's drafts; it closes when the data is ready for review.
    if (importPreviewWanted_ && !ImGui::IsPopupOpen("자료 임포트")) ImGui::OpenPopup("자료 임포트");
    bool closeImport = false;
    if (theme::beginDialog("자료 임포트", "자료 임포트", 640, &closeImport)) {
        const auto status = importProgress_;
        auto finish = [&] { importPreviewWanted_ = false; importProgress_.reset(); importPlan_.reset(); ImGui::CloseCurrentPopup(); };
        if (!importPreviewWanted_ || (!status && !importPlan_)) { importPreviewWanted_ = false; ImGui::CloseCurrentPopup(); }
        else if (closeImport) { if (status && !status->finished) status->cancelled = true; else finish(); }
        const float full = ImGui::GetContentRegionAvail().x, rowHeight = 55;
        auto draw = ImGui::GetWindowDrawList();
        // One row of the list: a capture or a folder of images, its numbered name, where it comes from, and on the right
        // its image count or how far it is.
        auto subtitle = [](const Json& item) {
            if (item.value("kind", "") == "capture") return item.value("name", "") + (item.contains("cameras") ? " · 카메라 " + std::to_string(item.value("cameras", 0)) + "대" : std::string());
            return item.value("folder", item.value("name", ""));
        };
        auto beginList = [&](int rows) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(theme::bg1)); ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::line));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0}); ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 0});
            ImGui::BeginChild("##importItems", {full, std::min(6, std::max(1, rows)) * rowHeight + 2}, ImGuiChildFlags_Borders);
        };
        auto endList = [] { ImGui::EndChild(); ImGui::PopStyleVar(3); ImGui::PopStyleColor(2); };
        int index = 0;
        auto row = [&](float left, theme::Icon glyph, ImU32 tint, ImU32 nameColor, const std::string& name, const std::string& sub, ImU32 rightColor, const std::string& right, ImU32 fill = 0, bool numeric = true) {
            auto list = ImGui::GetWindowDrawList();
            const ImVec2 at = ImGui::GetCursorScreenPos(); const float w = ImGui::GetContentRegionAvail().x;
            if (fill) list->AddRectFilled(at, {at.x + w, at.y + rowHeight}, fill);
            if (index++) list->AddLine({at.x, at.y + .5f}, {at.x + w, at.y + .5f}, theme::line);
            theme::icon(list, glyph, {at.x + left + 8, at.y + rowHeight / 2}, 14, tint);
            ImFont* rightFont = numeric ? theme::mono : nullptr;
            const float rightWidth = right.empty() ? 0 : theme::extent(right, theme::caption, rightFont).x;
            const float room = w - left - 26 - 24 - rightWidth;
            theme::write(list, {at.x + left + 26, at.y + 10}, nameColor, tailFit(name, room, theme::base, nullptr), theme::base);
            theme::write(list, {at.x + left + 26, at.y + 30}, theme::text3, tailFit(sub, room, theme::caption, nullptr), theme::caption);
            if (!right.empty()) theme::write(list, {at.x + w - 14 - rightWidth, at.y + rowHeight / 2 - theme::caption / 2}, rightColor, right, theme::caption, rightFont);
            return at;
        };
        if (status && importPlan_) {
            // The import itself: the bar and count of the current stage, every item's state, and the time left.
            std::string phase, outcome, reason; uint64_t done = 0, total = 0; int item = -1; std::vector<int> states; double remaining = -1; bool finished = false;
            { std::lock_guard guard(status->lock); phase = status->phase; done = status->done; total = status->total; item = status->item; states = status->states; remaining = status->remaining;
              finished = status->finished; outcome = status->outcome; reason = status->reason; }
            if (finished) {   // how it ended: what came in, what failed and why
                int captures = 0, came = 0, failedItems = 0; uint64_t images = 0;
                for (size_t i = 0; i < status->items.size(); ++i) {
                    const int state = i < states.size() ? states[i] : 0;
                    if (state == 3) { const auto& entry = status->items[i]; ++came; captures += entry.value("kind", "") == "capture"; images += entry.value("images", uint64_t(0)); }
                    failedItems += state == 5;
                }
                std::string line = outcome == "done" ? "완료" : outcome == "partial" ? "일부 완료" : outcome == "stopped" ? "중단" : "실패";
                if (came) { if (captures) line += " · 촬영 " + std::to_string(captures) + "개"; if (images) line += " · 이미지 " + grouped(int(images)) + "장"; }
                else line += " · 프로젝트는 그대로";
                if (failedItems) line += " · 실패 " + std::to_string(failedItems) + "개";
                const ImU32 tone = outcome == "done" ? theme::ok : outcome == "stopped" ? theme::hold : outcome == "partial" ? theme::warn : theme::warn;
                theme::progressBar({full, 6}, came ? float(came) / float(std::max<size_t>(1, status->items.size())) : 0.f);
                ImGui::Dummy({full, 10});
                theme::write(draw, ImGui::GetCursorScreenPos(), tone, line, theme::base, theme::bold);
                ImGui::Dummy({full, 24});
                std::vector<std::string> notes;   // why: each failed item, or the one reason when nothing came in
                for (size_t i = 0; i < status->items.size(); ++i)
                    if (i < states.size() && states[i] == 5 && i < status->reasons.size()) notes.push_back((i < status->titles.size() ? status->titles[i] : std::string()) + " · " + status->reasons[i]);
                if (notes.empty() && !reason.empty()) notes.push_back(reason);
                for (const auto& note : notes) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::text2)); ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + full);
                    ImGui::TextUnformatted(note.c_str()); ImGui::PopTextWrapPos(); ImGui::PopStyleColor();
                }
                if (!notes.empty()) ImGui::Dummy({full, 4});
                beginList(int(status->items.size()));
                for (size_t i = 0; i < status->items.size(); ++i) {
                    const auto& entry = status->items[i]; const int state = i < states.size() ? states[i] : 0;
                    const bool ok = state == 3, broke = state == 5;
                    const std::string right = ok ? "완료" : broke ? "실패" : "되돌림";
                    const ImVec2 rowAt = row(14, entry.value("kind", "") == "capture" ? theme::Icon::Camera : theme::Icon::Image, theme::text3, theme::text,
                                             i < status->titles.size() ? status->titles[i] : entry.value("name", ""), subtitle(entry), ok ? theme::ok : broke ? theme::warn : theme::text3, right, 0, false);
                    if (ok) theme::check(ImGui::GetWindowDrawList(), {rowAt.x + ImGui::GetContentRegionAvail().x - 22 - theme::extent(right, theme::caption).x, rowAt.y + rowHeight / 2}, 10, theme::ok);
                    ImGui::Dummy({ImGui::GetContentRegionAvail().x, rowHeight});
                }
                endList();
                theme::dialogFooter();
                const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
                if (theme::footerButton("확인", "Enter", true) || enter) finish();
                theme::endDialog();
                return;
            }
            theme::progressBar({full, 6}, total ? float(double(done) / double(total)) : -1.f);
            ImGui::Dummy({full, 10});
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const bool copying = phase == "원본 복사" || phase == "복사 확인" || phase == "장면 구성";
            const std::string title = item >= 0 && item < int(status->titles.size()) ? status->titles[size_t(item)] : std::string();
            const std::string amount = !total ? std::string() : copying ? sizeLabel(done) + " / " + sizeLabel(total) : grouped(int(done)) + " / " + grouped(int(total));
            const float amountWidth = amount.empty() ? 0 : theme::extent(amount, theme::caption, theme::mono).x;
            theme::write(draw, at, theme::text2, tailFit(title.empty() ? phase : title + " · " + phase, full - amountWidth - 16, theme::caption, nullptr), theme::caption);
            if (!amount.empty()) theme::write(draw, {at.x + full - amountWidth, at.y}, theme::text2, amount, theme::caption, theme::mono);
            ImGui::Dummy({full, 24});
            beginList(int(status->items.size()));
            for (size_t i = 0; i < status->items.size(); ++i) {
                const auto& entry = status->items[i]; const int state = i < states.size() ? states[i] : 0;
                std::string right = "대기"; ImU32 color = theme::text3;
                if (state == 3) { right = "완료"; color = theme::ok; }
                else if (state == 4) right = "복사됨";
                else if (state == 1) { right = "복사 중"; color = theme::accent; }
                else if (state == 2) { right = phase == "반복 장면 거르기" ? "반복 장면 거르는 중" : phase == "원 맞춤" ? "원 맞추는 중" : "초안 생성 중"; color = theme::accent; }
                const ImVec2 rowAt = row(14, entry.value("kind", "") == "capture" ? theme::Icon::Camera : theme::Icon::Image, theme::text3, theme::text,
                                         i < status->titles.size() ? status->titles[i] : entry.value("name", ""), subtitle(entry), color, right, 0, false);
                if (state == 3) theme::check(ImGui::GetWindowDrawList(), {rowAt.x + ImGui::GetContentRegionAvail().x - 22 - theme::extent(right, theme::caption).x, rowAt.y + rowHeight / 2}, 10, theme::ok);
                ImGui::Dummy({ImGui::GetContentRegionAvail().x, rowHeight});
            }
            endList();
            std::string note;
            if (status->cancelled) note = "중단하는 중";
            else if (remaining >= 0) note = remaining < 45 ? "1분 미만 남음" : "약 " + std::to_string(int(std::ceil(remaining / 60))) + "분 남음";
            theme::dialogFooter(note);
            if (theme::footerButton("중단", "Esc", false, !status->cancelled)) status->cancelled = true;
        } else if (status) {
            // The check: what is being read, and how many of the found captures and images are done.
            std::string phase; uint64_t done = 0, total = 0;
            { std::lock_guard guard(status->lock); phase = status->phase; done = status->done; total = status->total; }
            const ImVec2 at = ImGui::GetCursorScreenPos();
            theme::spinner(draw, {at.x + 7, at.y + 9}, 6, theme::accent);
            theme::write(draw, {at.x + 22, at.y + 1}, theme::text, tailFit(phase, full - 22 - 160, theme::base, nullptr), theme::base);
            const std::string amount = total ? grouped(int(done)) + " / " + grouped(int(total)) : "";
            if (!amount.empty()) theme::write(draw, {at.x + full - theme::extent(amount, theme::caption, theme::mono).x, at.y + 2}, theme::text2, amount, theme::caption, theme::mono);
            ImGui::Dummy({full, 20});
            theme::progressBar({full, 8}, total ? float(double(done) / double(total)) : -1.f);
            if (status->cancelled) theme::write(draw, {at.x, at.y + 42}, theme::hold, "중단하는 중", theme::caption);
            ImGui::Dummy({full, 6});
            theme::dialogFooter();
            if (theme::footerButton("중단", "Esc", false, !status->cancelled)) status->cancelled = true;
        } else if (importPlan_) {   // nothing to show on the frame the dialog closes after an import
            // The list to confirm: every item can be left out; captures are numbered as they will be named.
            if (importChosen_.size() != importPlan_->items.size()) importChosen_.assign(importPlan_->items.size(), 1);
            const auto summary = importSummary_.is_object() ? importSummary_ : Json::object();
            const auto skipped = summary.value("skipped", Json::array()), errors = summary.value("errors", Json::array());
            int numbered = 0, captures = 0; uint64_t images = 0, bytes = 0; bool any = false;
            for (const auto& entry : importPlan_->base.at("imports")) numbered += entry.at("kind") == "capture";
            for (size_t i = 0; i < importPlan_->items.size(); ++i) if (importChosen_[i]) {
                const auto& entry = importPlan_->items[i]; any = true;
                captures += entry.value("kind", "") == "capture"; images += entry.value("images", uint64_t(0)); bytes += entry.value("bytes", uint64_t(0));
            }
            {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                char line[160]{}; std::snprintf(line, sizeof line, "촬영 %d개 · 이미지 %s장", captures, grouped(int(images)).c_str());
                theme::write(draw, at, theme::text, line, theme::base, theme::bold);
                const auto size = sizeLabel(bytes);
                theme::write(draw, {at.x + full - theme::extent(size, theme::caption, theme::mono).x, at.y + 1}, theme::text2, size, theme::caption, theme::mono);
                ImGui::Dummy({full, 18});
            }
            beginList(int(importPlan_->items.size() + skipped.size() + errors.size()));
            for (size_t i = 0; i < importPlan_->items.size(); ++i) {
                const auto& entry = importPlan_->items[i]; const bool on = importChosen_[i] != 0, capture = entry.value("kind", "") == "capture";
                const std::string name = capture && on ? captureTitle(++numbered) : entry.value("name", "");
                ImGui::PushID(int(i));
                const ImVec2 at = ImGui::GetCursorScreenPos(); const float w = ImGui::GetContentRegionAvail().x;
                if (ImGui::InvisibleButton("##choose", {w, rowHeight})) importChosen_[i] = !on;
                const bool hovered = ImGui::IsItemHovered();
                ImGui::SetCursorScreenPos(at);
                auto list = ImGui::GetWindowDrawList();
                if (hovered) list->AddRectFilled(at, {at.x + w, at.y + rowHeight}, theme::bg2);
                row(40, capture ? theme::Icon::Camera : theme::Icon::Image, on ? theme::text3 : theme::bg4, on ? theme::text : theme::text3, name, subtitle(entry),
                    on ? theme::text2 : theme::text3, grouped(entry.value("images", 0)) + "장");
                const ImVec2 box{at.x + 16, at.y + rowHeight / 2 - 7};
                if (on) { list->AddRectFilled(box, {box.x + 14, box.y + 14}, theme::accent, 3); theme::check(list, {box.x + 7, box.y + 7}, 9, theme::ink); }
                else list->AddRect(box, {box.x + 14, box.y + 14}, theme::line2, 3, 0, 1.5f);
                ImGui::Dummy({w, rowHeight});
                ImGui::PopID();
            }
            for (const auto& file : skipped) { row(40, theme::Icon::Info, theme::text3, theme::text2, "이미 있는 자료", file.get<std::string>(), theme::text3, ""); ImGui::Dummy({ImGui::GetContentRegionAvail().x, rowHeight}); }
            for (const auto& error : errors) {
                row(40, theme::Icon::Info, theme::warn, theme::text, "제외 · " + error.at("error").get<std::string>(), error.at("file").get<std::string>(), theme::text3, "", theme::rgb(0xff8a4c, 14));
                ImGui::Dummy({ImGui::GetContentRegionAvail().x, rowHeight});
            }
            endList();
            theme::dialogFooter();
            if (theme::footerButton("임포트", nullptr, true, !busy() && any)) later_ = [this] { commitPlannedImport(); };
            if (theme::footerButton("취소", "Esc", false)) { importPreviewWanted_ = false; importPlan_.reset(); ImGui::CloseCurrentPopup(); }
        } else ImGui::Dummy({full, 1});   // the dialog's body starts below its title; something must stand there
        theme::endDialog();
    }
}
// The data tab: the imported captures and folders of images, each with a thumbnail, its scenes and images left for
// review after the repeated frames, and how many of those scenes are confirmed. Importing is the only action here.
void Application::preparePage() {
    auto draw = ImGui::GetWindowDrawList();
    const float full = ImGui::GetContentRegionAvail().x;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    theme::write(draw, {top.x, top.y + 6}, theme::text, "자료", theme::heading, theme::bold);
    const float imagesWidth = 132, folderWidth = 118;
    ImGui::SetCursorScreenPos({top.x + full - folderWidth - 8 - imagesWidth, top.y});
    const bool open = !workspace_.empty() && !busy();
    if (theme::button("이미지 임포트", {imagesWidth, 34}, false, theme::Icon::Image, open)) later_ = [this] { importData(); };
    ImGui::SameLine(0, 8);
    if (theme::button("폴더 임포트", {folderWidth, 34}, false, theme::Icon::Folder, open)) later_ = [this] { chooseImportFolder(); };
    ImGui::SetCursorScreenPos({top.x, top.y + 50});
    if (workspace_.empty()) {   // an older label project opened from its file: its data is reviewed, not imported
        theme::write(draw, {top.x, top.y + 54}, theme::text3, "기존 라벨 작업 · 자료를 추가하려면 새 프로젝트를 만드세요", theme::caption);
        ImGui::Dummy({full, 24});
    }
    if (!catalog_.is_object()) return;
    const auto& recordings = catalog_.at("recordings");
    if (recordings.empty()) {
        const ImVec2 at = ImGui::GetCursorScreenPos(); const float height = 340;
        for (float x = at.x; x < at.x + full; x += 8) { draw->AddLine({x, at.y}, {std::min(x + 4, at.x + full), at.y}, theme::line2); draw->AddLine({x, at.y + height}, {std::min(x + 4, at.x + full), at.y + height}, theme::line2); }
        for (float y = at.y; y < at.y + height; y += 8) { draw->AddLine({at.x, y}, {at.x, std::min(y + 4, at.y + height)}, theme::line2); draw->AddLine({at.x + full, y}, {at.x + full, std::min(y + 4, at.y + height)}, theme::line2); }
        const float middle = at.x + full / 2;
        theme::icon(draw, theme::Icon::Download, {middle, at.y + 140}, 20, theme::text2);
        const std::string empty = "임포트한 자료가 없습니다", hint = "폴더나 이미지를 끌어 놓기";
        theme::write(draw, {middle - theme::extent(empty, theme::base, theme::bold).x / 2, at.y + 172}, theme::text, empty, theme::base, theme::bold);
        theme::write(draw, {middle - theme::extent(hint, theme::caption).x / 2, at.y + 198}, theme::text3, hint, theme::caption);
        ImGui::Dummy({full, height});
        return;
    }
    // Per recording: the scenes with an image left for review, those whose images are all confirmed, and those images.
    struct Count { int scenes = 0, reviewed = 0, images = 0; std::string thumb; bool color = false; };
    std::map<std::string, Count> counts;
    for (const auto& scene : catalog_.at("scenes")) {
        auto& count = counts[scene.at("recording").get<std::string>()];
        int kept = 0, confirmed = 0;
        for (const auto& member : scene.at("members")) {
            if (count.thumb.empty()) { count.thumb = member.at("key").get<std::string>(); count.color = member.value("kind", "") == "컬러"; }
            if (!member.value("selected", true)) continue;
            ++kept; confirmed += member.value("reviewed", false);
        }
        if (!kept) continue;
        ++count.scenes; count.reviewed += confirmed == kept; count.images += kept;
    }
    const float x0 = ImGui::GetCursorScreenPos().x, sceneX = full * .42f, imageX = full * .57f, reviewX = full * .715f;
    const float headerHeight = 30, groupHeight = 36, rowHeight = 62;
    int captures = 0, folders = 0;
    for (const auto& recording : recordings) (recording.value("source_type", "capture") == "images" ? folders : captures)++;
    const float height = headerHeight + (captures ? groupHeight + captures * rowHeight : 0) + (folders ? groupHeight + folders * rowHeight : 0);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(at, {at.x + full, at.y + height}, theme::bg1, 8);
    draw->AddRect(at, {at.x + full, at.y + height}, theme::line, 8);
    float y = at.y;
    for (const auto& [label, x] : {std::pair<const char*, float>{"자료", 88.f}, {"장면", sceneX}, {"이미지", imageX}, {"검수", reviewX}})
        theme::write(draw, {x0 + x, y + headerHeight / 2 - theme::caption / 2}, theme::text3, label, theme::caption);
    y += headerHeight;
    for (const bool images : {false, true}) {
        const int members = images ? folders : captures;
        if (!members) continue;
        draw->AddRectFilled({at.x + 1, y}, {at.x + full - 1, y + groupHeight}, theme::bg2);
        draw->AddLine({at.x, y + .5f}, {at.x + full, y + .5f}, theme::line);
        theme::icon(draw, images ? theme::Icon::Image : theme::Icon::Camera, {x0 + 24, y + groupHeight / 2}, 13, theme::text2);
        const std::string group = images ? "일반 이미지" : "카메라 촬영";
        theme::write(draw, {x0 + 44, y + groupHeight / 2 - theme::sub / 2}, theme::text, group, theme::sub, theme::bold);
        theme::write(draw, {x0 + 52 + theme::extent(group, theme::sub, theme::bold).x, y + groupHeight / 2 - theme::caption / 2}, theme::text3, std::to_string(members), theme::caption);
        y += groupHeight;
        for (const auto& recording : recordings) {
            if ((recording.value("source_type", "capture") == "images") != images) continue;
            const auto id = recording.at("id").get<std::string>(); const auto& count = counts[id];
            draw->AddLine({at.x, y + .5f}, {at.x + full, y + .5f}, theme::line);
            const ImVec2 a{x0 + 16, y + 10}, b{x0 + 72, y + 52};
            draw->AddRectFilled(a, b, theme::bg3, 4);
            if (!count.thumb.empty()) if (const GLuint texture = thumb(count.thumb, count.color)) {
                ImVec2 uv0{0, 0}, uv1{1, 1};   // the middle of a picture that is not 4:3
                if (recording.contains("image_size")) {
                    const double w = recording["image_size"][0], h = recording["image_size"][1], want = 56.0 / 42.0;
                    if (w > 0 && h > 0) { if (w / h > want) { const float cut = float((1 - want * h / w) / 2); uv0.x = cut; uv1.x = 1 - cut; } else { const float cut = float((1 - w / h / want) / 2); uv0.y = cut; uv1.y = 1 - cut; } }
                }
                draw->AddImageRounded(ImTextureRef(ImTextureID(texture)), a, b, uv0, uv1, IM_COL32_WHITE, 4);
            }
            std::string sub;
            if (images) {
                sub = recording.value("format", "");
                if (recording.contains("image_size")) sub += (sub.empty() ? "" : " · ") + std::to_string(recording["image_size"][0].get<int>()) + " × " + std::to_string(recording["image_size"][1].get<int>());
            } else {
                sub = "카메라 " + std::to_string(recording.at("cameras").size()) + "대";
                const auto started = catalog_.at("stats").value(id, Json::object()).value("started", Json());
                if (started.is_string()) sub += " · " + started.get<std::string>();
            }
            const float room = sceneX - 88 - 16;
            theme::write(draw, {x0 + 88, y + 13}, theme::text, tailFit(recording.value("title", recording.value("name", "")), room, theme::base, nullptr), theme::base);
            theme::write(draw, {x0 + 88, y + 34}, theme::text3, tailFit(sub, room, theme::caption, nullptr), theme::caption);
            theme::write(draw, {x0 + sceneX, y + rowHeight / 2 - theme::base / 2}, theme::text, grouped(count.scenes), theme::base, theme::mono);
            theme::write(draw, {x0 + imageX, y + rowHeight / 2 - theme::base / 2}, theme::text, grouped(count.images), theme::base, theme::mono);
            theme::write(draw, {x0 + reviewX, y + 14}, theme::text, grouped(count.reviewed) + " / " + grouped(count.scenes), theme::base, theme::mono);
            const float barWidth = std::min(260.f, full - reviewX - 40);
            draw->AddRectFilled({x0 + reviewX, y + 39}, {x0 + reviewX + barWidth, y + 43}, theme::bg4, 2);
            if (count.scenes && count.reviewed) draw->AddRectFilled({x0 + reviewX, y + 39}, {x0 + reviewX + std::max(4.f, barWidth * float(count.reviewed) / float(count.scenes)), y + 43}, theme::ok, 2);
            y += rowHeight;
        }
    }
    ImGui::Dummy({full, height + 8});
}
void Application::projectUiCheck(int step, const fs::path& first, const fs::path& second) {
    if (step == 0) {
        if (!ready()) throw std::runtime_error("Project check needs a frame");
        current().annotation["boxes"] = Json::array({{{"bbox", {8, 8, 12, 12}}, {"source", "manual"}, {"color", "unknown"}, {"occluded", false}}});
        current().dirty = true; openProject(second);
    } else if (step == 1) { if (workspace_ != workspaceRoot(second)) throw std::runtime_error("Second project not activated"); openProject(first); }
    else { if (workspace_ != workspaceRoot(first) || !ready() || current().annotation.at("boxes").empty()) throw std::runtime_error("Saved label lost across project switch"); }
}
}
