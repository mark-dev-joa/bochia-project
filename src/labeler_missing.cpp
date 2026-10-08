#include "labeler_missing.hpp"
#include "labeler_assist.hpp"
#include "labeler_sam.hpp"
#include "labeler_util.hpp"
#include "native_labeler_model.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <thread>

namespace boccia::labeler {
namespace fs = std::filesystem;
namespace {
bool truthy(const Json& value) { return value.is_boolean() ? value.get<bool>() : !value.is_null() && !(value.is_number() && value.get<double>() == 0); }
bool humanWork(const Json& frame) { return truthy(frame.value("human_touched", Json(false))) || truthy(frame.value("reviewed", Json(false))); }
Json seedBounds(const Detection& d, int width, int height) {
    const int left = std::max(0, int(std::floor(d.x - d.radius))), top = std::max(0, int(std::floor(d.y - d.radius)));
    const int right = std::min(width, int(std::ceil(d.x + d.radius))), bottom = std::min(height, int(std::ceil(d.y + d.radius)));
    return {left, top, std::max(1, right - left), std::max(1, bottom - top)};
}
// A detector ball counts as missing when no box of the frame has its centre within the ball's radius and no box overlaps it.
bool missing(const Json& boxes, const Detection& d, const Json& bounds) {
    for (const auto& box : boxes) {
        const Json& b = box.contains("detection_bbox") ? box["detection_bbox"] : box["bbox"];
        const double cx = b[0].get<double>() + b[2].get<double>() / 2, cy = b[1].get<double>() + b[3].get<double>() / 2;
        if (std::hypot(cx - d.x, cy - d.y) <= std::max(6.0, d.radius) || overlap(bounds, box["bbox"]) > .3) return false;
    }
    return true;
}
std::vector<Detection> candidates(const Json& frame, const std::vector<Detection>& detections, const MissingOptions& o) {
    std::vector<Detection> out;
    const int width = frame.value("width", 0), height = frame.value("height", 0);
    for (const auto& d : detections) {
        if (d.coverage < o.coverage || d.radius < o.minRadius || d.radius > o.maxRadius) continue;
        if (!missing(frame["boxes"], d, seedBounds(d, width, height))) continue;
        out.push_back(d);
    }
    return out;
}
struct Target { std::string key; Json frame; std::vector<Detection> balls; bool colour; };
}

std::map<std::string, std::vector<Detection>> loadDetections(const Catalog& catalog, const fs::path& root) {
    std::map<std::string, std::vector<Detection>> out;
    std::map<std::string, const Recording*> byName;
    for (const auto& recording : catalog.recordings) byName[recording.name] = &recording;
    const fs::path path = root / "output/ball-detection-2026-10-02/full/detections.jsonl";
    if (!fs::is_regular_file(path)) return out;
    std::ifstream file(path, std::ios::binary);
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line == "\r") continue;
        const Json entry = Json::parse(line);
        const auto rec = byName.find(entry["session"].get<std::string>());
        if (rec == byName.end()) continue;
        const std::string camera = "camera-" + std::to_string(entry["camera"].get<int>());
        const CameraStream* stream = rec->second->camera(camera);
        if (!stream) continue;
        const int frame = entry["frame"].get<int>();
        if (frame < 0 || size_t(frame) >= stream->frames.size()) continue;
        const FrameInfo& info = stream->frames[size_t(frame)];
        const Json serial = entry.value("serial", Json()), sourceId = entry.value("source_id", Json());
        if (!stream->hasSerial || !serial.is_string() || stream->serial != serial.get<std::string>()) continue;
        if (!info.hasHostNs || info.hostNs != entry["host_ns"].get<int64_t>()) continue;
        if (!info.hasId || !sourceId.is_number() || info.id != sourceId.get<uint64_t>()) continue;
        auto& balls = out[rec->second->id + "/" + camera + "/" + std::to_string(frame)];
        for (const auto& b : entry["balls"]) balls.push_back({b.value("x", 0.0), b.value("y", 0.0), b.value("radius", 0.0), b.value("coverage", 0.0)});
    }
    return out;
}

