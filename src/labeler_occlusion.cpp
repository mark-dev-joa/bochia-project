#include "labeler_occlusion.hpp"
#include "labeler_assist.hpp"
#include "labeler_util.hpp"
#include "native_labeler_model.hpp"
#include <opencv2/imgproc.hpp>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

namespace boccia::labeler {
namespace {
bool truthy(const Json& value) { return value.is_boolean() ? value.get<bool>() : !value.is_null() && !(value.is_number() && value.get<double>() == 0); }
bool humanWork(const Json& frame) { return truthy(frame.value("human_touched", Json(false))) || truthy(frame.value("reviewed", Json(false))); }
const Json& shownBox(const Json& box) { return box.contains("detection_bbox") ? box["detection_bbox"] : box["bbox"]; }
struct Disc { double x, y, r; };
Disc discOf(const Json& box) {
    if (box.contains("circle")) return {box["circle"][0].get<double>(), box["circle"][1].get<double>(), box["circle"][2].get<double>()};
    const Json& b = shownBox(box);
    return {b[0].get<double>() + b[2].get<double>() / 2, b[1].get<double>() + b[3].get<double>() / 2, std::max(b[2].get<double>(), b[3].get<double>()) / 2};
}
struct Verdict { bool hidden = false; std::string reason; double coverage = 0, circularity = 0, aspect = 0; bool neighbour = false, edge = false; };
// One ball's evidence. The rim share comes from the Canny circle fit on the ball's mask; a missing arc that faces another
// ball within touching distance is the neighbour hiding it on the floor, which the full-ball rule leaves unflagged.
Verdict judge(const Json& box, const Json& boxes, size_t self, const cv::Mat& image, const OcclusionOptions& o) {
    Verdict v;
    const Json& b = shownBox(box);
    const double w = b[2].get<double>(), h = b[3].get<double>();
    v.aspect = w > 0 && h > 0 ? std::min(w, h) / std::max(w, h) : 1;
    v.edge = b[0].get<double>() <= 1 || b[1].get<double>() <= 1 || b[0].get<double>() + w >= image.cols - 1 || b[1].get<double>() + h >= image.rows - 1;
    const Disc own = discOf(box);
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (i == self) continue;
        const Disc other = discOf(boxes[i]);
        if (std::hypot(own.x - other.x, own.y - other.y) < own.r + other.r + 4) v.neighbour = true;
    }
    if (box.contains("segmentation")) {
        const cv::Mat mask = decodeMaskChecked(box["segmentation"], image.cols, image.rows);
        const cv::Rect bounds = cv::boundingRect(mask);
        const double r = std::max(bounds.width, bounds.height) / 2.0;
        v.circularity = r > 0 ? cv::countNonZero(mask) / (3.141592653589793 * r * r) : 1;
        const CircleFit fit = refineCircle(image, mask);
        v.coverage = fit.points >= 12 ? fit.coverage : 0;
    } else { v.circularity = 1; v.coverage = 1; }
    char why[96];
    if (v.edge) { v.hidden = true; v.reason = "화면 가장자리에 걸침"; }
    else if (v.aspect < o.aspect) { v.hidden = true; std::snprintf(why, sizeof why, "박스 비율 %.2f", v.aspect); v.reason = why; }
    else if (v.circularity < o.circularity) { v.hidden = true; std::snprintf(why, sizeof why, "마스크 원형도 %.2f", v.circularity); v.reason = why; }
    else if (v.coverage < o.coverage && !v.neighbour) { v.hidden = true; std::snprintf(why, sizeof why, "테두리 지지 %.0f%%", v.coverage * 100); v.reason = why; }
    else if (v.coverage < o.coverage && v.neighbour) { std::snprintf(why, sizeof why, "테두리 지지 %.0f%%, 옆 공에 가림", v.coverage * 100); v.reason = why; }
    return v;
}
struct Target { std::string key; Json frame; bool reviewed; };
}

OcclusionOptions occlusionOptions(const Json& payload) {
    OcclusionOptions o;
    o.coverage = payload.value("coverage", .72); o.circularity = payload.value("circularity", .8); o.aspect = payload.value("aspect", .85);
    for (double value : {o.coverage, o.circularity, o.aspect}) if (!std::isfinite(value) || value < .3 || value > 1) throw std::runtime_error("판정 기준은 0.3에서 1 사이여야 합니다.");
    o.includeReviewed = payload.value("include_reviewed", false);
    o.dryRun = payload.value("dry_run", false); o.countOnly = payload.value("count_only", false);
    o.limitFrames = std::clamp(payload.value("limit_frames", 0), 0, 100000); o.sampleLimit = std::clamp(payload.value("sample_limit", 8), 0, 64);
    return o;
}
Json occlusionStatus(const Project& project) { std::lock_guard guard(project.lock); return project.occlusionJob; }
Json stopOcclusionMark(Project& project) { std::lock_guard guard(project.lock); project.occlusionWorker.request_stop(); return project.occlusionJob; }

