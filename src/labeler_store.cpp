#include "labeler_store.hpp"
#include "labeler_assist.hpp"
#include "labeler_util.hpp"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace boccia::labeler {
namespace fs = std::filesystem;
namespace {
bool truthy(const Json& value) { return value.is_boolean() ? value.get<bool>() : !value.is_null() && !(value.is_number() && value.get<double>() == 0) && !(value.is_object() && value.empty()) && !(value.is_array() && value.empty()); }
std::optional<int64_t> modifiedStamp(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return std::nullopt;
    return int64_t((uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime);
}
std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("파일을 읽지 못했습니다: " + utf8(path));
    std::string text((std::istreambuf_iterator<char>(in)), {});
    return text.substr(0, 3) == "\xEF\xBB\xBF" ? text.substr(3) : text;
}
bool inside(const fs::path& inner, const fs::path& outer) {
    const auto relative = inner.lexically_relative(outer);
    return !relative.empty() && relative.native().rfind(L"..", 0) != 0;
}
}
fs::path selectionPath(const fs::path& project) { return project.parent_path() / (project.stem().wstring() + L"-selection.json"); }
SelectionFile::SelectionFile(const Catalog& catalog, fs::path sidecar) : path(std::move(sidecar)), catalog_(catalog) {}
Json SelectionFile::read() {
    const auto stamp = modifiedStamp(path);
    if (stamp == stamp_) return value_;
    if (!stamp) { value_ = nullptr; stamp_ = std::nullopt; return nullptr; }
    Json value = Json::parse(readText(path));
    if (value.value("version", 0) != 1 || value.value("source_signature", "") != catalog_.signature) throw std::runtime_error("대표 프레임 필터와 촬영 데이터가 다릅니다.");
    const auto decisions = value.find("frames");
    if (decisions == value.end() || !decisions->is_object()) throw std::runtime_error("대표 프레임 필터 형식이 올바르지 않습니다.");
    for (const auto& [key, item] : decisions->items()) {
        const auto first = key.find('/'), second = key.find('/', first + 1);
        if (first == std::string::npos || second == std::string::npos) throw std::runtime_error("촬영 또는 카메라를 찾지 못했습니다.");
        catalog_.frameInfo(key.substr(0, first), key.substr(first + 1, second - first - 1), std::stoi(key.substr(second + 1)));
        if (!item.is_object() || !item.contains("selected") || !item["selected"].is_boolean()) throw std::runtime_error("대표 프레임 선택 상태가 올바르지 않습니다.");
        const std::string representative = item.value("representative", key);
        if (!item["selected"].get<bool>()) {
            const auto other = decisions->find(representative);
            if (representative.substr(0, representative.rfind('/')) != key.substr(0, key.rfind('/')) || other == decisions->end() || !other->value("selected", false))
                throw std::runtime_error("중복 프레임은 같은 촬영·카메라의 대표 프레임을 참조해야 합니다.");
        }
    }
    value_ = std::move(value); stamp_ = stamp;
    return value_;
}
Json SelectionFile::view(const Json& annotations) {
    const Json value = read();
    if (value.is_null() || !truthy(value)) return {{"enabled", false}, {"frames", Json::object()}};
    Json decisions = value["frames"];
    // Human edits after selection must never silently vanish from training.
    for (const auto& [key, frame] : annotations.items())
        if (truthy(frame.value("human_touched", Json(false))) || truthy(frame.value("reviewed", Json(false))))
            decisions[key] = {{"selected", true}, {"representative", key}, {"reason", "human_preserved"}};
    const int total = catalog_.totalFrames();
    int excluded = 0;
    for (const auto& [key, item] : decisions.items()) if (!item["selected"].get<bool>()) ++excluded;
    return {{"enabled", true}, {"path", utf8(path)}, {"created_at", value.value("created_at", Json())}, {"algorithm", value.value("algorithm", Json())},
            {"rules", value.value("rules", Json())}, {"total", total}, {"selected", total - excluded}, {"excluded", excluded},
            {"coverage", value["frames"].size()}, {"frames", std::move(decisions)}};
}
Project::Project(const Catalog& source, const fs::path& project, const fs::path& exports)
    : catalog(source), path(fs::weakly_canonical(project).make_preferred()), exportRoot(fs::weakly_canonical(exports).make_preferred()), selection(source, selectionPath(path)) {
    bool plainImages = true;
    for (const auto& recording : catalog.recordings) for (const auto& stream : recording.cameras) if (!stream.rawPath.empty()) plainImages = false;
    if (plainImages && (exportRoot == catalog.source || inside(exportRoot, catalog.source)))
        throw std::runtime_error("이미지 입력 폴더 밖에 --export-root를 지정하세요. 내보낸 이미지가 다시 입력에 포함되지 않아야 합니다.");
    if (fs::exists(path)) {
        state = Json::parse(readText(path));
        if (state.value("version", 0) != 1 || state.value("source_signature", "") != catalog.signature)
            throw std::runtime_error("기존 라벨 프로젝트와 촬영 데이터가 다릅니다. 입력 폴더에 촬영을 넣거나 뺐다면 원래대로 되돌리면 다시 열립니다. 다른 촬영을 새로 시작하려면 --project로 다른 저장 파일을 지정하세요.");
    } else {
        Json splits = Json::object();
        for (const auto& recording : catalog.recordings) splits[recording.id] = "unassigned";
        state = {{"version", 1}, {"source", utf8(catalog.source)}, {"source_signature", catalog.signature}, {"revision", 0}, {"splits", splits}, {"frames", Json::object()}};
    }
}
Project::~Project() {
    colorWorker.request_stop();
    if (colorWorker.joinable()) colorWorker.join();
    if (exportWorker.joinable()) exportWorker.join();
}
void Project::write(const Json& replacement) const { atomicJson(path, replacement, replacement.value("frames", Json::object()).size() < 128); }
Json Project::summary() const {
    std::lock_guard guard(lock);
    const Json selected = selection.view(state["frames"]);
    const Json& decisions = selected["frames"];
    Json frames = Json::object();
    int reviewed = 0, boxes = 0, exportFrames = 0, exportBoxes = 0, automatic = 0, drafts = 0;
    for (const auto& [key, item] : state["frames"].items()) {
        const bool done = truthy(item.value("reviewed", Json(false))), touched = truthy(item.value("human_touched", Json(false)));
        const bool isAutomatic = item.contains("automatic") && truthy(item["automatic"]);
        const int count = int(item.value("boxes", Json::array()).size());
        const Json decision = decisions.contains(key) ? decisions[key] : Json::object();
        const bool included = decision.value("selected", true);
        frames[key] = {{"reviewed", done}, {"count", count},
                       {"review_status", item.contains("review_status") ? item["review_status"] : Json(done ? "reviewed" : touched ? "editing" : "draft")},
                       {"automatic", isAutomatic}, {"human_touched", item.value("human_touched", Json(false))},
                       {"needs_attention", item.contains("automatic") && item["automatic"].is_object() ? item["automatic"].value("needs_attention", Json(false)) : Json(false)},
                       {"selected", included}, {"representative", decision.value("representative", key)}, {"selection_reason", decision.value("reason", "unfiltered")}};
        reviewed += done; if (done) boxes += count; if (done && included) { ++exportFrames; exportBoxes += count; }
        automatic += isAutomatic; if (!done) drafts += count;
    }
    Json rest = Json::object();
    for (const auto& [key, value] : selected.items()) if (key != "frames") rest[key] = value;
    return {{"revision", state["revision"]}, {"splits", state["splits"]}, {"frames", std::move(frames)}, {"reviewed", reviewed}, {"boxes", boxes},
            {"selection", std::move(rest)}, {"export_frames", exportFrames}, {"export_boxes", exportBoxes}, {"auto_revision", state.value("auto_revision", 0)},
            {"automatic_frames", automatic}, {"draft_boxes", drafts}};
}
int Project::commitAutomatic(const std::vector<std::pair<std::string, Json>>& entries) {
    // Background additions have their own revision, so they cannot conflict with a manual revision. Any saved frame wins over the batch.
    std::lock_guard guard(lock);
    Json additions = Json::object();
    for (const auto& [key, item] : entries) if (!state["frames"].contains(key)) additions[key] = item;
    if (additions.empty()) return 0;
    Json replacement = state;
    for (auto& [key, item] : additions.items()) replacement["frames"][key] = std::move(item);
    replacement["auto_revision"] = replacement.value("auto_revision", 0) + 1;
    write(replacement);
    state = std::move(replacement);
    return int(additions.size());
}
Json Project::mutate(int expectedRevision, const std::function<void(Json&)>& callback) {
    std::lock_guard guard(lock);
    if (expectedRevision != state["revision"].get<int>()) throw Conflict("다른 창에서 변경했습니다. 이 창을 새로고침한 후 다시 작업하세요.");
    Json replacement = state;
    callback(replacement);
    replacement["revision"] = replacement["revision"].get<int>() + 1;
    write(replacement);
    state = std::move(replacement);
    return summary();
}
Json Project::mutateAutomatic(const std::function<void(Json&)>& callback) {
    std::lock_guard guard(lock);
    Json replacement = state;
    callback(replacement);
    replacement["auto_revision"] = replacement.value("auto_revision", 0) + 1;
    write(replacement);
    state = std::move(replacement);
    return summary();
}
Json Project::frame(const std::string& recording, const std::string& camera, int index) const {
    const auto& info = catalog.frameInfo(recording, camera, index);
    const auto& stream = catalog.camera(recording, camera);
    int width = info.width, height = info.height;
    cv::Mat image;
    if (info.isImage()) { image = catalog.read(recording, camera, index); width = image.cols; height = image.rows; }
    const std::string key = recording + "/" + camera + "/" + std::to_string(index);
    Json annotation, decisions; int revision = 0;
    {
        std::lock_guard guard(lock);
        annotation = state["frames"].contains(key) ? state["frames"][key] : Json{{"boxes", Json::array()}, {"reviewed", false}};
        decisions = selection.view(state["frames"])["frames"];
        revision = state["revision"];
    }
    // The colour camera says which ball it is: a masked ball without a colour gets one from its pixels whenever the
    // frame is shown, and keeps it once the frame is saved or confirmed.
    if (stream.kind == "컬러") for (auto& box : annotation["boxes"]) if (box.contains("segmentation") && box.value("color", "unknown") == "unknown") {
        if (image.empty()) image = catalog.read(recording, camera, index);
        const auto colour = ballColor(image, decodeMaskChecked(box["segmentation"], width, height));
        if (colour != "unknown") box["color"] = colour;
    }
    for (auto& box : annotation["boxes"]) if (box.contains("segmentation")) box["contours"] = maskContours(decodeMaskChecked(box["segmentation"], width, height));
    Json result = {{"key", key}, {"recording", recording}, {"camera", camera}, {"index", index}, {"width", width}, {"height", height},
                   {"host_ns", info.hasHostNs ? std::to_string(info.hostNs) : std::to_string(index)}, {"revision", revision}};
    for (auto& [name, value] : annotation.items()) result[name] = std::move(value);
    result["selection"] = decisions.contains(key) ? decisions[key] : Json{{"selected", true}, {"reason", "unfiltered"}, {"representative", key}};
    return result;
}
Json Project::saveFrame(const Json& payload) {
    const std::string recording = payload.at("recording"), camera = payload.at("camera");
    const int index = payload.at("index").get<int>();
    const Json current = frame(recording, camera, index);
    const int width = current["width"], height = current["height"];
    const Json boxes = validateBoxes(payload.at("boxes"), width, height);
    const Json reviewedValue = payload.at("reviewed");
    if (!reviewedValue.is_boolean()) throw std::runtime_error("검수 상태가 올바르지 않습니다.");
    const bool reviewed = reviewedValue.get<bool>();
    const Json status = payload.value("review_status", Json(reviewed ? "reviewed" : "editing"));
    static const char* statuses[] = {"draft", "editing", "reviewed", "hold"};
    if (!status.is_string() || std::find(std::begin(statuses), std::end(statuses), status.get<std::string>()) == std::end(statuses) || (status == "reviewed") != reviewed)
        throw std::runtime_error("검수 상태와 완료 여부가 일치하지 않습니다.");
    if (catalog.camera(recording, camera).kind == "모노")
        for (const auto& box : boxes) if (box.value("color", "unknown") != "unknown") throw std::runtime_error("모노 이미지의 색상은 미지정으로 유지하세요.");
    const std::string key = current["key"];
    return mutate(payload.at("expected_revision").get<int>(), [&](Json& next) {
        const Json previous = next["frames"].contains(key) ? next["frames"][key] : Json::object();
        Json entry = {{"recording", recording}, {"camera", camera}, {"index", index}, {"width", width}, {"height", height}, {"boxes", boxes},
                      {"reviewed", reviewed}, {"review_status", status}, {"human_touched", true}};
        if (previous.contains("automatic")) entry["automatic"] = previous["automatic"];
        next["frames"][key] = std::move(entry);
        next.erase("native_split");
    });
}
Json Project::saveSplits(const Json& payload) {
    const Json splits = payload.at("splits");
    bool valid = splits.is_object();
    if (valid) for (const auto& [recording, split] : splits.items()) {
        if (!catalog.find(recording) || !split.is_string()) { valid = false; break; }
        const std::string name = split;
        if (name != "unassigned" && std::find(std::begin(datasetSplits), std::end(datasetSplits), name) == std::end(datasetSplits)) { valid = false; break; }
    }
    if (!valid) throw std::runtime_error("촬영별 분할이 올바르지 않습니다.");
    return mutate(payload.at("expected_revision").get<int>(), [&](Json& next) { for (const auto& [recording, split] : splits.items()) next["splits"][recording] = split; });
}
std::vector<std::pair<std::string, Json>> selectedReviewed(const Json& snapshot) {
    const Json decisions = snapshot.contains("frame_selection") && snapshot["frame_selection"].is_object() ? snapshot["frame_selection"].value("frames", Json::object()) : Json::object();
    std::vector<std::pair<std::string, Json>> result;
    for (const auto& [key, item] : snapshot["frames"].items())
        if (truthy(item.value("reviewed", Json(false))) && (truthy(item.value("human_touched", Json(false))) || (decisions.contains(key) ? decisions[key].value("selected", true) : true)))
            result.emplace_back(key, item);
    return result;
}
}