MissingOptions missingOptions(const Json& payload, const Project& project) {
    MissingOptions o;
    o.cameras = payload.value("cameras", "colour");
    if (o.cameras != "colour" && o.cameras != "all") throw std::runtime_error("카메라 범위는 colour 또는 all이어야 합니다.");
    o.coverage = payload.value("coverage", .8);
    if (!std::isfinite(o.coverage) || o.coverage < .5 || o.coverage > 1) throw std::runtime_error("커버리지 기준은 0.5에서 1 사이여야 합니다.");
    o.minRadius = payload.value("min_radius", 6.0); o.maxRadius = payload.value("max_radius", 45.0);
    if (!std::isfinite(o.minRadius) || !std::isfinite(o.maxRadius) || o.minRadius < 2 || o.maxRadius > 300 || o.minRadius > o.maxRadius) throw std::runtime_error("반지름 범위가 올바르지 않습니다.");
    { std::lock_guard guard(project.lock); o.margin = payload.value("margin", project.state.value("circle_margin", defaultCircleMargin)); }
    if (!std::isfinite(o.margin) || o.margin < -2 || o.margin > 3) throw std::runtime_error("둘레 여유는 -2에서 3픽셀 사이여야 합니다.");
    o.dryRun = payload.value("dry_run", false); o.countOnly = payload.value("count_only", false);
    o.limitFrames = std::clamp(payload.value("limit_frames", 0), 0, 100000); o.sampleLimit = std::clamp(payload.value("sample_limit", 8), 0, 64);
    o.name = payload.value("name", "");
    return o;
}
Json missingFillStatus(const Project& project) { std::lock_guard guard(project.lock); return project.missingJob; }
Json stopMissingFill(Project& project) { std::lock_guard guard(project.lock); project.missingWorker.request_stop(); return project.missingJob; }

