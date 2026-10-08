#include "labeler_propagate.hpp"
#include "labeler_assist.hpp"
#include "labeler_util.hpp"
#include <opencv2/imgproc.hpp>
#include <atomic>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <mutex>
#include <thread>

namespace boccia::labeler {
namespace {
bool truthy(const Json& value) { return value.is_boolean() ? value.get<bool>() : !value.is_null() && !(value.is_number() && value.get<double>() == 0); }
bool humanWork(const Json& frame) { return truthy(frame.value("human_touched", Json(false))) || truthy(frame.value("reviewed", Json(false))); }
const Json& shownBox(const Json& box) { return box.contains("detection_bbox") ? box["detection_bbox"] : box["bbox"]; }
std::string sourceHash(const Json& boxes) { return sha256Hex(boxes.dump()); }
struct Target { std::string key, representative, hash; Json frame, confirmed; bool carry = false; };
// A draft agrees with the confirmed frame when the counts match and every confirmed ball has a draft within the matching rule.
bool consistent(const Json& confirmed, const Json& drafts) {
    if (confirmed.size() != drafts.size()) return false;
    std::vector<bool> used(drafts.size(), false);
    for (const auto& r : confirmed) {
        const Json& rb = shownBox(r);
        const double rx = rb[0].get<double>() + rb[2].get<double>() / 2, ry = rb[1].get<double>() + rb[3].get<double>() / 2;
        bool found = false;
        for (size_t i = 0; i < drafts.size() && !found; ++i) {
            if (used[i]) continue;
            const Json& db = shownBox(drafts[i]);
            const double dx = db[0].get<double>() + db[2].get<double>() / 2, dy = db[1].get<double>() + db[3].get<double>() / 2;
            const bool sized = std::abs(rb[2].get<double>() - db[2].get<double>()) <= std::max(4.0, .12 * rb[2].get<double>()) && std::abs(rb[3].get<double>() - db[3].get<double>()) <= std::max(4.0, .12 * rb[3].get<double>());
            if (std::hypot(rx - dx, ry - dy) <= 8 && sized) { used[i] = true; found = true; }
        }
        if (!found) return false;
    }
    return true;
}
struct Disc { double x, y, r; };
Disc discOf(const Json& box) {
    if (box.contains("circle")) return {box["circle"][0].get<double>(), box["circle"][1].get<double>(), box["circle"][2].get<double>()};
    const Json& b = shownBox(box);
    return {b[0].get<double>() + b[2].get<double>() / 2, b[1].get<double>() + b[3].get<double>() / 2, std::min(b[2].get<double>(), b[3].get<double>()) / 2};
}
// The mean colour inside a ball's disc: where a ball moved away the floor remains, which looks nothing like the ball.
cv::Scalar discMean(const cv::Mat& image, const Json& box) {
    const Disc d = discOf(box);
    cv::Mat mask = cv::Mat::zeros(image.size(), CV_8UC1);
    cv::circle(mask, cv::Point(int(std::lround(d.x)), int(std::lround(d.y))), std::max(1, int(d.r * .7)), cv::Scalar(255), -1);
    cv::Scalar mean = cv::mean(image, mask);
    if (image.channels() == 1) mean[1] = mean[2] = mean[0];
    return mean;
}
bool sameLook(const cv::Scalar& a, const cv::Scalar& b) { return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) <= 96; }
}

