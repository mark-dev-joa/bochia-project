// The labeler's side of the hub (design/hub/DESIGN.md): the worker dialog, the client, the status it sends twice a
// second, what /register carries and the commands it answers. The events are the log lines (labeler_log.hpp).
#include "native_labeler_app.hpp"
#include "native_labeler_theme.hpp"
#include "labeler_log.hpp"
#include "labeler_sam.hpp"
#include "yolo_detector.hpp"
#include "build_info.hpp"
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace boccia::labeler {
namespace fs = std::filesystem;
namespace {
void fill(char* buffer, size_t size, const std::string& text) {
    const size_t count = std::min(size - 1, text.size());
    std::memcpy(buffer, text.data(), count);
    buffer[count] = 0;
}
std::string trimmed(const char* text) {
    std::string result = text;
    while (!result.empty() && (result.back() == ' ' || result.back() == '\t' || result.back() == '\r' || result.back() == '\n')) result.pop_back();
    const auto start = result.find_first_not_of(" \t");
    return start == std::string::npos ? std::string() : result.substr(start);
}
}

// --hub --worker --token: a hidden check joins with these instead of the PC's settings; nothing is saved.
void Application::hubForCheck(const std::string& address, const std::string& worker, const std::string& token) {
    settings_.hubAddress = address; settings_.worker = worker; settings_.token = token;
    startHub();
}

void Application::startHub() {
    if (settings_.worker.empty() || settings_.hubAddress.empty()) { setLogSink(nullptr); hub_.reset(); return; }
    hub::Settings made;
    made.address = settings_.hubAddress; made.token = settings_.token; made.worker = settings_.worker;
    made.pc = hub::hostName(); made.account = hub::accountName(); made.app = "labeler"; made.version = boccia::versionString();
    hubStarted_ = std::chrono::steady_clock::now();
    if (hub_) { hub_->setSettings(made); return; }
    wchar_t location[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", location, 32768);
    // The registration and the log sink hold nothing of the app itself: the client's threads may outlive a frame of it.
    hub_ = std::make_shared<hub::Client>(made, fs::path(location) / "Boccia/hub-outbox", [] { return logFile(); },
                                         [shared = hubShared_] { std::lock_guard guard(shared->lock); return shared->registration; });
    setLogSink([weak = std::weak_ptr<hub::Client>(hub_)](const std::string& line) { if (const auto client = weak.lock()) client->event(line); });
}

// A hidden check with --hub waits for the hub's answer (or 5 s) before it captures and ends.
bool Application::hubSettled() const {
    return !hub_ || hub_->state() != hub::Client::State::Offline || std::chrono::steady_clock::now() - hubStarted_ > std::chrono::seconds(5);
}

void Application::openWorkerDialog() {
    fill(workerName_, sizeof workerName_, settings_.worker);
    fill(workerToken_, sizeof workerToken_, settings_.token);
    fill(hubAddress_, sizeof hubAddress_, settings_.hubAddress);
    workerDialog_ = true;
}

void Application::saveWorker() {
    auto settings = loadLabelerSettings();   // the file as it is now, so the 실행 장치 stays
    settings.worker = trimmed(workerName_); settings.token = trimmed(workerToken_); settings.hubAddress = trimmed(hubAddress_);
    if (settings.hubAddress.empty()) settings.hubAddress = LabelerSettings{}.hubAddress;
    try { saveLabelerSettings(settings); } catch (const std::exception& error) { error_ = error.what(); return; }
    settings_.worker = settings.worker; settings_.token = settings.token; settings_.hubAddress = settings.hubAddress;
    logLine(settings.worker.empty() ? std::string("작업자 없음") : "작업자 · " + settings.worker + " · " + settings.hubAddress);
    startHub();
    workerDialog_ = false;
}

// The worker dialog: the name the hub shows, the invite code it gave and where it is. Opens by itself on a PC without a name.
void Application::workerPopup() {
    if (workerDialog_ && !ImGui::IsPopupOpen("작업자")) ImGui::OpenPopup("작업자");
    bool close = false;
    if (theme::beginDialog("작업자", "작업자", 460, &close)) {
        if (!workerDialog_ || close) { workerDialog_ = false; ImGui::CloseCurrentPopup(); }
        const float full = ImGui::GetContentRegionAvail().x;
        theme::fieldLabel("이름");
        theme::textField("##workerName", workerName_, sizeof workerName_, full, true);
        ImGui::Dummy({full, 10});
        theme::fieldLabel("초대 코드");
        theme::textField("##workerToken", workerToken_, sizeof workerToken_, full);
        ImGui::Dummy({full, 10});
        theme::fieldLabel("허브 주소");
        theme::textField("##hubAddress", hubAddress_, sizeof hubAddress_, full);
        if (hub_ && hub_->state() == hub::Client::State::Unauthorized) {
            ImGui::Dummy({full, 8});
            theme::write(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), theme::warn, "초대 코드가 맞지 않습니다", theme::caption);
            ImGui::Dummy({full, 18});
        }
        theme::dialogFooter();
        const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        if (theme::footerButton("저장", "Enter", true) || enter) later_ = [this] { saveWorker(); };
        if (theme::footerButton("취소", "Esc", false)) { workerDialog_ = false; ImGui::CloseCurrentPopup(); }
        theme::endDialog();
    }
}

hub::Registration Application::currentRegistration() const {
    hub::Registration made;
    made.device = Json{{"gpu", modelsOnGpu()}, {"cuda", !yoloCudaRuntime().empty()}, {"problem", gpuProblem()}};
    Json summary = Json::object();
    if (projectOpened()) {
        summary["project"] = workspace_.empty() ? std::string("기존 라벨 작업") : workspaceName_;
        summary["path"] = utf8(workspace_.empty() ? config_.project : workspace_);
        const Json totals = catalog_.value("summary", Json::object());
        for (const char* key : {"target", "reviewed", "hold", "total", "scenes", "revision"}) if (totals.contains(key)) summary[key] = totals[key];
    }
    made.summary = summary;
    return made;
}

Json Application::hubScene() const {
    if (!ready() || !catalog_.contains("scenes") || scene_ < 0 || scene_ >= int(catalog_["scenes"].size())) return nullptr;
    const auto& scene = catalog_["scenes"][scene_];
    char recording[48];
    std::snprintf(recording, sizeof recording, "%s %02d", imageScene() ? "이미지 자료" : "촬영", scene.value("recording_number", 0));
    return Json{{"recording", recording}, {"index", scene.value("index", 0)}, {"image", imageScene()}};
}

std::string Application::hubSceneName() const {
    const Json scene = hubScene();
    if (scene.is_null()) return "장면 없음";
    char frame[16];
    std::snprintf(frame, sizeof frame, "%04d", scene.value("index", 0));
    return scene.value("recording", "") + (scene.value("image", false) ? " · 이미지 " : " · 프레임 ") + frame;
}

Json Application::hubJob() const {
    if (importing()) {
        std::lock_guard guard(importProgress_->lock);
        const uint64_t done = importProgress_->done, total = importProgress_->total;
        return Json{{"kind", "import"}, {"text", "임포트 · " + importProgress_->phase + (total ? " " + grouped(int(done)) + " / " + grouped(int(total)) : std::string())},
                    {"fraction", total ? double(done) / double(total) : -1.}};
    }
    struct Running { const Json* state; const char* kind; std::string label; };
    const Running jobs[] = {{&exportStatus_, "export", "내보내기"}, {&redraftStatus_, "redraft", "초안 " + redraftStatus_.value("stage", "")},
                            {&missingStatus_, "missing", "빠진 공 찾기"}, {&occlusionStatus_, "occlusion", "가려진 공 표시"}, {&circleStatus_, "circles", "원형 검출"},
                            {&propagationStatus_, "propagate", "전파"}, {&colorStatus_, "color", "색 채우기"}};
    for (const auto& job : jobs) {
        if (job.state->value("status", "") != "running" || (job.state == &exportStatus_ && exportDemo_)) continue;
        const int done = job.state->value("done", 0), total = job.state->value("total", 0);
        return Json{{"kind", job.kind}, {"text", job.label + (total ? " " + grouped(done) + " / " + grouped(total) : std::string())}, {"fraction", total ? double(done) / total : -1.}};
    }
    return nullptr;
}

// At the end of every tick: the status when it changed, the registration the next knock sends, and the commands.
void Application::hubTick() {
    if (!hub_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - hubTicked_ < std::chrono::milliseconds(500)) return;
    hubTicked_ = now;
    const std::string page = page_ == Page::Data ? "data" : page_ == Page::Export ? "export" : "review";
    hub_->setStatus(hubStatusJson(page, hubScene(), hubJob(), applyPrevious_, modelsOnGpu(), error_));
    { std::lock_guard guard(hubShared_->lock); hubShared_->registration = currentRegistration(); }
    for (const auto& command : hub_->takeCommands()) {
        if (command.name == "notify") { hubNotice_ = command.value; logLine("허브 알림 · " + command.value); hub_->finish(command, true, "띄움"); }
        else if (command.name == "log") {
            const auto file = logFile();
            if (file.empty()) hub_->finish(command, false, "로그 없음");
            else { hub_->upload("log", file); hub_->finish(command, true, utf8(file.filename())); }
        }
        else if (command.name == "ping") hub_->finish(command, true, "pong");
        else hub_->finish(command, false, "모르는 명령");
    }
}
}