Json startMissingFill(Project& project, const fs::path& root, const fs::path& modelDir, const MissingOptions& o) {
    std::lock_guard guard(project.lock);
    if (project.missingJob.value("status", "") == "running") throw std::runtime_error("이미 누락 공을 채우는 중입니다.");
    if (!o.countOnly && !samModelFilesExist(modelDir)) throw std::runtime_error("자동 분할 모델을 먼저 설치하세요.");
    const auto detections = loadDetections(project.catalog, root);
    if (detections.empty()) throw std::runtime_error("검출기 결과 파일(output/ball-detection-2026-10-02/full/detections.jsonl)이 없거나 이 촬영과 맞지 않습니다.");
    const Json options = {{"cameras", o.cameras}, {"coverage", o.coverage}, {"min_radius", o.minRadius}, {"max_radius", o.maxRadius}, {"margin", o.margin},
                          {"limit_frames", o.limitFrames}, {"name", o.name}};
    // The frames a person has not touched, of the wanted cameras, that hold a detector ball the draft lacks.
    std::vector<Target> targets; int candidateCount = 0, scanned = 0;
    for (const auto& [key, frame] : project.state["frames"].items()) {
        if (humanWork(frame)) continue;
        const bool colour = project.catalog.camera(frame["recording"], frame["camera"]).kind == "컬러";
        if (o.cameras == "colour" && !colour) continue;
        ++scanned;
        const auto found = detections.find(key);
        if (found == detections.end()) continue;
        auto balls = candidates(frame, found->second, o);
        if (balls.empty()) continue;
        candidateCount += int(balls.size());
        targets.push_back({key, frame, std::move(balls), colour});
    }
    const int framesWithCandidates = int(targets.size());
    if (o.limitFrames > 0 && int(targets.size()) > o.limitFrames) {   // a trial: every n-th frame, so every recording is represented
        std::vector<Target> chosen; const double step = double(targets.size()) / o.limitFrames;
        for (int i = 0; i < o.limitFrames; ++i) chosen.push_back(std::move(targets[size_t(i * step)]));
        targets = std::move(chosen);
    }
    project.missingJob = {{"status", o.countOnly ? "complete" : "running"}, {"run_id", localStamp() + "-" + randomHex(4)}, {"options", options},
                          {"dry_run", o.dryRun || o.countOnly}, {"count_only", o.countOnly}, {"scanned", scanned}, {"frames_with_candidates", framesWithCandidates},
                          {"candidates", candidateCount}, {"total", targets.size()}, {"done", 0}, {"frames", 0}, {"balls", 0}, {"sam", 0}, {"geometry", 0},
                          {"circles", 0}, {"skipped", 0}, {"samples", Json::array()}, {"started_at", utcIso()}};
    if (o.countOnly) return project.missingJob;
    if (project.missingWorker.joinable()) project.missingWorker.join();
    project.missingWorker = std::jthread([&project, targets = std::move(targets), o, modelDir](std::stop_token stop) {
        try {
            std::map<std::string, Json> results; std::mutex resultsLock;
            std::atomic<size_t> next{0}; std::atomic<int> done{0}, sam{0}, geometry{0}, circles{0};
            std::mutex failureLock; std::string failure;
            const int workers = std::max(1, std::min(4, int(std::thread::hardware_concurrency()) / 2));
            std::vector<std::thread> pool;
            for (int w = 0; w < workers; ++w) pool.emplace_back([&] {
                try {
                    BallAssist assist(modelDir);   // one model per thread, as the automatic draft does
                    for (size_t i = next++; i < targets.size() && !stop.stop_requested(); i = next++) {
                        const Target& target = targets[i];
                        const cv::Mat image = project.catalog.read(target.frame["recording"], target.frame["camera"], target.frame["index"]);
                        const int width = image.cols, height = image.rows;
                        Json added = Json::array(); int fits = 0, viaSam = 0, viaGeometry = 0;
                        for (const Detection& d : target.balls) {
                            const Json bounds = seedBounds(d, width, height);
                            Json box;
                            try {
                                box = assist.segment(image, target.key, Json::array({{{"x", d.x}, {"y", d.y}, {"label", 1}}}), d.radius, bounds);
                                if (overlap(box["bbox"], bounds) < .4 || box.value("area", 0.0) < 3.141592653589793 * d.radius * d.radius * .25) throw std::runtime_error("분할 영역이 후보와 다릅니다.");
                                box["source"] = "auto_sam"; ++viaSam;
                            } catch (const std::exception&) {
                                box = {{"bbox", bounds}, {"occluded", false}, {"source", "auto_geometry"}}; ++viaGeometry;
                            }
                            bool taken = false;
                            for (const auto& other : added) if (overlap(box["bbox"], other["bbox"]) > .6) taken = true;
                            if (taken) continue;
                            if (box.contains("segmentation") && refineBallBox(box, image, o.margin)) ++fits;
                            if (target.colour && box.contains("segmentation")) { const auto colour = ballColor(image, decodeMaskChecked(box["segmentation"], width, height)); if (colour != "unknown") box["color"] = colour; }
                            box["detector"] = {{"x", d.x}, {"y", d.y}, {"radius", d.radius}, {"coverage", d.coverage}};
                            added.push_back(box);
                        }
                        sam += viaSam; geometry += viaGeometry; circles += fits;
                        { std::lock_guard guard(resultsLock); results[target.key] = {{"boxes", added}, {"was", target.frame["boxes"]}, {"width", width}, {"height", height}}; }
                        { std::lock_guard guard(project.lock); project.missingJob["done"] = ++done; project.missingJob["sam"] = sam.load(); project.missingJob["geometry"] = geometry.load(); project.missingJob["circles"] = circles.load(); }
                    }
                } catch (const std::exception& error) { std::lock_guard guard(failureLock); if (failure.empty()) failure = error.what(); }
            });
            for (auto& thread : pool) thread.join();
            if (!failure.empty()) throw std::runtime_error(failure);
            // A few of the new balls, for a look before applying: spread over the frames that got any.
            Json samples = Json::array(); int ballsFound = 0, framesFound = 0;
            for (const auto& [key, result] : results) if (!result["boxes"].empty()) { ++framesFound; ballsFound += int(result["boxes"].size()); }
            if (framesFound > 0 && o.sampleLimit > 0) {
                const double step = std::max(1.0, double(framesFound) / o.sampleLimit); double at = 0; int seen = 0;
                for (const auto& [key, result] : results) {
                    if (result["boxes"].empty()) continue;
                    if (seen++ >= int(at) && int(samples.size()) < o.sampleLimit) { at += step; samples.push_back({{"key", key}, {"box", result["boxes"][0]}, {"count", result["boxes"].size()}}); }
                }
            }
            std::lock_guard guard(project.lock);
            if (stop.stop_requested()) { project.missingJob["status"] = "stopped"; return; }
            project.missingJob["frames"] = framesFound; project.missingJob["balls"] = ballsFound; project.missingJob["samples"] = samples;
            if (o.dryRun) {   // kept in memory for assembling trials later; the labels are untouched
                Json trial = Json::object();
                for (const auto& [key, result] : results) if (!result["boxes"].empty()) trial[key] = result["boxes"];
                project.missingJob["results"] = trial;
                project.missingJob["status"] = "complete"; project.missingJob["finished_at"] = utcIso();
                return;
            }
            int applied = 0, appliedBalls = 0, skipped = 0;
            const std::string runId = project.missingJob.value("run_id", "");
            project.mutateAutomatic([&](Json& state) {
                for (const auto& [key, result] : results) {
                    if (result["boxes"].empty() || !state["frames"].contains(key)) continue;
                    Json& frame = state["frames"][key];
                    if (humanWork(frame) || frame["boxes"] != result["was"]) { ++skipped; continue; }   // a person got there first, or the draft changed
                    Json boxes = frame["boxes"];
                    for (const auto& box : result["boxes"]) boxes.push_back(box);
                    frame["boxes"] = validateBoxes(boxes, result["width"], result["height"]);
                    frame["automatic"]["missing_fill"] = {{"run_id", runId}, {"added", result["boxes"].size()}};
                    ++applied; appliedBalls += int(result["boxes"].size());
                }
                state["missing_fill"] = {{"at", utcIso()}, {"frames", applied}, {"balls", appliedBalls}, {"options", o.cameras}, {"coverage", o.coverage}, {"run_id", runId}};
            });
            project.missingJob["applied_frames"] = applied; project.missingJob["applied_balls"] = appliedBalls; project.missingJob["skipped"] = skipped;
            project.missingJob["status"] = "complete"; project.missingJob["finished_at"] = utcIso();
        } catch (const std::exception& error) {
            std::lock_guard guard(project.lock); project.missingJob["status"] = "error"; project.missingJob["error"] = error.what();
        }
    });
    return project.missingJob;
}
}
