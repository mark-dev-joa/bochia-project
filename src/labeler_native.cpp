#include "labeler_native.hpp"
#include "labeler_assist.hpp"
#include "labeler_export.hpp"
#include "labeler_util.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace boccia::labeler {
namespace fs = std::filesystem;
namespace {
bool truthy(const Json& value) { return value.is_boolean() ? value.get<bool>() : !value.is_null() && !(value.is_number() && value.get<double>() == 0) && !(value.is_object() && value.empty()) && !(value.is_array() && value.empty()); }
bool humanWork(const Json& frame) { return truthy(frame.value("human_touched", Json(false))) || truthy(frame.value("reviewed", Json(false))); }
std::string recordingOf(const std::string& key) { return key.substr(0, key.find('/')); }
std::string formatLocal(int64_t seconds, const char* format) {
    const time_t stamp = time_t(seconds); tm local{}; localtime_s(&local, &stamp);
    char text[64]{}; strftime(text, sizeof text, format, &local); return text;
}
}
Json sceneIndex(const Catalog& catalog) {
    Json scenes = Json::array();
    int recordingNumber = 0;
    for (const auto& recording : catalog.recordings) {
        ++recordingNumber;
        const auto& cameras = recording.cameras;
        const auto& anchor = cameras.front();
        struct Row { int64_t time; std::map<std::string, int> members; };
        std::vector<Row> rows;
        for (size_t index = 0; index < anchor.times.size(); ++index) rows.push_back({anchor.times[index], {{anchor.id, int(index)}}});
        const auto& times = anchor.times;
        for (size_t c = 1; c < cameras.size(); ++c) {
            const auto& camera = cameras[c];
            for (size_t index = 0; index < camera.times.size(); ++index) {
                const int64_t time = camera.times[index];
                const auto position = int(std::lower_bound(times.begin(), times.end(), time) - times.begin());
                std::vector<int> candidates;
                for (int offset : {-1, 0, 1}) { const int number = std::max(0, std::min(int(times.size()) - 1, position + offset)); if (std::find(candidates.begin(), candidates.end(), number) == candidates.end()) candidates.push_back(number); }
                std::stable_sort(candidates.begin(), candidates.end(), [&](int a, int b) { return std::abs(times[size_t(a)] - time) < std::abs(times[size_t(b)] - time); });
                int match = -1;
                for (int number : candidates) if (std::abs(times[size_t(number)] - time) <= 20'000'000 && !rows[size_t(number)].members.count(camera.id)) { match = number; break; }
                if (match < 0) rows.push_back({time, {{camera.id, int(index)}}});
                else rows[size_t(match)].members[camera.id] = int(index);
            }
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.time < b.time; });
        for (const auto& row : rows) {
            Json members = Json::array();
            for (const auto& camera : cameras) {
                const auto found = row.members.find(camera.id);
                if (found == row.members.end()) continue;
                const int index = found->second;
                members.push_back({{"key", recording.id + "/" + camera.id + "/" + std::to_string(index)}, {"camera", camera.id}, {"name", camera.name}, {"kind", camera.kind},
                                   {"index", index}, {"host_ns", std::to_string(camera.times[size_t(index)])}, {"delta_ms", double(camera.times[size_t(index)] - row.time) / 1e6}});
            }
            const auto anchored = row.members.find(anchor.id);
            scenes.push_back({{"id", recording.id + "/" + std::to_string(scenes.size())}, {"recording", recording.id}, {"recording_number", recordingNumber},
                              {"index", anchored != row.members.end() ? anchored->second : members[0]["index"].get<int>()}, {"host_ns", std::to_string(row.time)}, {"members", members}});
        }
    }
    return scenes;
}
Json targets(const Project& project) {
    const Json decisions = project.selection.view(project.state["frames"])["frames"];
    Json result = Json::array();
    for (const auto& scene : sceneIndex(project.catalog)) for (const auto& member : scene["members"]) {
        const std::string key = member["key"];
        if (!decisions.contains(key) || decisions[key].value("selected", true)) result.push_back(member);
    }
    return result;
}
std::string fingerprint(const Project& project, const Json* stateOverride, const Json* groupsOverride) {
    const Json& state = stateOverride ? *stateOverride : project.state;
    const Json groups = groupsOverride ? *groupsOverride : (state.contains("native_split") && state["native_split"].is_object() ? state["native_split"].value("groups", Json::object()) : Json::object());
    const Json selection = project.selection.view(state["frames"])["frames"];
    std::map<std::string, std::vector<std::string>> batches;
    for (const auto& [recording, group] : groups.items()) batches[group.get<std::string>()].push_back(recording);
    std::vector<std::string> selected;
    for (const auto& [key, value] : selection.items()) if (value.value("selected", false)) selected.push_back(key);
    std::sort(selected.begin(), selected.end());
    // The same labels with another test set are another dataset. Group names do not matter.
    std::vector<std::vector<std::string>> grouped;
    for (auto& [group, members] : batches) { std::sort(members.begin(), members.end()); grouped.push_back(members); }
    std::sort(grouped.begin(), grouped.end());
    const Json snapshot = {{"source", project.catalog.signature}, {"frames", state["frames"]}, {"selected", selected}, {"splits", state["splits"]}, {"groups", grouped}};
    return sha256Hex(snapshot.dump(-1, ' ', true));
}
bool splitValid(const Project& project) {
    const auto confirmation = project.state.find("native_split");
    return confirmation != project.state.end() && confirmation->is_object() && truthy(*confirmation)
        && confirmation->value("fingerprint", "") == fingerprint(project) && confirmation->value("splits", Json()) == project.state["splits"];
}
Json recordingStats(const Project& project) {
    Json stats = Json::object();
    int number = 0;
    for (const auto& recording : project.catalog.recordings) {
        ++number;
        const std::string stamp = recording.name.substr(recording.name.rfind('-') + 1);
        const bool dated = stamp.size() == 13 && std::all_of(stamp.begin(), stamp.end(), [](char c) { return c >= '0' && c <= '9'; });
        const int64_t started = dated ? std::stoll(stamp) / 1000 : 0;
        const auto& times = recording.cameras.front().times;
        Json frames = Json::array();
        for (const auto& stream : recording.cameras) frames.push_back(stream.frames.size());
        stats[recording.id] = {{"number", number}, {"name", recording.name}, {"day", dated ? Json(formatLocal(started, "%Y-%m-%d")) : Json()},
                               {"started", dated ? Json(formatLocal(started, "%m-%d %H:%M:%S")) : Json()},
                               {"seconds", dated ? Json(std::round(double(times.back() - times.front()) / 1e9 * 10) / 10) : Json()},
                               {"frames", frames}, {"images", 0}, {"balls", 0}, {"empty", 0}, {"small", 0}, {"reviewed", 0}};
    }
    for (const auto& member : targets(project)) {
        const std::string key = member["key"];
        Json& item = stats[recordingOf(key)];
        const Json frame = project.state["frames"].contains(key) ? project.state["frames"][key] : Json::object();
        const Json boxes = frame.value("boxes", Json::array());
        item["images"] = item["images"].get<int>() + 1;
        item["balls"] = item["balls"].get<int>() + int(boxes.size());
        item["empty"] = item["empty"].get<int>() + (boxes.empty() ? 1 : 0);
        int small = 0;
        for (const auto& box : boxes) { const Json& bounds = box.contains("detection_bbox") ? box["detection_bbox"] : box["bbox"]; if (std::max(bounds[2].get<double>(), bounds[3].get<double>()) < 32) ++small; }
        item["small"] = item["small"].get<int>() + small;
        item["reviewed"] = item["reviewed"].get<int>() + (truthy(frame.value("reviewed", Json(false))) ? 1 : 0);
    }
    return stats;
}
cv::Mat thumbnail(Project& project, const std::string& recording, const std::string& camera, int index, int width) {
    if (width < 16 || width > 512) throw std::runtime_error("썸네일 폭이 올바르지 않습니다.");
    const std::string key = recording + "/" + camera + "/" + std::to_string(index) + "@" + std::to_string(width);
    {
        std::lock_guard guard(project.lock);
        const auto found = project.thumbnails.find(key);
        if (found != project.thumbnails.end()) return found->second;
    }
    const cv::Mat image = project.catalog.read(recording, camera, index);
    const int height = std::max(1, int(std::nearbyint(double(image.rows) * width / image.cols)));
    cv::Mat small; cv::resize(image, small, {width, height}, 0, 0, cv::INTER_AREA);
    std::lock_guard guard(project.lock);
    return project.thumbnails.emplace(key, small).first->second;
}
Json exportHistory(const Project& project) {
    Json entries = Json::array();
    if (!fs::is_directory(project.exportRoot)) return entries;
    std::vector<fs::path> folders;
    for (const auto& entry : fs::directory_iterator(project.exportRoot)) folders.push_back(entry.path());
    std::sort(folders.begin(), folders.end(), [](const fs::path& a, const fs::path& b) { return pathLess(b, a); });
    for (const auto& folder : folders) {
        const fs::path manifest = folder / "manifest.json";
        if (!fs::is_regular_file(manifest) || fs::exists(folder / "INCOMPLETE.txt")) continue;
        Json value;
        try { std::ifstream in(manifest, std::ios::binary); value = Json::parse(in); } catch (...) { continue; }
        if (!value.is_object()) continue;
        const Json split = value.contains("native_split") && value["native_split"].is_object() ? value["native_split"] : Json::object();
        entries.push_back({{"name", utf8(folder.filename())}, {"path", utf8(folder)}, {"created_at", value.value("created_at", Json())},
                           {"counts", value.value("counts", Json::object())}, {"fingerprint", split.value("fingerprint", Json())}});
    }
    return entries;
}
int colorable(const Project& project) {
    int count = 0;
    for (const auto& [key, frame] : project.state["frames"].items()) {
        if (project.catalog.camera(frame["recording"], frame["camera"]).kind != "컬러") continue;
        for (const auto& box : frame["boxes"]) if (box.contains("segmentation") && box.value("color", "unknown") == "unknown") ++count;
    }
    return count;
}
Json growable(const Project& project) {
    int frames = 0, balls = 0;
    for (const auto& [key, frame] : project.state["frames"].items()) {
        if (humanWork(frame)) continue;
        int masks = 0;
        for (const auto& box : frame["boxes"]) if (box.contains("segmentation")) ++masks;
        if (masks) { ++frames; balls += masks; }
    }
    return {{"frames", frames}, {"balls", balls}};
}
Json colorFillStatus(const Project& project) { std::lock_guard guard(project.lock); return project.colorJob; }
Json startColorFill(Project& project) {
    std::lock_guard guard(project.lock);
    if (project.colorJob.value("status", "") == "running") throw std::runtime_error("이미 색상을 채우는 중입니다.");
    std::vector<std::pair<std::string, Json>> targets;
    for (const auto& [key, frame] : project.state["frames"].items()) {
        if (project.catalog.camera(frame["recording"], frame["camera"]).kind != "컬러") continue;
        bool wanted = false;
        for (const auto& box : frame["boxes"]) if (box.contains("segmentation") && box.value("color", "unknown") == "unknown") wanted = true;
        if (wanted) targets.emplace_back(key, frame);
    }
    project.colorJob = {{"status", "running"}, {"done", 0}, {"total", targets.size()}, {"filled", 0}};
    if (project.colorWorker.joinable()) project.colorWorker.join();   // the previous run has finished; its thread is still a member
    project.colorWorker = std::jthread([&project, targets = std::move(targets)](std::stop_token stop) {
        try {
            // The colours are decided outside the lock and written in one revision at the end; a ball that got a colour
            // meanwhile, or an image whose balls changed, is left alone.
            std::map<std::string, Json> results;
            int number = 0;
            for (const auto& [key, frame] : targets) {
                if (stop.stop_requested()) { std::lock_guard guard(project.lock); project.colorJob["status"] = "stopped"; return; }
                const cv::Mat image = project.catalog.read(frame["recording"], frame["camera"], frame["index"]);
                Json colors = Json::array();
                for (const auto& box : frame["boxes"])
                    colors.push_back(box.contains("segmentation") && box.value("color", "unknown") == "unknown" ? Json(ballColor(image, decodeMaskChecked(box["segmentation"], frame["width"], frame["height"]))) : Json());
                results[key] = std::move(colors);
                std::lock_guard guard(project.lock); project.colorJob["done"] = ++number;
            }
            std::lock_guard guard(project.lock);
            project.mutate(project.state["revision"].get<int>(), [&](Json& state) {
                for (const auto& [key, colors] : results) {
                    if (!state["frames"].contains(key) || state["frames"][key]["boxes"].size() != colors.size()) continue;
                    Json& boxes = state["frames"][key]["boxes"];
                    for (size_t i = 0; i < colors.size(); ++i)
                        if (colors[i].is_string() && colors[i] != "unknown" && boxes[i].value("color", "unknown") == "unknown") { boxes[i]["color"] = colors[i]; project.colorJob["filled"] = project.colorJob["filled"].get<int>() + 1; }
                }
                state["color_fill"] = {{"balls", project.colorJob["filled"]}, {"at", utcIso()}};
                state.erase("native_split");
            });
            project.colorJob["status"] = "complete";
        } catch (const std::exception& error) {
            std::lock_guard guard(project.lock); project.colorJob["status"] = "error"; project.colorJob["error"] = error.what();
        }
    });
    return project.colorJob;
}
Json nativeCatalog(const Project& project) {
    std::lock_guard guard(project.lock);
    const Json summary = project.summary();
    Json scenes = sceneIndex(project.catalog);
    int count = 0, reviewed = 0;
    for (auto& scene : scenes) for (auto& member : scene["members"]) {
        const std::string key = member["key"];
        const Json entry = summary["frames"].contains(key) ? summary["frames"][key] : Json::object();
        member["selected"] = entry.value("selected", true); member["reviewed"] = entry.value("reviewed", false); member["status"] = entry.value("review_status", "draft");
        member["count"] = entry.value("count", 0); member["attention"] = entry.value("needs_attention", false);
        if (member["selected"].get<bool>()) { ++count; reviewed += member["reviewed"].get<bool>(); }
    }
    const bool valid = splitValid(project);
    // The batches outlive a confirmation: a label fix must not turn them back into single recordings.
    const Json groups = project.state.contains("native_groups") && truthy(project.state["native_groups"]) ? project.state["native_groups"]
                      : project.state.contains("native_split") && project.state["native_split"].is_object() ? project.state["native_split"].value("groups", Json::object()) : Json::object();
    return {{"api_version", nativeApiVersion}, {"source", utf8(project.catalog.source)}, {"project_path", utf8(project.path)},
            {"recordings", project.catalog.metadata()}, {"summary", summary}, {"scenes", scenes},
            {"target_images", count}, {"reviewed_target_images", reviewed}, {"split_confirmed", valid}, {"groups", groups},
            {"stats", recordingStats(project)}, {"growable", growable(project)}, {"mask_growth", project.state.value("mask_growth", Json())},
            {"colorable", colorable(project)}, {"color_fill", project.state.value("color_fill", Json())},
            // The data version: set only while the confirmed split still matches the labels.
            {"fingerprint", valid ? project.state["native_split"]["fingerprint"] : Json()}};
}
std::pair<Json, Json> groupPlan(const Project& project, const Json& groups) {
    const auto& recordings = project.catalog.recordings;
    bool valid = groups.is_object() && groups.size() == recordings.size();
    if (valid) for (const auto& recording : recordings) {
        const auto found = groups.find(recording.id);
        if (found == groups.end() || !found->is_string() || found->get<std::string>().empty() || found->get<std::string>().size() > 128) { valid = false; break; }
    }
    if (!valid) throw std::runtime_error("촬영 그룹은 모든 촬영에 지정해야 합니다.");
    Json counts = Json::object();
    for (const auto& recording : recordings) counts[recording.id] = 0;
    for (const auto& member : targets(project)) { const auto id = recordingOf(member["key"]); counts[id] = counts[id].get<int>() + 1; }
    std::map<std::string, int> grouped;
    for (const auto& [recording, group] : groups.items()) grouped[group.get<std::string>()] += counts[recording].get<int>();
    std::vector<std::string> usable;
    int total = 0;
    for (const auto& [group, count] : grouped) { total += count; if (count) usable.push_back(group); }
    if (usable.size() < 3) throw std::runtime_error("학습·검증·최종 평가에는 독립적인 촬영 그룹이 최소 3개 필요합니다.");
    // Reserve the groups nearest the 10% target; keep every group intact.
    std::set<std::string> remaining(usable.begin(), usable.end());
    std::map<std::string, std::string> assignment;
    for (const auto& [group, count] : grouped) assignment[group] = "train";
    const double goal = total * .1;
    for (const char* split : {"test", "val"}) {
        const auto chosen = *std::min_element(remaining.begin(), remaining.end(), [&](const std::string& a, const std::string& b) {
            const double da = std::abs(grouped[a] - goal), db = std::abs(grouped[b] - goal); return da < db || (da == db && a < b); });
        assignment[chosen] = split; remaining.erase(chosen);
    }
    Json splits = Json::object();
    for (const auto& [recording, group] : groups.items()) splits[recording] = assignment[group.get<std::string>()];
    return {splits, counts};
}
Json nativeAction(Project& project, const std::string& action, const Json& payload) {
    if (action == "review") {
        const Json keys = payload.value("keys", Json());
        const Json status = payload.value("status", Json("reviewed"));
        std::set<std::string> unique;
        if (keys.is_array()) for (const auto& key : keys) if (key.is_string()) unique.insert(key.get<std::string>());
        if (!keys.is_array() || keys.empty() || keys.size() > 3 || unique.size() != keys.size() || !status.is_string() || (status != "reviewed" && status != "hold"))
            throw std::runtime_error("현재 장면의 검수 상태 요청이 올바르지 않습니다.");
        std::lock_guard guard(project.lock);
        std::vector<std::pair<std::string, Json>> entries;
        std::set<std::string> recordings;
        for (const auto& item : keys) {
            const std::string key = item;
            const auto first = key.find('/'), second = key.find('/', first + 1);
            if (first == std::string::npos || second == std::string::npos) throw std::runtime_error("촬영 또는 카메라를 찾지 못했습니다.");
            entries.emplace_back(key, project.frame(key.substr(0, first), key.substr(first + 1, second - first - 1), std::stoi(key.substr(second + 1))));
            recordings.insert(key.substr(0, first));
        }
        if (recordings.size() != 1) throw std::runtime_error("한 장면의 촬영만 함께 검수할 수 있습니다.");
        bool together = false;
        for (const auto& scene : sceneIndex(project.catalog)) {
            std::set<std::string> members;
            for (const auto& member : scene["members"]) members.insert(member["key"].get<std::string>());
            if (std::includes(members.begin(), members.end(), unique.begin(), unique.end())) { together = true; break; }
        }
        if (!together) throw std::runtime_error("서로 다른 시점의 프레임을 한 장면으로 검수할 수 없습니다.");
        // The automatic draft can add balls after a screen loaded the scene. They must be seen before they are confirmed.
        if (payload.contains("seen") && !payload["seen"].is_null()) {
            const Json& seen = payload["seen"];
            bool differs = !seen.is_object();
            if (!differs) for (const auto& [key, frame] : entries) if (!seen.contains(key) || seen[key] != Json(frame["boxes"].size())) differs = true;
            if (differs) throw Conflict("저장된 라벨이 화면에 표시된 것과 다릅니다. 장면을 다시 불러온 뒤 확인하세요.");
        }
        const bool reviewed = status == "reviewed";
        const Json summary = project.mutate(payload.at("expected_revision").get<int>(), [&](Json& state) {
            for (const auto& [key, frame] : entries) {
                Json annotation = Json::object();
                for (const char* name : {"recording", "camera", "index", "width", "height", "boxes", "automatic"}) if (frame.contains(name)) annotation[name] = frame[name];
                for (auto& box : annotation["boxes"]) box.erase("contours");   // the frame response includes transient contours; never store them
                annotation["reviewed"] = reviewed; annotation["review_status"] = status; annotation["human_touched"] = true;
                state["frames"][key] = std::move(annotation);
            }
            state.erase("native_split");
        });
        return {{"summary", summary}};
    }
    if (action == "split-preview") {
        std::lock_guard guard(project.lock);
        Json groups = payload.value("groups", Json());
        if (groups.is_null()) { groups = Json::object(); for (const auto& recording : project.catalog.recordings) groups[recording.id] = recording.id; }
        auto [splits, counts] = groupPlan(project, groups);
        return {{"groups", groups}, {"splits", splits}, {"counts", counts}};
    }
    if (action == "split-confirm") {
        std::lock_guard guard(project.lock);
        int unresolved = 0;
        for (const auto& member : targets(project)) { const std::string key = member["key"]; if (!project.state["frames"].contains(key) || !truthy(project.state["frames"][key].value("reviewed", Json(false)))) ++unresolved; }
        if (unresolved) throw std::runtime_error("검수 대상 " + std::to_string(unresolved) + "장이 미완료입니다. 전체 검수를 먼저 완료하세요.");
        const Json groups = payload.at("groups");
        groupPlan(project, groups);
        const Json splits = payload.at("splits");
        bool valid = splits.is_object() && splits.size() == project.catalog.recordings.size();
        if (valid) for (const auto& recording : project.catalog.recordings) {
            const auto found = splits.find(recording.id);
            if (found == splits.end() || !found->is_string() || std::find(std::begin(splitNames), std::end(splitNames), found->get<std::string>()) == std::end(splitNames)) { valid = false; break; }
        }
        if (!valid) throw std::runtime_error("모든 촬영을 학습·검증·최종 평가로 지정하세요.");
        std::map<std::string, std::set<std::string>> perGroup;
        for (const auto& [recording, group] : groups.items()) perGroup[group.get<std::string>()].insert(splits[recording].get<std::string>());
        for (const auto& [group, used] : perGroup) if (used.size() != 1) throw std::runtime_error("동일 촬영 그룹을 서로 다른 분할로 나눌 수 없습니다.");
        std::set<std::string> covered;
        for (const auto& member : targets(project)) covered.insert(splits[recordingOf(member["key"])].get<std::string>());
        if (covered != std::set<std::string>{"train", "val", "test"}) throw std::runtime_error("학습·검증·최종 평가에 검수 이미지가 각각 있어야 합니다.");
        const Json summary = project.mutate(payload.at("expected_revision").get<int>(), [&](Json& state) {
            state["splits"] = splits; state["native_groups"] = groups;
            state["native_split"] = {{"groups", groups}, {"splits", splits}, {"fingerprint", fingerprint(project, &state, &groups)}};
        });
        return {{"summary", summary}, {"split_confirmed", true}, {"fingerprint", project.state["native_split"]["fingerprint"]}};
    }
    if (action == "fill-colors") return startColorFill(project);
    if (action == "grow-masks") {
        // The automatic draft stops short of the ball edge by about a pixel (measured 2026-10-08), the same way on
        // every ball; one pass moves every draft mask outward. Images a person has saved or confirmed are left alone.
        const Json pixelsValue = payload.value("pixels", Json(1));
        if (!pixelsValue.is_number_integer() || pixelsValue.get<int>() < 1 || pixelsValue.get<int>() > 3) throw std::runtime_error("넓힐 픽셀 수는 1에서 3 사이여야 합니다.");
        const int pixels = pixelsValue.get<int>();
        const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, {2 * pixels + 1, 2 * pixels + 1});
        int frames = 0, balls = 0;
        std::lock_guard guard(project.lock);
        const Json summary = project.mutate(payload.at("expected_revision").get<int>(), [&](Json& state) {
            for (auto& [key, frame] : state["frames"].items()) {
                if (humanWork(frame)) continue;
                bool touched = false;
                for (auto& box : frame["boxes"]) {
                    if (!box.contains("segmentation")) continue;
                    cv::Mat grown; cv::dilate(decodeMaskChecked(box["segmentation"], frame["width"], frame["height"]), grown, kernel);
                    box["segmentation"] = encodeMask(grown);
                    const auto bounds = cv::boundingRect(grown);
                    box["bbox"] = {bounds.x, bounds.y, bounds.width, bounds.height};
                    if (box.contains("area")) box["area"] = cv::countNonZero(grown);
                    touched = true; ++balls;
                }
                frames += touched;
            }
            const int previous = state.contains("mask_growth") && state["mask_growth"].is_object() ? state["mask_growth"].value("pixels", 0) : 0;
            state["mask_growth"] = {{"pixels", pixels + previous}, {"frames", frames}, {"balls", balls}, {"at", utcIso()}};
            state.erase("native_split");
        });
        return {{"summary", summary}, {"frames", frames}, {"balls", balls}, {"mask_growth", project.state["mask_growth"]}, {"growable", growable(project)}};
    }
    if (action == "export") {
        std::lock_guard guard(project.lock);
        if (!splitValid(project)) throw std::runtime_error("전체 검수 후 데이터 분할을 확정하세요. 라벨 수정 후에는 다시 확정해야 합니다.");
        return startExport(project, payload.value("crops", true));
    }
    throw std::runtime_error("지원하지 않는 네이티브 작업입니다.");
}
Json assistance(const Project& project, BallAssist& assist, const std::string& method, const Json& payload) {
    const std::string recording = payload.at("recording"), camera = payload.at("camera");
    const int index = payload.at("index").get<int>();
    const Json frame = project.frame(recording, camera, index);
    const int width = frame["width"], height = frame["height"];
    if (method == "polygon") return {{"key", frame["key"]}, {"proposals", Json::array({polygonProposal(payload.at("points"), width, height)})}};
    const cv::Mat image = project.catalog.read(recording, camera, index);
    if (method == "segment") {
        Json box;
        try { box = assist.segment(image, project.catalog.signature + "/" + frame["key"].get<std::string>(), payload.at("points"), payload.value("radius", Json(14)), payload.value("box", Json())); }
        catch (const std::runtime_error&) { throw; }
        catch (const std::exception& error) { throw std::runtime_error(std::string("분할 실행에 실패했습니다: ") + std::string(error.what()).substr(0, 240)); }
        return {{"key", frame["key"]}, {"proposals", Json::array({box})}};
    }
    if (method == "circles") {
        const Json region = payload.value("region", Json({0, 0, width, height}));
        const Json roi = validateBoxes(Json::array({{{"bbox", region}}}), width, height)[0]["bbox"];
        const double x = roi[0], y = roi[1], w = roi[2], h = roi[3];
        const int left = int(std::floor(x)), top = int(std::floor(y)), right = int(std::ceil(x + w)), bottom = int(std::ceil(y + h));
        Json proposals = BallAssist::circles(image(cv::Rect(left, top, right - left, bottom - top)), payload.value("minimum", Json(6)), payload.value("maximum", Json(45)));
        Json kept = Json::array();
        for (auto& box : proposals) {
            box["bbox"][0] = box["bbox"][0].get<int>() + left; box["bbox"][1] = box["bbox"][1].get<int>() + top;
            bool taken = false;
            for (const auto& existing : frame["boxes"]) if (overlap(box["bbox"], existing["bbox"]) > .4) taken = true;
            if (!taken) kept.push_back(box);
        }
        return {{"key", frame["key"]}, {"proposals", kept}};
    }
    if (method == "propagate") {
        Json prior;
        {
            std::lock_guard guard(project.lock);
            for (const auto& [key, item] : project.state["frames"].items())
                if (item["recording"] == recording && item["camera"] == camera && item["index"].get<int>() < index && !item["boxes"].empty() && (prior.is_null() || item["index"].get<int>() > prior["index"].get<int>())) prior = item;
            if (prior.is_null()) throw std::runtime_error("이 카메라의 이전 프레임에 저장한 공이 없습니다. 먼저 한 프레임을 라벨링하세요.");
        }
        const cv::Mat old = project.catalog.read(recording, camera, prior["index"].get<int>());
        return {{"key", frame["key"]}, {"previous_index", prior["index"]}, {"proposals", BallAssist::propagate(old, image, prior["boxes"], frame["boxes"])}};
    }
    throw std::runtime_error("지원하지 않는 보조 도구입니다.");
}
}
