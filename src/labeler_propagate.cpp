#include "labeler_propagate.hpp"
#include "labeler_assist.hpp"
#include "labeler_util.hpp"
#include <opencv2/imgproc.hpp>
#include <atomic>
#include <cmath>
#include <map>
#include <set>
#include <mutex>
#include <thread>

namespace boccia::labeler {
namespace {
bool truthy(const Json& value) { return value.is_boolean() ? value.get<bool>() : !value.is_null() && !(value.is_number() && value.get<double>() == 0); }
bool humanWork(const Json& frame) { return truthy(frame.value("human_touched", Json(false))) || truthy(frame.value("reviewed", Json(false))); }
const Json& shownBox(const Json& box) { return box.contains("detection_bbox") ? box["detection_bbox"] : box["bbox"]; }
std::string sourceHash(const Json& boxes) { return sha256Hex(boxes.dump()); }
struct Target { std::string key, representative, hash; Json frame, confirmed; };
// The confirmed balls carried onto the repeated frame: its own draft ball where one matches (within 8 px and 12 % in
// size, the repeated-scene filter's own rule), the confirmed ball copied where none does; both measured on this image.
Json transfer(const Json& confirmed, const Json& drafts, const cv::Mat& image, double margin, int& matched, int& copied) {
    std::vector<bool> used(drafts.size(), false); Json out = Json::array();
    for (const auto& r : confirmed) {
        const Json& rb = shownBox(r);
        const double rx = rb[0].get<double>() + rb[2].get<double>() / 2, ry = rb[1].get<double>() + rb[3].get<double>() / 2;
        const bool manual = r.value("source", "").rfind("manual", 0) == 0;   // an outline a person drew replaces the draft's
        int best = -1; double bestDistance = 0;
        if (!manual) for (size_t i = 0; i < drafts.size(); ++i) {
            if (used[i]) continue;
            const Json& db = shownBox(drafts[i]);
            const double dx = db[0].get<double>() + db[2].get<double>() / 2, dy = db[1].get<double>() + db[3].get<double>() / 2;
            const double distance = std::hypot(rx - dx, ry - dy);
            const bool sized = std::abs(rb[2].get<double>() - db[2].get<double>()) <= std::max(4.0, .12 * rb[2].get<double>()) && std::abs(rb[3].get<double>() - db[3].get<double>()) <= std::max(4.0, .12 * rb[3].get<double>());
            if (distance <= 8 && sized && (best < 0 || distance < bestDistance)) { best = int(i); bestDistance = distance; }
        }
        Json box;
        if (best >= 0) {
            used[size_t(best)] = true; box = drafts[size_t(best)];
            box["occluded"] = r.value("occluded", false);
            if (r.contains("color")) box["color"] = r["color"]; else box.erase("color");
            ++matched;
        } else { box = r; ++copied; }
        if (box.contains("segmentation") && !box.value("occluded", false)) refineBallBox(box, image, margin);   // this frame's own edge
        out.push_back(box);
    }
    return out;
}
// The repeated frames whose confirmed representative they do not yet carry (or all of them, on request).
std::vector<Target> plan(const Project& project, bool force, int& representatives, int& duplicates, int& pending, int& done) {
    std::vector<Target> targets; representatives = duplicates = pending = done = 0;
    const Json view = project.selection.view(project.state["frames"]);
    const Json& decisions = view["frames"]; const Json& frames = project.state["frames"];
    std::set<std::string> confirmedRepresentatives;
    for (const auto& [key, decision] : decisions.items()) {
        if (decision.value("selected", true)) continue;
        const std::string representative = decision.value("representative", key);
        if (representative == key || !frames.contains(key) || !frames.contains(representative)) continue;
        const Json& source = frames[representative];
        if (!truthy(source.value("reviewed", Json(false))) || source.value("review_status", "") != "reviewed") continue;
        const Json& frame = frames[key];
        if (humanWork(frame)) continue;
        ++duplicates; confirmedRepresentatives.insert(representative);
        const std::string hash = sourceHash(source["boxes"]);
        const bool current = frame.contains("propagation") && frame["propagation"].value("source_hash", "") == hash;
        if (current) ++done; else ++pending;
        if (current && !force) continue;
        targets.push_back({key, representative, hash, frame, source["boxes"]});
    }
    representatives = int(confirmedRepresentatives.size());
    return targets;
}
}

Json propagationStatus(const Project& project) { std::lock_guard guard(project.lock); return project.propagationJob; }
Json propagationCounts(const Project& project) {
    std::lock_guard guard(project.lock);
    int representatives, duplicates, pending, done;
    plan(project, false, representatives, duplicates, pending, done);
    return {{"representatives", representatives}, {"duplicates", duplicates}, {"pending", pending}, {"done", done}};
}
Json startPropagation(Project& project, bool force) {
    std::lock_guard guard(project.lock);
    if (project.propagationJob.value("status", "") == "running") throw std::runtime_error("이미 라벨을 전파하는 중입니다.");
    int representatives, duplicates, pending, done;
    std::vector<Target> targets = plan(project, force, representatives, duplicates, pending, done);
    const double margin = project.state.value("circle_margin", defaultCircleMargin);
    project.propagationJob = {{"status", "running"}, {"run_id", localStamp() + "-" + randomHex(4)}, {"total", targets.size()}, {"done", 0},
                              {"frames", 0}, {"balls", 0}, {"matched", 0}, {"copied", 0}, {"skipped", 0}, {"force", force}, {"started_at", utcIso()}};
    if (project.propagationWorker.joinable()) project.propagationWorker.join();
    project.propagationWorker = std::jthread([&project, targets = std::move(targets), margin](std::stop_token stop) {
        try {
            std::map<std::string, Json> results; std::mutex resultsLock;
            std::atomic<size_t> next{0}; std::atomic<int> finished{0}, matchedAll{0}, copiedAll{0};
            std::mutex failureLock; std::string failure;
            const int workers = std::max(1, std::min(4, int(std::thread::hardware_concurrency()) / 2));
            std::vector<std::thread> pool;
            for (int w = 0; w < workers; ++w) pool.emplace_back([&] {
                try {
                    for (size_t i = next++; i < targets.size() && !stop.stop_requested(); i = next++) {
                        const Target& target = targets[i];
                        const cv::Mat image = project.catalog.read(target.frame["recording"], target.frame["camera"], target.frame["index"]);
                        int matched = 0, copied = 0;
                        Json boxes = transfer(target.confirmed, target.frame["boxes"], image, margin, matched, copied);
                        matchedAll += matched; copiedAll += copied;
                        { std::lock_guard guard(resultsLock); results[target.key] = {{"boxes", boxes}, {"was", target.frame["boxes"]}, {"from", target.representative}, {"hash", target.hash}, {"width", image.cols}, {"height", image.rows}}; }
                        { std::lock_guard guard(project.lock); project.propagationJob["done"] = ++finished; }
                    }
                } catch (const std::exception& error) { std::lock_guard guard(failureLock); if (failure.empty()) failure = error.what(); }
            });
            for (auto& thread : pool) thread.join();
            if (!failure.empty()) throw std::runtime_error(failure);
            std::lock_guard guard(project.lock);
            if (stop.stop_requested()) { project.propagationJob["status"] = "stopped"; return; }
            const std::string runId = project.propagationJob.value("run_id", "");
            int applied = 0, balls = 0, skipped = 0;
            project.mutateAutomatic([&](Json& state) {
                for (const auto& [key, result] : results) {
                    if (!state["frames"].contains(key)) continue;
                    Json& frame = state["frames"][key];
                    if (humanWork(frame) || frame["boxes"] != result["was"]) { ++skipped; continue; }   // a person got there, or the draft moved on
                    frame["boxes"] = validateBoxes(result["boxes"], result["width"], result["height"]);
                    frame["reviewed"] = false; frame["review_status"] = "propagated"; frame["propagated"] = true;
                    frame["propagation"] = {{"from", result["from"]}, {"source_hash", result["hash"]}, {"run_id", runId}, {"at", utcIso()}};
                    ++applied; balls += int(frame["boxes"].size());
                }
                if (applied > 0 || !state.contains("propagation")) state["propagation"] = {{"at", utcIso()}, {"frames", applied}, {"balls", balls}, {"run_id", runId}};
            });
            project.propagationJob["frames"] = applied; project.propagationJob["balls"] = balls; project.propagationJob["skipped"] = skipped;
            project.propagationJob["matched"] = matchedAll.load(); project.propagationJob["copied"] = copiedAll.load();
            project.propagationJob["status"] = "complete"; project.propagationJob["finished_at"] = utcIso();
        } catch (const std::exception& error) {
            std::lock_guard guard(project.lock); project.propagationJob["status"] = "error"; project.propagationJob["error"] = error.what();
        }
    });
    return project.propagationJob;
}
}
