#include "labeler_auto.hpp"
#include "labeler_util.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <queue>
#include <stdexcept>

namespace boccia::labeler {
namespace fs = std::filesystem;
namespace {
std::string readText(const fs::path& path) { std::ifstream in(path, std::ios::binary); return std::string((std::istreambuf_iterator<char>(in)), {}); }
double distance(const Seed& a, const Seed& b) { return std::hypot(a.x - b.x, a.y - b.y); }
double meanAll(const cv::Mat& m) { const auto s = cv::sum(m); return (s[0] + s[1] + s[2]) / (double(m.total()) * m.channels()); }
}
Json seedBbox(const Seed& seed, int width, int height) {
    const int left = std::max(0, int(std::floor(seed.x - seed.radius))), top = std::max(0, int(std::floor(seed.y - seed.radius)));
    const int right = std::min(width, int(std::ceil(seed.x + seed.radius))), bottom = std::min(height, int(std::ceil(seed.y + seed.radius)));
    return {left, top, right - left, bottom - top};
}
cv::Mat appearance(const cv::Mat& image, const Seed& seed) {
    const int side = std::max(8, int(std::nearbyint(seed.radius * 2.4)));
    cv::Mat patch, small, result;
    cv::getRectSubPix(image, {side, side}, {float(seed.x), float(seed.y)}, patch);
    cv::resize(patch, small, {40, 40}, 0, 0, cv::INTER_AREA);
    small.convertTo(result, CV_32F);
    return result;
}
bool similar(const cv::Mat& a, const cv::Mat& b) {
    const cv::Mat ca = a - cv::Scalar::all(meanAll(a)), cb = b - cv::Scalar::all(meanAll(b));
    const double denominator = std::sqrt(ca.dot(ca)) * std::sqrt(cb.dot(cb));
    cv::Mat delta; cv::absdiff(a, b, delta);
    return denominator > 1 && ca.dot(cb) / denominator >= .96 && meanAll(delta) <= 6;
}
Json transferMask(const Json& previousBox, const Seed& previousSeed, const Seed& seed, cv::Size shape) {
    const double ratio = seed.radius / previousSeed.radius;
    const cv::Mat mask = decodeMaskChecked(previousBox["segmentation"], shape.width, shape.height);
    const cv::Mat transform = (cv::Mat_<float>(2, 3) << float(ratio), 0, float(seed.x - ratio * previousSeed.x), 0, float(ratio), float(seed.y - ratio * previousSeed.y));
    cv::Mat moved; cv::warpAffine(mask, moved, transform, shape, cv::INTER_NEAREST);
    if (cv::countNonZero(moved) == 0) throw std::runtime_error("추적 마스크가 원본 밖으로 이동했습니다.");
    const auto bounds = cv::boundingRect(moved);
    return {{"bbox", {bounds.x, bounds.y, bounds.width, bounds.height}}, {"segmentation", encodeMask(moved)}, {"occluded", false}, {"source", "auto_tracking"}};
}
SeedCache::SeedCache(const Catalog& catalog, const fs::path& root) {
    std::map<std::string, const Recording*> byName;
    for (const auto& recording : catalog.recordings) byName[recording.name] = &recording;
    const fs::path detectionsPath = root / "output/ball-detection-2026-10-02/full/detections.jsonl";
    if (!fs::is_regular_file(detectionsPath)) return;
    std::map<std::tuple<std::string, std::string, int>, Json> detections;
    std::ifstream detectionsFile(detectionsPath, std::ios::binary);
    for (std::string line; std::getline(detectionsFile, line);) {
        if (line.empty() || line == "\r") continue;
        const Json entry = Json::parse(line);
        const auto rec = byName.find(entry["session"].get<std::string>());
        const std::string camera = "camera-" + std::to_string(entry["camera"].get<int>());
        if (rec == byName.end()) continue;
        const CameraStream* stream = rec->second->camera(camera);
        if (!stream) continue;
        const int frame = entry["frame"].get<int>();
        if (frame < 0 || size_t(frame) >= stream->frames.size()) continue;
        const FrameInfo& info = stream->frames[size_t(frame)];
        const Json serial = entry.value("serial", Json()), sourceId = entry.value("source_id", Json());
        if (!stream->hasSerial || !serial.is_string() || stream->serial != serial.get<std::string>()) continue;
        if (!info.hasHostNs || info.hostNs != entry["host_ns"].get<int64_t>()) continue;
        if (!info.hasId || !sourceId.is_number() || info.id != sourceId.get<uint64_t>()) continue;
        const auto key = std::make_tuple(rec->second->id, camera, frame);
        detections[key] = entry["balls"];
        covered_.insert(key);
    }
    const fs::path sessions = root / "output/mono-color-2026-10-03/final-sessions";
    std::vector<fs::path> matchFiles;
    if (fs::is_directory(sessions)) for (const auto& entry : fs::directory_iterator(sessions)) if (fs::is_regular_file(entry.path() / "matches.jsonl")) matchFiles.push_back(entry.path() / "matches.jsonl");
    std::sort(matchFiles.begin(), matchFiles.end(), pathLess);
    for (const auto& path : matchFiles) {
        const auto rec = byName.find(utf8(path.parent_path().filename()));
        if (rec == byName.end()) continue;
        std::map<std::string, std::string> cameraBySerial;
        for (const auto& stream : rec->second->cameras) if (stream.hasSerial) cameraBySerial[stream.serial] = stream.id;
        std::ifstream matches(path, std::ios::binary);
        for (std::string line; std::getline(matches, line);) {
            if (line.empty() || line == "\r") continue;
            const Json entry = Json::parse(line);
            if (!entry.value("timing_accepted", false)) continue;
            for (const auto& ball : entry["balls"]) {
                if (ball.value("floor_warning", false)) continue;
                const Json& pixels = ball["raw_pixels"];
                for (size_t slot = 0; slot < pixels.size(); ++slot) {
                    if (pixels[slot].is_null()) continue;
                    const auto camera = cameraBySerial.find(entry["rig_serials"][slot].get<std::string>());
                    if (camera == cameraBySerial.end()) continue;
                    const auto key = std::make_tuple(rec->second->id, camera->second, entry["selected_frames"][slot].get<int>());
                    const auto candidates = detections.find(key);
                    if (candidates == detections.end() || candidates->second.empty()) continue;
                    const double px = pixels[slot][0], py = pixels[slot][1];
                    const Json* nearest = nullptr; double best = 0;
                    for (const auto& candidate : candidates->second) {
                        const double d = std::hypot(candidate["x"].get<double>() - px, candidate["y"].get<double>() - py);
                        if (!nearest || d < best) { nearest = &candidate; best = d; }
                    }
                    if (best > .5) continue;
                    const Seed seed{(*nearest)["x"], (*nearest)["y"], (*nearest)["radius"]};
                    auto& seeds = seeds_[key];
                    if (std::none_of(seeds.begin(), seeds.end(), [&](const Seed& s) { return distance(s, seed) < 2; })) seeds.push_back(seed);
                }
            }
        }
    }
    if (!covered_.empty()) method_ = "stereo_associated_native_circles";
}
size_t SeedCache::seedCount() const { size_t count = 0; for (const auto& [key, seeds] : seeds_) count += seeds.size(); return count; }
std::pair<std::vector<Seed>, std::string> SeedCache::get(const std::string& recording, const std::string& camera, int index, const cv::Mat& image) {
    const auto key = std::make_tuple(recording, camera, index);
    if (covered_.count(key)) { const auto found = seeds_.find(key); return {found == seeds_.end() ? std::vector<Seed>{} : found->second, "stereo_associated_native_circles"}; }
    // A new input has no stereo cache; all of these still require human review.
    const Json proposals = BallAssist::circles(image, 6, 45);
    std::vector<Seed> seeds;
    for (size_t i = 0; i < proposals.size() && i < 12; ++i) {
        const Json& b = proposals[i]["bbox"];
        seeds.push_back({b[0].get<double>() + b[2].get<double>() / 2, b[1].get<double>() + b[3].get<double>() / 2, std::max(b[2].get<double>(), b[3].get<double>()) / 2});
    }
    return {seeds, "circle_candidates"};
}
namespace {
class ModelSegmenter : public Segmenter {
public:
    explicit ModelSegmenter(const fs::path& modelDir) : assist_(modelDir) {}
    Json segment(const cv::Mat& image, const std::string& key, const Json& points, const Json& radius, const Json& seedBox) override { return assist_.segment(image, key, points, radius, seedBox); }
private:
    BallAssist assist_;
};
}
AutoLabelJob::AutoLabelJob(Project& project, fs::path root, fs::path modelDir)
    : statusPath(project.path.parent_path() / (project.path.stem().wstring() + L"-auto-status.json")), project_(project), root_(std::move(root)), modelDir_(std::move(modelDir)) {
    const fs::path previousPath = fs::exists(statusPath) ? statusPath : project.path.parent_path() / "auto-status.json";
    if (fs::exists(previousPath)) {
        try {
            const Json previous = Json::parse(readText(previousPath));
            if (previous.is_object() && previous.value("project_path", "") == utf8(project.path)) {
                state_ = previous;
                if (state_.value("status", "") == "running") { state_["status"] = "interrupted"; state_["message"] = "중단된 작업입니다. 자동 초안을 다시 시작하면 미처리 프레임부터 이어갑니다."; }
            }
        } catch (...) {}
    }
}
AutoLabelJob::~AutoLabelJob() { stop_ = true; if (worker_.joinable()) worker_.join(); }
Json AutoLabelJob::status() const { std::lock_guard guard(lock_); return state_; }
bool AutoLabelJob::ready() const { return samModelFilesExist(modelDir_); }
Json AutoLabelJob::start(std::function<std::unique_ptr<SeedSource>()> seeds, std::function<std::unique_ptr<Segmenter>()> segmenters) {
    if (!segmenters && !ready()) throw std::runtime_error("자동 분할 모델을 먼저 설치하세요.");
    std::lock_guard guard(lock_);
    if (state_.value("status", "") == "running") return state_;
    if (worker_.joinable()) worker_.join();
    state_ = {{"status", "running"}, {"run_id", localStamp() + "-" + randomHex(4)}, {"project_path", utf8(project_.path)},
              {"source_signature", project_.catalog.signature}, {"total", project_.catalog.totalFrames()}, {"done", 0},
              {"written", 0}, {"preserved", 0}, {"boxes", 0}, {"sam_masks", 0}, {"tracked_masks", 0},
              {"fallback_boxes", 0}, {"failed_frames", 0}, {"started_at", utcIso()}};
    stop_ = false;
    worker_ = std::thread([this, seeds = std::move(seeds), segmenters = std::move(segmenters)] { run(seeds, segmenters); });
    return state_;
}
Json AutoLabelJob::stop() { stop_ = true; return status(); }
void AutoLabelJob::persist() { std::lock_guard guard(lock_); atomicJson(statusPath, state_, true); }
void AutoLabelJob::run(std::function<std::unique_ptr<SeedSource>()> seedFactory, std::function<std::unique_ptr<Segmenter>()> segmenterFactory) {
    const auto started = std::chrono::steady_clock::now();
    auto bump = [this](const char* name, int by = 1) { std::lock_guard guard(lock_); state_[name] = state_[name].get<int>() + by; };
    try {
        std::unique_ptr<SeedSource> cache = seedFactory ? seedFactory() : std::make_unique<SeedCache>(project_.catalog, root_);
        { std::lock_guard guard(lock_); state_["seed_method"] = cache->method(); }
        std::mutex cacheLock;   // the circle fallback inside get() is stateless, but the seed tables are shared
        const std::string runId = state_["run_id"];
        auto cameraTask = [&](const Recording& recording, const CameraStream& stream, Segmenter& segmenter) {
            std::vector<std::pair<std::string, Json>> pending;
            struct Prior { Json box; Seed seed; cv::Mat patch; int anchor; };
            std::vector<Prior> previous;
            auto flush = [&] {
                const int written = project_.commitAutomatic(pending); pending.clear();
                bump("written", written); persist();
            };
            for (int index = 0; index < int(stream.frames.size()); ++index) {
                if (stop_) break;
                const std::string key = recording.id + "/" + stream.id + "/" + std::to_string(index);
                bool preserved;
                { std::lock_guard guard(project_.lock); preserved = project_.state["frames"].contains(key); }
                if (preserved) { bump("preserved"); bump("done"); continue; }
                const cv::Mat image = project_.catalog.read(recording.id, stream.id, index);
                const int height = image.rows, width = image.cols;
                std::vector<Seed> seeds; std::string method;
                { std::lock_guard guard(cacheLock); std::tie(seeds, method) = cache->get(recording.id, stream.id, index, image); }
                Json boxes = Json::array(); std::vector<Prior> next; std::vector<std::string> notes;
                for (const Seed& seed : seeds) {
                    const Json bounds = seedBbox(seed, width, height);
                    const cv::Mat patch = appearance(image, seed);
                    const Prior* prior = nullptr;
                    for (const auto& candidate : previous) if (!prior || distance(candidate.seed, seed) < distance(prior->seed, seed)) prior = &candidate;
                    Json box; int anchor = index;
                    if (prior && index - prior->anchor < 24 && std::abs(seed.radius / prior->seed.radius - 1) < .15 && distance(prior->seed, seed) < std::max(20.0, seed.radius * 2) && similar(patch, prior->patch)) {
                        box = transferMask(prior->box, prior->seed, seed, {width, height}); anchor = prior->anchor; bump("tracked_masks");
                    }
                    if (box.is_null()) {
                        try {
                            box = segmenter.segment(image, key, Json::array({{{"x", seed.x}, {"y", seed.y}, {"label", 1}}}), seed.radius, bounds);
                            if (overlap(box["bbox"], bounds) < .4 || box.value("area", 0.0) < 3.141592653589793 * seed.radius * seed.radius * .25) throw std::runtime_error("분할 영역이 시작 후보와 다릅니다.");
                            box["source"] = "auto_sam"; anchor = index; bump("sam_masks");
                        } catch (const std::runtime_error& error) {
                            box = {{"bbox", bounds}, {"occluded", false}, {"source", "auto_geometry"}}; notes.push_back(error.what()); bump("fallback_boxes");
                        }
                    }
                    bool taken = false;
                    for (const auto& other : boxes) if (overlap(box["bbox"], other["bbox"]) > .6) taken = true;
                    if (taken) continue;
                    boxes.push_back(box);
                    if (box.contains("segmentation")) next.push_back({box, seed, patch, anchor});
                }
                previous = std::move(next);
                if (stream.kind == "컬러")   // the colour camera also says which ball it is
                    for (auto& box : boxes) if (box.contains("segmentation")) box["color"] = ballColor(image, decodeMaskChecked(box["segmentation"], width, height));
                boxes = validateBoxes(boxes, width, height);
                Json automatic = {{"run_id", runId}, {"seed_method", method}, {"seed_count", seeds.size()},
                                  {"needs_attention", !notes.empty() || boxes.empty() || method == "circle_candidates"}, {"notes", Json::array()}};
                for (size_t i = 0; i < notes.size() && i < 10; ++i) automatic["notes"].push_back(notes[i]);
                pending.emplace_back(key, Json{{"recording", recording.id}, {"camera", stream.id}, {"index", index}, {"width", width}, {"height", height},
                                              {"boxes", boxes}, {"reviewed", false}, {"human_touched", false}, {"automatic", automatic}});
                {
                    std::lock_guard guard(lock_);
                    state_["done"] = state_["done"].get<int>() + 1; state_["boxes"] = state_["boxes"].get<int>() + int(boxes.size());
                    state_["current"] = recording.name + " / " + stream.id + " / " + std::to_string(index);
                    state_["elapsed_seconds"] = std::round(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() * 10) / 10;
                }
                if (pending.size() >= 32) flush();
            }
            if (!pending.empty()) flush();
        };
        std::vector<std::pair<const Recording*, const CameraStream*>> tasks;
        for (const auto& recording : project_.catalog.recordings) for (const auto& stream : recording.cameras) tasks.emplace_back(&recording, &stream);
        const int workers = std::min(3, std::max(1, int(std::thread::hardware_concurrency()) / 4));
        { std::lock_guard guard(lock_); state_["workers"] = workers; }
        std::atomic<size_t> nextTask{0};
        std::mutex failureLock; std::string failure;
        std::vector<std::thread> pool;
        for (int w = 0; w < workers; ++w) pool.emplace_back([&] {
            try {
                std::unique_ptr<Segmenter> segmenter = segmenterFactory ? segmenterFactory() : std::make_unique<ModelSegmenter>(modelDir_);   // one model per thread
                for (size_t task = nextTask++; task < tasks.size() && !stop_; task = nextTask++) cameraTask(*tasks[task].first, *tasks[task].second, *segmenter);
            } catch (const std::exception& error) {
                std::lock_guard guard(failureLock); if (failure.empty()) failure = error.what(); stop_ = true;
            }
        });
        for (auto& thread : pool) thread.join();
        if (!failure.empty()) throw std::runtime_error(failure);
        std::lock_guard guard(lock_);
        state_["status"] = stop_ ? "stopped" : "complete";
        state_["elapsed_seconds"] = std::round(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() * 10) / 10;
    } catch (const std::exception& error) {
        std::lock_guard guard(lock_); state_["status"] = "error"; state_["error"] = error.what();
    }
    persist();
}
}
