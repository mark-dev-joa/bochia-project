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
const Json& shownBox(const Json& box) { return box.contains("detection_bbox") ? box["detection_bbox"] : box["bbox"]; }
// A detector ball counts as missing when no box of the frame has its centre within the ball's radius and no box overlaps it.
bool missing(const Json& boxes, const Detection& d, const Json& bounds) {
    for (const auto& box : boxes) {
        const Json& b = shownBox(box);
        const double cx = b[0].get<double>() + b[2].get<double>() / 2, cy = b[1].get<double>() + b[3].get<double>() / 2;
        if (std::hypot(cx - d.x, cy - d.y) <= std::max(6.0, d.radius) || overlap(bounds, box["bbox"]) > .3) return false;
    }
    return true;
}
std::vector<Detection> candidates(const Json& frame, const std::vector<Detection>& detections, double coverage, double minRadius, double maxRadius) {
    std::vector<Detection> out;
    const int width = frame.value("width", 0), height = frame.value("height", 0);
    for (const auto& d : detections) {
        if (d.coverage < coverage || d.radius < minRadius || d.radius > maxRadius) continue;
        if (!missing(frame["boxes"], d, seedBounds(d, width, height))) continue;
        out.push_back(d);
    }
    return out;
}
struct Target { std::string key; Json frame; std::vector<Detection> balls; bool colour; };
// Two trial balls are the same ball when their centres lie within a radius of each other or their boxes mostly overlap.
bool sameBall(const Json& x, const Json& y) {
    const Json& a = shownBox(x); const Json& b = shownBox(y);
    const double ax = a[0].get<double>() + a[2].get<double>() / 2, ay = a[1].get<double>() + a[3].get<double>() / 2;
    const double bx = b[0].get<double>() + b[2].get<double>() / 2, by = b[1].get<double>() + b[3].get<double>() / 2;
    const double r = std::max(6.0, std::min(a[2].get<double>(), a[3].get<double>()) / 2);
    return std::hypot(ax - bx, ay - by) <= r || overlap(x["bbox"], y["bbox"]) > .5;
}
bool matches(const Json& box, const Json& others) { for (const auto& other : others) if (sameBall(box, other)) return true; return false; }
fs::path trialsPath(const Project& project) { fs::path path = project.path; path.replace_extension(); path += "-trials.json"; return path; }
Json& trials(Project& project) {   // under the project lock
    if (project.missingTrials.is_null()) {
        project.missingTrials = Json::object();
        const auto path = trialsPath(project);
        if (fs::is_regular_file(path)) { try { std::ifstream in(path, std::ios::binary); Json loaded = Json::parse(in); if (loaded.is_object()) project.missingTrials = std::move(loaded); } catch (...) {} }
    }
    return project.missingTrials;
}
void storeTrials(Project& project) { atomicJson(trialsPath(project), project.missingTrials, true); }
Json trialSummary(const Json& trial) {
    return {{"name", trial.value("name", "")}, {"at", trial.value("at", "")}, {"options", trial.value("options", Json())}, {"derived", trial.value("derived", Json())},
            {"frames", trial.value("frames", 0)}, {"balls", trial.value("balls", 0)}};
}
void countTrial(Json& trial) {
    int frames = 0, balls = 0;
    for (const auto& [key, result] : trial["results"].items()) if (!result["boxes"].empty()) { ++frames; balls += int(result["boxes"].size()); }
    trial["frames"] = frames; trial["balls"] = balls;
}
// Writes a set of results (key → {boxes, was, width, height}) on the automatic revision. A frame a person touched since,
// or whose draft changed since the balls were found, is left alone.
Json applyResults(Project& project, const Json& results, const std::string& runId, const Json& record) {
    int applied = 0, appliedBalls = 0, skipped = 0;
    project.mutateAutomatic([&](Json& state) {
        for (const auto& [key, result] : results.items()) {
            if (result["boxes"].empty() || !state["frames"].contains(key)) continue;
            Json& frame = state["frames"][key];
            if (humanWork(frame) || frame["boxes"] != result["was"]) { ++skipped; continue; }
            Json boxes = frame["boxes"];
            for (const auto& box : result["boxes"]) boxes.push_back(box);
            frame["boxes"] = validateBoxes(boxes, result["width"], result["height"]);
            frame["automatic"]["missing_fill"] = {{"run_id", runId}, {"added", result["boxes"].size()}};
            ++applied; appliedBalls += int(result["boxes"].size());
        }
        Json entry = record; entry["at"] = utcIso(); entry["frames"] = applied; entry["balls"] = appliedBalls; entry["run_id"] = runId;
        if (applied > 0 || !state.contains("missing_fill")) state["missing_fill"] = entry;   // a run that added nothing keeps the last real record
    });
    return {{"applied_frames", applied}, {"applied_balls", appliedBalls}, {"skipped", skipped}};
}
Json spreadSamples(const Json& results, int limit) {
    Json samples = Json::array(); int framesFound = 0;
    for (const auto& [key, result] : results.items()) if (!result["boxes"].empty()) ++framesFound;
    if (framesFound == 0 || limit <= 0) return samples;
    const double step = std::max(1.0, double(framesFound) / limit); double at = 0; int seen = 0;
    for (const auto& [key, result] : results.items()) {
        if (result["boxes"].empty()) continue;
        if (seen++ >= int(at) && int(samples.size()) < limit) { at += step; samples.push_back({{"key", key}, {"box", result["boxes"][0]}, {"count", result["boxes"].size()}}); }
    }
    return samples;
}
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
    const Json options = {{"cameras", o.cameras}, {"coverage", o.coverage}, {"min_radius", o.minRadius}, {"max_radius", o.maxRadius}, {"margin", o.margin}, {"limit_frames", o.limitFrames}};
    // The frames a person has not touched, of the wanted cameras, that hold any detector ball the draft lacks under the
    // loosest conditions: the population a trial samples from, so trials with different options compare on the same frames.
    std::vector<Target> population; int scanned = 0;
    for (const auto& [key, frame] : project.state["frames"].items()) {
        if (humanWork(frame)) continue;
        const bool colour = project.catalog.camera(frame["recording"], frame["camera"]).kind == "컬러";
        if (o.cameras == "colour" && !colour) continue;
        ++scanned;
        const auto found = detections.find(key);
        if (found == detections.end() || candidates(frame, found->second, .5, 2, 300).empty()) continue;
        population.push_back({key, frame, {}, colour});
    }
    if (o.limitFrames > 0 && int(population.size()) > o.limitFrames) {   // every n-th frame, so every recording is represented
        std::vector<Target> chosen; const double step = double(population.size()) / o.limitFrames;
        for (int i = 0; i < o.limitFrames; ++i) chosen.push_back(std::move(population[size_t(i * step)]));
        population = std::move(chosen);
    }
    std::vector<Target> targets; int candidateCount = 0;
    for (auto& target : population) {
        target.balls = candidates(target.frame, detections.at(target.key), o.coverage, o.minRadius, o.maxRadius);
        if (target.balls.empty()) continue;
        candidateCount += int(target.balls.size()); targets.push_back(std::move(target));
    }
    project.missingJob = {{"status", o.countOnly ? "complete" : "running"}, {"run_id", localStamp() + "-" + randomHex(4)}, {"options", options},
                          {"dry_run", o.dryRun || o.countOnly}, {"count_only", o.countOnly}, {"scanned", scanned}, {"frames_with_candidates", targets.size()},
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
            Json found = Json::object(); int ballsFound = 0, framesFound = 0;
            for (const auto& [key, result] : results) if (!result["boxes"].empty()) { found[key] = result; ++framesFound; ballsFound += int(result["boxes"].size()); }
            std::lock_guard guard(project.lock);
            if (stop.stop_requested()) { project.missingJob["status"] = "stopped"; return; }
            project.missingJob["frames"] = framesFound; project.missingJob["balls"] = ballsFound; project.missingJob["samples"] = spreadSamples(found, o.sampleLimit);
            if (o.dryRun) {   // kept for a look and for saving as a trial; the labels are untouched
                project.missingJob["results"] = found;
                project.missingJob["status"] = "complete"; project.missingJob["finished_at"] = utcIso();
                return;
            }
            const Json applied = applyResults(project, found, project.missingJob.value("run_id", ""), {{"options", o.cameras}, {"coverage", o.coverage}});
            for (const auto& [name, value] : applied.items()) project.missingJob[name] = value;
            project.missingJob["status"] = "complete"; project.missingJob["finished_at"] = utcIso();
        } catch (const std::exception& error) {
            std::lock_guard guard(project.lock); project.missingJob["status"] = "error"; project.missingJob["error"] = error.what();
        }
    });
    return project.missingJob;
}

