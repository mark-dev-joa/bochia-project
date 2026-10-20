#include "native_labeler_model.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

namespace boccia::labeler {
Json hubProgress(const Json& catalog, const std::string& worker, std::time_t now) {
    if (!catalog.is_object()) return Json();
    Json progress{{"target", catalog.value("target_images", 0)}, {"reviewed", catalog.value("reviewed_target_images", 0)}};
    // Per recording, in scene order: the review targets (selected members) and the confirmed ones among them.
    Json recordings = Json::array();
    std::map<int, size_t> where;
    static const Json none = Json::object();
    const Json& scenes = catalog.contains("scenes") && catalog["scenes"].is_array() ? catalog["scenes"] : none;
    for (const auto& scene : scenes) {
        const int number = scene.value("recording_number", 0);
        auto found = where.find(number);
        if (found == where.end()) {
            char name[48];
            std::snprintf(name, sizeof name, "%s %02d", scene.value("source_type", "") == "images" ? "이미지 자료" : "촬영", number);
            found = where.emplace(number, recordings.size()).first;
            recordings.push_back({{"name", name}, {"reviewed", 0}, {"target", 0}});
        }
        auto& entry = recordings[found->second];
        if (!scene.contains("members") || !scene["members"].is_array()) continue;
        for (const auto& member : scene["members"]) {
            if (!member.value("selected", true)) continue;
            entry["target"] = entry["target"].get<int>() + 1;
            if (member.value("reviewed", false)) entry["reviewed"] = entry["reviewed"].get<int>() + 1;
        }
    }
    progress["recordings"] = std::move(recordings);
    // The 7 local dates ending today, at noon so a daylight-saving change cannot skip one.
    std::tm today{};
    localtime_s(&today, &now);
    std::vector<std::string> full, shown;
    for (int back = 6; back >= 0; --back) {
        std::tm day = today;
        day.tm_mday -= back; day.tm_hour = 12; day.tm_min = 0; day.tm_sec = 0; day.tm_isdst = -1;
        std::mktime(&day);
        char date[16], label[8];
        std::strftime(date, sizeof date, "%Y-%m-%d", &day);
        std::strftime(label, sizeof label, "%m-%d", &day);
        full.emplace_back(date); shown.emplace_back(label);
    }
    std::vector<int> counts(full.size(), 0);
    int hold = 0, propagated = 0, last10 = 0;
    const Json& summary = catalog.contains("summary") && catalog["summary"].is_object() ? catalog["summary"] : none;
    const Json& frames = summary.contains("frames") && summary["frames"].is_object() ? summary["frames"] : none;
    for (const auto& [key, frame] : frames.items()) {
        const std::string status = frame.value("review_status", "");
        if (status == "hold") ++hold;
        if (frame.value("propagated", false)) ++propagated;
        if (worker.empty() || status != "reviewed" || frame.value("reviewed_by", "") != worker) continue;
        const std::string at = frame.value("reviewed_at", "");
        if (at.size() < 19) continue;
        const auto day = std::find(full.begin(), full.end(), at.substr(0, 10));
        if (day == full.end()) continue;
        ++counts[size_t(day - full.begin())];
        if (day + 1 != full.end()) continue;   // the last ten minutes are today's
        std::tm parsed{};
        std::istringstream(at) >> std::get_time(&parsed, "%Y-%m-%d %H:%M:%S");
        parsed.tm_isdst = -1;
        const std::time_t when = std::mktime(&parsed);
        if (when != -1 && when <= now && now - when <= 600) ++last10;
    }
    Json days = Json::array();
    for (size_t i = 0; i < full.size(); ++i) days.push_back({{"date", shown[i]}, {"count", counts[i]}});
    progress["days"] = std::move(days);
    progress["today"] = counts.back();
    progress["last10"] = last10;
    progress["hold"] = hold;
    progress["propagated"] = propagated;
    return progress;
}
std::string jobEndProblem(const std::string& job, const Json& status) {
    const std::string state = status.is_object() ? status.value("status", "") : std::string();
    if (state == "complete") return {};
    if (state == "stopped") return job + " 중단";
    std::string reason;
    if (status.is_object() && status.contains("error")) reason = status["error"].is_string() ? status["error"].get<std::string>() : status["error"].dump();
    return "오류 · " + job + (reason.empty() ? std::string() : " · " + reason);
}
Json hubStatusJson(const std::string& page, const Json& scene, const Json& job, bool applyPrevious, bool gpu, const std::string& error, const Json& progress) {
    std::string text = page == "data" ? "자료" : page == "export" ? "내보내기" : "검수";
    if (scene.is_object()) {
        char frame[16];
        std::snprintf(frame, sizeof frame, "%04d", scene.value("index", 0));
        text += " · " + scene.value("recording", "") + (scene.value("image", false) ? " · 이미지 " : " · 프레임 ") + frame;
    }
    if (job.is_object() && !job.value("text", "").empty()) text += " · " + job.value("text", "");
    if (!error.empty()) text += " · 오류";
    Json status{{"page", page}, {"scene", scene.is_object() ? scene : Json()}, {"job", job.is_object() ? job : Json()},
                {"apply_previous", applyPrevious}, {"device", gpu ? "GPU" : "CPU"}, {"error", error}, {"text", text}};
    if (progress.is_object()) status["progress"] = progress;
    return status;
}
}
#include <cmath>
#include <stdexcept>