Json startOcclusionMark(Project& project, const OcclusionOptions& o) {
    std::lock_guard guard(project.lock);
    if (project.occlusionJob.value("status", "") == "running") throw std::runtime_error("이미 가림을 판정하는 중입니다.");
    const Json options = {{"coverage", o.coverage}, {"circularity", o.circularity}, {"aspect", o.aspect}, {"include_reviewed", o.includeReviewed}, {"limit_frames", o.limitFrames}};
    // Every frame with a ball not yet flagged; a person's images only on request, and there the flag only ever goes on.
    std::vector<Target> targets; int balls = 0;
    for (const auto& [key, frame] : project.state["frames"].items()) {
        const bool reviewed = humanWork(frame);
        if (reviewed && !o.includeReviewed) continue;
        int unflagged = 0;
        for (const auto& box : frame["boxes"]) if (!box.value("occluded", false)) ++unflagged;
        if (!unflagged) continue;
        balls += unflagged; targets.push_back({key, frame, reviewed});
    }
    const int framesTotal = int(targets.size());
    if (o.limitFrames > 0 && int(targets.size()) > o.limitFrames) {
        std::vector<Target> chosen; const double step = double(targets.size()) / o.limitFrames;
        for (int i = 0; i < o.limitFrames; ++i) chosen.push_back(std::move(targets[size_t(i * step)]));
        targets = std::move(chosen);
    }
    project.occlusionJob = {{"status", o.countOnly ? "complete" : "running"}, {"run_id", localStamp() + "-" + randomHex(4)}, {"options", options},
                            {"dry_run", o.dryRun || o.countOnly}, {"count_only", o.countOnly}, {"frames_total", framesTotal}, {"balls_total", balls},
                            {"total", targets.size()}, {"done", 0}, {"frames", 0}, {"balls", 0}, {"neighbours", 0}, {"samples", Json::array()}, {"started_at", utcIso()}};
    if (o.countOnly) return project.occlusionJob;
    if (project.occlusionWorker.joinable()) project.occlusionWorker.join();
    project.occlusionWorker = std::jthread([&project, targets = std::move(targets), o, options](std::stop_token stop) {
        try {
            std::map<std::string, Json> results; std::mutex resultsLock;
            std::atomic<size_t> next{0}; std::atomic<int> done{0}, neighbours{0};
            std::mutex failureLock; std::string failure;
            const int workers = std::max(1, std::min(4, int(std::thread::hardware_concurrency()) / 2));
            std::vector<std::thread> pool;
            for (int w = 0; w < workers; ++w) pool.emplace_back([&] {
                try {
                    for (size_t i = next++; i < targets.size() && !stop.stop_requested(); i = next++) {
                        const Target& target = targets[i];
                        const cv::Mat image = project.catalog.read(target.frame["recording"], target.frame["camera"], target.frame["index"]);
                        const Json& boxes = target.frame["boxes"];
                        Json hidden = Json::array();   // [{index, reason, box}]
                        for (size_t k = 0; k < boxes.size(); ++k) {
                            if (boxes[k].value("occluded", false)) continue;
                            const Verdict v = judge(boxes[k], boxes, k, image, o);
                            if (v.neighbour && !v.hidden) ++neighbours;
                            if (v.hidden) hidden.push_back({{"index", k}, {"reason", v.reason}, {"box", boxes[k]}});
                        }
                        if (!hidden.empty()) { std::lock_guard guard(resultsLock); results[target.key] = {{"hidden", hidden}, {"was", boxes}}; }
                        { std::lock_guard guard(project.lock); project.occlusionJob["done"] = ++done; }
                    }
                } catch (const std::exception& error) { std::lock_guard guard(failureLock); if (failure.empty()) failure = error.what(); }
            });
            for (auto& thread : pool) thread.join();
            if (!failure.empty()) throw std::runtime_error(failure);
            // Samples spread over the frames with a flagged ball, each with the reason.
            Json samples = Json::array(); int framesFound = int(results.size()), ballsFound = 0;
            for (const auto& [key, result] : results) ballsFound += int(result["hidden"].size());
            if (framesFound > 0 && o.sampleLimit > 0) {
                const double step = std::max(1.0, double(framesFound) / o.sampleLimit); double at = 0; int seen = 0;
                for (const auto& [key, result] : results)
                    if (seen++ >= int(at) && int(samples.size()) < o.sampleLimit) { at += step; const Json& first = result["hidden"][0]; samples.push_back({{"key", key}, {"box", first["box"]}, {"count", result["hidden"].size()}, {"note", first["reason"]}}); }
            }
            std::lock_guard guard(project.lock);
            if (stop.stop_requested()) { project.occlusionJob["status"] = "stopped"; return; }
            project.occlusionJob["frames"] = framesFound; project.occlusionJob["balls"] = ballsFound; project.occlusionJob["neighbours"] = neighbours.load(); project.occlusionJob["samples"] = samples;
            if (o.dryRun) { project.occlusionJob["status"] = "complete"; project.occlusionJob["finished_at"] = utcIso(); return; }
            int applied = 0, appliedBalls = 0, skipped = 0;
            const std::string runId = project.occlusionJob.value("run_id", "");
            project.mutateAutomatic([&](Json& state) {
                for (const auto& [key, result] : results) {
                    if (!state["frames"].contains(key)) continue;
                    Json& frame = state["frames"][key];
                    if (frame["boxes"] != result["was"]) { ++skipped; continue; }   // changed meanwhile
                    for (const auto& item : result["hidden"]) { Json& box = frame["boxes"][item["index"].get<size_t>()]; setOccludedFlag(box, true); ++appliedBalls; }
                    frame["automatic"]["occlusion_mark"] = {{"run_id", runId}, {"balls", result["hidden"].size()}};
                    ++applied;
                }
                if (applied > 0 || !state.contains("occlusion_mark")) state["occlusion_mark"] = {{"at", utcIso()}, {"frames", applied}, {"balls", appliedBalls}, {"run_id", runId}, {"options", options}};
            });
            project.occlusionJob["applied_frames"] = applied; project.occlusionJob["applied_balls"] = appliedBalls; project.occlusionJob["skipped"] = skipped;
            project.occlusionJob["status"] = "complete"; project.occlusionJob["finished_at"] = utcIso();
        } catch (const std::exception& error) {
            std::lock_guard guard(project.lock); project.occlusionJob["status"] = "error"; project.occlusionJob["error"] = error.what();
        }
    });
    return project.occlusionJob;
}
}