// ---- trials ---------------------------------------------------------------------------------

Json missingTrials(Project& project) {
    std::lock_guard guard(project.lock);
    Json list = Json::array();
    for (const auto& [name, trial] : trials(project).items()) list.push_back(trialSummary(trial));
    std::sort(list.begin(), list.end(), [](const Json& a, const Json& b) { return a.value("at", "") < b.value("at", ""); });
    return {{"trials", list}};
}
Json saveMissingTrial(Project& project, const std::string& name) {
    std::lock_guard guard(project.lock);
    if (name.empty() || name.size() > 32) throw std::runtime_error("시험 이름은 1~32자여야 합니다.");
    const Json& job = project.missingJob;
    if (job.value("status", "") != "complete" || !job.value("dry_run", false) || job.value("count_only", false) || !job.contains("results")) throw std::runtime_error("저장할 시험 결과가 없습니다. 먼저 시험 실행을 끝내세요.");
    Json trial = {{"name", name}, {"at", utcIso()}, {"options", job["options"]}, {"derived", nullptr}, {"results", job["results"]}};
    countTrial(trial);
    trials(project)[name] = std::move(trial); storeTrials(project);
    return missingTrials(project);
}
Json combineMissingTrials(Project& project, const std::string& name, const std::string& op, const std::string& a, const std::string& b) {
    std::lock_guard guard(project.lock);
    if (name.empty() || name.size() > 48) throw std::runtime_error("조립 이름은 1~48자여야 합니다.");
    if (op != "or" && op != "and" && op != "and_not") throw std::runtime_error("조립 연산은 or, and, and_not 중 하나여야 합니다.");
    Json& all = trials(project);
    if (!all.contains(a) || !all.contains(b)) throw std::runtime_error("조립할 시험이 없습니다.");
    const Json& left = all[a]["results"]; const Json& right = all[b]["results"];
    Json results = Json::object();
    auto meta = [&](const std::string& key, Json& out) { const Json& src = left.contains(key) ? left[key] : right[key]; out["was"] = src["was"]; out["width"] = src["width"]; out["height"] = src["height"]; };
    std::vector<std::string> keys;
    for (const auto& [key, value] : left.items()) keys.push_back(key);
    if (op == "or") for (const auto& [key, value] : right.items()) if (!left.contains(key)) keys.push_back(key);
    for (const auto& key : keys) {
        const Json empty = Json::array();
        const Json& lb = left.contains(key) ? left[key]["boxes"] : empty; const Json& rb = right.contains(key) ? right[key]["boxes"] : empty;
        Json boxes = Json::array();
        if (op == "or") { for (const auto& box : lb) boxes.push_back(box); for (const auto& box : rb) if (!matches(box, lb)) boxes.push_back(box); }
        else if (op == "and") { for (const auto& box : lb) if (matches(box, rb)) boxes.push_back(box); }
        else { for (const auto& box : lb) if (!matches(box, rb)) boxes.push_back(box); }
        if (boxes.empty()) continue;
        Json out = {{"boxes", boxes}}; meta(key, out); results[key] = std::move(out);
    }
    const std::string expression = a + (op == "or" ? " or " : op == "and" ? " and " : " and not ") + b;
    Json trial = {{"name", name}, {"at", utcIso()}, {"options", nullptr}, {"derived", expression}, {"results", results}};
    countTrial(trial);
    all[name] = std::move(trial); storeTrials(project);
    return missingTrials(project);
}
Json dropMissingTrial(Project& project, const std::string& name) {
    std::lock_guard guard(project.lock);
    trials(project).erase(name); storeTrials(project);
    return missingTrials(project);
}
Json missingTrialSamples(Project& project, const std::string& name, int limit) {
    std::lock_guard guard(project.lock);
    Json& all = trials(project);
    if (!all.contains(name)) throw std::runtime_error("그 이름의 시험이 없습니다.");
    return {{"name", name}, {"samples", spreadSamples(all[name]["results"], std::clamp(limit, 1, 64))}};
}
Json applyMissingTrial(Project& project, const std::string& name) {
    std::lock_guard guard(project.lock);
    if (project.missingJob.value("status", "") == "running") throw std::runtime_error("누락 공 작업이 끝난 뒤 적용하세요.");
    Json& all = trials(project);
    if (!all.contains(name)) throw std::runtime_error("그 이름의 시험이 없습니다.");
    const Json& trial = all[name];
    Json record = {{"trial", name}, {"options", trial.value("options", Json())}, {"derived", trial.value("derived", Json())}};
    Json result = applyResults(project, trial["results"], localStamp() + "-" + randomHex(4), record);
    result["name"] = name;
    return result;
}
}