namespace boccia::labeler {
cv::Mat decodeMask(const Json& rle, int width, int height) {
    if (!rle.is_object() || rle.at("size") != Json::array({height, width})) throw std::runtime_error("마스크 크기가 다릅니다.");
    cv::Mat mask(height, width, CV_8UC1, cv::Scalar(0));
    size_t offset = 0, total = size_t(width) * height;
    bool filled = false;
    for (const auto& run : rle.at("counts")) {
        if (!run.is_number_integer() || run.get<long long>() < 0) throw std::runtime_error("마스크 길이가 올바르지 않습니다.");
        const auto count = run.get<size_t>();
        if (count > total - offset) throw std::runtime_error("마스크가 원본 범위를 벗어났습니다.");
        if (filled) for (size_t i = offset; i < offset + count; ++i) mask.at<uchar>(int(i % height), int(i / height)) = 1;
        offset += count; filled = !filled;
    }
    if (offset != total) throw std::runtime_error("마스크 길이가 원본과 다릅니다.");
    return mask;
}
Json encodeMask(const cv::Mat& mask) {
    if (mask.empty() || mask.type() != CV_8UC1) throw std::runtime_error("마스크가 없습니다.");
    Json counts = Json::array(); size_t run = 0; bool previous = false;
    for (int x = 0; x < mask.cols; ++x) for (int y = 0; y < mask.rows; ++y) {
        const bool filled = mask.at<uchar>(y, x) != 0;
        if (filled != previous) { counts.push_back(run); run = 0; previous = filled; }
        ++run;
    }
    counts.push_back(run);
    return {{"size", {mask.rows, mask.cols}}, {"counts", counts}};
}
Json maskAnnotation(const cv::Mat& mask, const Json& previous, const std::string& source) {
    if (cv::countNonZero(mask) == 0) throw std::runtime_error("마스크가 비었습니다. 취소하거나 공 영역을 다시 칠하세요.");
    Json box = previous;
    auto bounds = cv::boundingRect(mask);
    box["bbox"] = {bounds.x, bounds.y, bounds.width, bounds.height};
    box["segmentation"] = encodeMask(mask); box["area"] = cv::countNonZero(mask); box["source"] = source;
    box.erase("detection_bbox"); box.erase("contours"); box.erase("circle");   // a new outline is measured again
    box["occluded"] = box.value("occluded", false);
    return box;
}
cv::Rect2d detectionBox(const Json& box) {
    const auto& value = box.contains("detection_bbox") ? box.at("detection_bbox") : box.at("bbox");
    return {value.at(0).get<double>(), value.at(1).get<double>(), value.at(2).get<double>(), value.at(3).get<double>()};
}
bool nudgeEdge(Json& box, int edge, bool expand, int width, int height) {
    auto b = detectionBox(box); double left = b.x, top = b.y, right = b.x + b.width, bottom = b.y + b.height;
    const double delta = expand ? 1 : -1;
    switch (edge) { case 0: top -= delta; break; case 1: bottom += delta; break; case 2: left -= delta; break; case 3: right += delta; break; default: return false; }
    if (left < 0 || top < 0 || right > width || bottom > height || right - left < 1 || bottom - top < 1) return false;
    // Four decimals, as the store keeps them: a measured box is fractional, and a step out and back must land exactly.
    auto round4 = [](double value) { return std::nearbyint(value * 10000) / 10000; };
    box["detection_bbox"] = {round4(left), round4(top), round4(right - left), round4(bottom - top)}; return true;
}
// A partly hidden ball's box is its visible part, so a circle inscribed in it (from a box edit or the box tool) means
// nothing and goes; a circle measured on the image's own edge stays (2026-10-08 user request).
void setOccludedFlag(Json& box, bool value) {
    box["occluded"] = value;
    if (value && box.value("circle_source", "") == "box") { box.erase("circle"); box.erase("circle_source"); }
}
Json annotationPayload(const Json& frame, int revision) {
    return {{"recording", frame.at("recording")}, {"camera", frame.at("camera")}, {"index", frame.at("index")},
            {"boxes", frame.at("boxes")}, {"reviewed", frame.value("reviewed", false)},
            {"review_status", frame.value("review_status", frame.value("reviewed", false) ? "reviewed" : "editing")},
            {"expected_revision", revision}};
}
}