Json carryLabels(const Json& confirmed, const Json& drafts, const cv::Mat& sourceImage, const cv::Mat& image, double margin, bool verify, int& matched, int& copied) {
    std::vector<bool> used(drafts.size(), false); Json out = Json::array();
    std::vector<cv::Scalar> looks;
    if (verify) for (const auto& r : confirmed) looks.push_back(discMean(sourceImage, r));
    auto looksLikeAny = [&](const cv::Scalar& look) { for (const auto& l : looks) if (sameLook(look, l)) return true; return false; };
    auto fits = [&](Json& box) { return box.contains("segmentation") && !box.value("occluded", false) && refineBallBox(box, image, margin); };
    for (size_t k = 0; k < confirmed.size(); ++k) {
        const Json& r = confirmed[k];
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
        if (best >= 0) {
            used[size_t(best)] = true; Json box = drafts[size_t(best)];
            box["occluded"] = r.value("occluded", false);
            if (r.contains("color")) box["color"] = r["color"]; else box.erase("color");
            const bool fitted = fits(box);
            if (verify && !fitted && !sameLook(discMean(image, box), looks[k])) continue;   // nothing there: a stale copy on bare floor
            ++matched; out.push_back(box);
        } else {
            Json box = r;
            if (verify) {
                if (!fits(box) || !sameLook(discMean(image, box), looks[k])) continue;   // the ball is not there any more
                box["source"] = "tracking";   // carried from the confirmed frame, not found on this one
            } else fits(box);
            ++copied; out.push_back(box);
        }
    }
    if (verify) for (size_t i = 0; i < drafts.size(); ++i) {   // a ball the draft found elsewhere: a moved ball stays, a shadow goes
        if (used[i]) continue;
        Json box = drafts[i];
        const bool fitted = fits(box);
        if ((fitted || box.contains("circle")) && looksLikeAny(discMean(image, box))) out.push_back(box);
    }
    return out;
}
namespace {
// The repeated frames whose confirmed representative they do not yet carry (or all of them, on request), and with
// carry the review targets after a confirmed frame whose draft holds fewer balls than it.
std::vector<Target> plan(const Project& project, bool force, bool carry, const std::set<std::string>& only, int& representatives, int& duplicates, int& pending, int& done, int& carries) {
    std::vector<Target> targets; representatives = duplicates = pending = done = carries = 0;
    const Json view = project.selection.view(project.state["frames"]);
    const Json& decisions = view["frames"]; const Json& frames = project.state["frames"];
    if (carry) {
        // Per camera stream, in frame order: a confirmed (or propagated) frame becomes the source for the untouched targets
        // that follow it within 1.5 s (30 frames without timestamps) and hold fewer balls than it, until the next source.
        for (const auto& recording : project.catalog.recordings) for (const auto& stream : recording.cameras) {
            std::string sourceKey; const Json* source = nullptr; int64_t sourceNs = 0; int sourceIndex = -1;
            for (int index = 0; index < int(stream.frames.size()); ++index) {
                const std::string key = recording.id + "/" + stream.id + "/" + std::to_string(index);
                const auto found = frames.find(key);
                if (found == frames.end()) continue;
                const Json& frame = *found;
                const FrameInfo& info = stream.frames[size_t(index)];
                if (truthy(frame.value("reviewed", Json(false))) || truthy(frame.value("propagated", Json(false)))) {
                    if (frame.value("review_status", "") == "hold" || (!only.empty() && !only.count(key))) { source = nullptr; continue; }   // a confirmed frame ends the window; only the listed ones open one
                    sourceKey = key; source = &frame; sourceNs = info.hasHostNs ? info.hostNs : 0; sourceIndex = index; continue;
                }
                if (!source || humanWork(frame) || source->value("boxes", Json::array()).empty()) continue;
                const bool near = info.hasHostNs && sourceNs ? info.hostNs - sourceNs <= 1500000000LL : index - sourceIndex <= 30;
                if (!near) { source = nullptr; continue; }
                if (decisions.contains(key) && !decisions[key].value("selected", true)) continue;   // a repeat: the propagation proper handles it
                // A draft that the stereo matching seeded is trusted unless a ball is missing: a ball that moved since the
                // confirmed frame must keep its new place. A draft without seeds follows the confirmed frame whenever it differs.
                const int seeds = frame.contains("automatic") && frame["automatic"].is_object() ? frame["automatic"].value("seed_count", 0) : 0;
                const Json carried = frame.contains("automatic") && frame["automatic"].is_object() ? frame["automatic"].value("carried", Json()) : Json();
                const bool oldRule = carried.is_object() && carried.value("rule", 1) < 2;   // carried before the presence check: looked at again
                if (!oldRule) {
                    if (seeds > 0 && frame["boxes"].size() >= (*source)["boxes"].size()) continue;
                    if (consistent((*source)["boxes"], frame["boxes"])) continue;   // the draft already agrees with the confirmed frame
                }
                const std::string hash = sourceHash((*source)["boxes"]);
                if (carried.is_object() && !oldRule && carried.value("from", "") == sourceKey && carried.value("source_hash", "") == hash && !force) continue;
                ++carries;
                targets.push_back({key, sourceKey, hash, frame, (*source)["boxes"], true});
            }
        }
    }
    std::set<std::string> confirmedRepresentatives;
    for (const auto& [key, decision] : decisions.items()) {
        if (decision.value("selected", true)) continue;
        const std::string representative = decision.value("representative", key);
        if (representative == key || !frames.contains(key) || !frames.contains(representative)) continue;
        if (!only.empty() && !only.count(representative)) continue;
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
    int representatives, duplicates, pending, done, carries;
    plan(project, false, true, {}, representatives, duplicates, pending, done, carries);
    return {{"representatives", representatives}, {"duplicates", duplicates}, {"pending", pending}, {"done", done}, {"carry", carries}};
}
Json startPropagation(Project& project, bool force, bool carry, const Json& sources) {
    std::lock_guard guard(project.lock);
    if (project.propagationJob.value("status", "") == "running") throw std::runtime_error("이미 라벨을 전파하는 중입니다.");
    std::set<std::string> only;
    if (sources.is_array()) for (const auto& key : sources) if (key.is_string()) only.insert(key.get<std::string>());
    int representatives, duplicates, pending, done, carries;
    std::vector<Target> targets = plan(project, force, carry, only, representatives, duplicates, pending, done, carries);
    const double margin = project.state.value("circle_margin", defaultCircleMargin);
    project.propagationJob = {{"status", "running"}, {"run_id", localStamp() + "-" + randomHex(4)}, {"total", targets.size()}, {"done", 0},
                              {"frames", 0}, {"balls", 0}, {"matched", 0}, {"copied", 0}, {"skipped", 0}, {"carried", 0}, {"force", force}, {"carry", carry}, {"live", !only.empty()}, {"started_at", utcIso()}};
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
                    std::map<std::string, cv::Mat> sourceImages;   // the confirmed frames' images, for the look of their balls
                    for (size_t i = next++; i < targets.size() && !stop.stop_requested(); i = next++) {
                        const Target& target = targets[i];
                        const cv::Mat image = project.catalog.read(target.frame["recording"], target.frame["camera"], target.frame["index"]);
                        cv::Mat sourceImage;
                        if (target.carry) {
                            auto found = sourceImages.find(target.representative);
                            if (found == sourceImages.end()) {
                                const std::string& key = target.representative; const auto a = key.find('/'), b = key.rfind('/');
                                if (sourceImages.size() > 8) sourceImages.clear();
                                found = sourceImages.emplace(key, project.catalog.read(key.substr(0, a), key.substr(a + 1, b - a - 1), std::stoi(key.substr(b + 1)))).first;
                            }
                            sourceImage = found->second;
                        }
                        int matched = 0, copied = 0;
                        Json boxes = carryLabels(target.confirmed, target.frame["boxes"], sourceImage, image, margin, target.carry, matched, copied);
                        matchedAll += matched; copiedAll += copied;
                        { std::lock_guard guard(resultsLock); results[target.key] = {{"boxes", boxes}, {"was", target.frame["boxes"]}, {"from", target.representative}, {"hash", target.hash}, {"width", image.cols}, {"height", image.rows}, {"carry", target.carry}}; }
                        { std::lock_guard guard(project.lock); project.propagationJob["done"] = ++finished; }
                    }
                } catch (const std::exception& error) { std::lock_guard guard(failureLock); if (failure.empty()) failure = error.what(); }
            });
            for (auto& thread : pool) thread.join();
            if (!failure.empty()) throw std::runtime_error(failure);
            std::lock_guard guard(project.lock);
            if (stop.stop_requested()) { project.propagationJob["status"] = "stopped"; return; }
            const std::string runId = project.propagationJob.value("run_id", "");
            int applied = 0, balls = 0, skipped = 0, carried = 0;
            project.mutateAutomatic([&](Json& state) {
                for (const auto& [key, result] : results) {
                    if (!state["frames"].contains(key)) continue;
                    Json& frame = state["frames"][key];
                    if (humanWork(frame) || frame["boxes"] != result["was"]) { ++skipped; continue; }   // a person got there, or the draft moved on
                    frame["boxes"] = validateBoxes(result["boxes"], result["width"], result["height"]);
                    if (result.value("carry", false)) {   // a draft for a review target: still a person's to confirm
                        if (!frame["automatic"].is_object()) frame["automatic"] = Json::object();
                        frame["automatic"]["carried"] = {{"from", result["from"]}, {"source_hash", result["hash"]}, {"run_id", runId}, {"at", utcIso()}, {"rule", 2}};
                        frame["automatic"]["needs_attention"] = false;
                        ++carried; balls += int(frame["boxes"].size()); continue;
                    }
                    frame["reviewed"] = false; frame["review_status"] = "propagated"; frame["propagated"] = true;
                    frame["propagation"] = {{"from", result["from"]}, {"source_hash", result["hash"]}, {"run_id", runId}, {"at", utcIso()}};
                    ++applied; balls += int(frame["boxes"].size());
                }
                if (applied > 0 || carried > 0 || !state.contains("propagation")) state["propagation"] = {{"at", utcIso()}, {"frames", applied}, {"carried", carried}, {"balls", balls}, {"run_id", runId}};
            });
            project.propagationJob["frames"] = applied; project.propagationJob["carried"] = carried; project.propagationJob["balls"] = balls; project.propagationJob["skipped"] = skipped;
            project.propagationJob["matched"] = matchedAll.load(); project.propagationJob["copied"] = copiedAll.load();
            project.propagationJob["status"] = "complete"; project.propagationJob["finished_at"] = utcIso();
        } catch (const std::exception& error) {
            std::lock_guard guard(project.lock); project.propagationJob["status"] = "error"; project.propagationJob["error"] = error.what();
        }
    });
    return project.propagationJob;
}
}
