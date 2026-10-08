#include "labeler_selection.hpp"
#include "labeler_assist.hpp"
#include "labeler_util.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace boccia::labeler {
namespace {
bool truthy(const Json& value) { return value.is_boolean() ? value.get<bool>() : !value.is_null() && !(value.is_number() && value.get<double>() == 0) && !(value.is_object() && value.empty()) && !(value.is_array() && value.empty()); }
double rule(const Json& rules, const char* name) { return rules.at(name).get<double>(); }
double roundTo(double value, int digits) { const double scale = std::pow(10.0, digits); return std::nearbyint(value * scale) / scale; }
// numpy's correlation of two patches after removing the mean over every value of each.
double centeredCorrelation(const cv::Mat& a, const cv::Mat& b, double mae) {
    const double meanA = cv::sum(a)[0] + cv::sum(a)[1] + cv::sum(a)[2], meanB = cv::sum(b)[0] + cv::sum(b)[1] + cv::sum(b)[2];
    const double count = double(a.total()) * a.channels();
    const cv::Mat ca = a - cv::Scalar::all(meanA / count), cb = b - cv::Scalar::all(meanB / count);
    const double denominator = std::sqrt(ca.dot(ca)) * std::sqrt(cb.dot(cb));
    return denominator > 1 ? ca.dot(cb) / denominator : (mae < 1 ? 1.0 : 0.0);
}
}
Json defaultSelectionRules() {
    // 8px of box movement is within the jitter of the automatic draft; the pixels decide whether the ball moved.
    return {{"center_pixels", 8.0}, {"size_pixels", 4.0}, {"size_fraction", .12},
            {"mask_iou", .85}, {"ball_mae", 5.0}, {"ball_correlation", .97},
            {"scene_width", 720}, {"scene_mae", 2.5}, {"scene_changed_fraction", .0015},
            {"scene_change_level", 12}, {"scene_component_pixels", 18}, {"scene_tile_mae", 4.0},
            {"periodic_seconds", 2.0}, {"periodic_frames_without_timestamps", 60}};
}
Json selectionRules(const Json& overrides) {
    Json rules = defaultSelectionRules();
    if (overrides.is_object()) for (const auto& [name, value] : overrides.items()) rules[name] = value;
    return rules;
}
std::optional<std::vector<BoxPair>> matchBoxes(const Json& a, const Json& b, const Json& rules) {
    if (a.size() != b.size()) return std::nullopt;
    struct Edge { double distance; int i, j; };
    std::vector<Edge> edges;
    for (int i = 0; i < int(a.size()); ++i) {
        const Json& old = a[size_t(i)]; const double x = old["bbox"][0], y = old["bbox"][1], w = old["bbox"][2], h = old["bbox"][3];
        for (int j = 0; j < int(b.size()); ++j) {
            const Json& fresh = b[size_t(j)]; const double u = fresh["bbox"][0], v = fresh["bbox"][1], s = fresh["bbox"][2], t = fresh["bbox"][3];
            const double distance = std::hypot(x + w / 2 - u - s / 2, y + h / 2 - v - t / 2);
            const bool sized = std::abs(w - s) <= std::max(rule(rules, "size_pixels"), w * rule(rules, "size_fraction")) && std::abs(h - t) <= std::max(rule(rules, "size_pixels"), h * rule(rules, "size_fraction"));
            if (distance <= rule(rules, "center_pixels") && old.value("occluded", false) == fresh.value("occluded", false) && sized) edges.push_back({distance, i, j});
        }
    }
    // Only a one-to-one correspondence is accepted. Ambiguous crowded balls conservatively become representatives.
    std::sort(edges.begin(), edges.end(), [](const Edge& p, const Edge& q) { return std::tie(p.distance, p.i, p.j) < std::tie(q.distance, q.i, q.j); });
    std::vector<BoxPair> pairs; std::vector<bool> usedA(a.size(), false), usedB(b.size(), false);
    for (const auto& edge : edges) if (!usedA[size_t(edge.i)] && !usedB[size_t(edge.j)]) { pairs.push_back({edge.i, edge.j, edge.distance}); usedA[size_t(edge.i)] = usedB[size_t(edge.j)] = true; }
    if (pairs.size() != a.size()) return std::nullopt;
    return pairs;
}
FrameFeatures frameFeatures(const cv::Mat& image, const Json& boxes, const Json& rules) {
    const int height = image.rows, width = image.cols;
    const double scale = std::min(1.0, rule(rules, "scene_width") / width);
    FrameFeatures result;
    cv::resize(image, result.scene, {int(std::nearbyint(width * scale)), int(std::nearbyint(height * scale))}, 0, 0, cv::INTER_AREA);
    result.image = image; result.boxes = boxes;
    // Balls are compared at the retained frame's position and masks in image coordinates, so a draft box that
    // only jittered on a still ball reads as the same ball.
    for (const auto& box : boxes) result.masks.push_back(box.contains("segmentation") ? decodeMaskChecked(box["segmentation"], width, height) : cv::Mat());
    return result;
}
cv::Mat ballPatch(const cv::Mat& image, const Json& box) {
    // Ball with context around the box center, blurred and resized so texture and occlusion differences remain.
    const double x = box["bbox"][0], y = box["bbox"][1], w = box["bbox"][2], h = box["bbox"][3];
    const int side = std::max(16, int(std::ceil(std::max(w, h) * 2)));
    cv::Mat patch, blurred, small, result;
    cv::getRectSubPix(image, {side, side}, {float(x + w / 2), float(y + h / 2)}, patch);
    cv::GaussianBlur(patch, blurred, {3, 3}, 0);
    cv::resize(blurred, small, {48, 48}, 0, 0, cv::INTER_AREA);
    small.convertTo(result, CV_32F);
    return result;
}
FrameComparison compareFrames(const FrameFeatures& a, const FrameFeatures& b, const std::vector<BoxPair>& pairs, const Json& rules) {
    if (a.scene.size() != b.scene.size() || a.scene.channels() != b.scene.channels()) return {false, "image_dimensions", Json::object()};
    cv::Mat absolute; cv::absdiff(a.scene, b.scene, absolute);
    cv::Mat difference(absolute.rows, absolute.cols, CV_8UC1);
    for (int y = 0; y < absolute.rows; ++y) for (int x = 0; x < absolute.cols; ++x) { const auto pixel = absolute.at<cv::Vec3b>(y, x); difference.at<uchar>(y, x) = std::max({pixel[0], pixel[1], pixel[2]}); }
    const cv::Mat changed = difference > rule(rules, "scene_change_level");
    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(changed, labels, stats, centroids, 8, CV_32S);
    int component = 0;
    for (int label = 1; label < count; ++label) component = std::max(component, stats.at<int>(label, cv::CC_STAT_AREA));
    double tileMae = 0;
    for (int y = 0; y < difference.rows; y += 30) for (int x = 0; x < difference.cols; x += 30)
        tileMae = std::max(tileMae, cv::mean(difference(cv::Rect(x, y, std::min(30, difference.cols - x), std::min(30, difference.rows - y))))[0]);
    double centerMax = 0;
    for (const auto& pair : pairs) centerMax = std::max(centerMax, pair.distance);
    FrameComparison result;
    result.metrics = {{"center_max", roundTo(centerMax, 3)}, {"scene_mae", roundTo(cv::mean(difference)[0], 4)},
                      {"scene_changed_fraction", roundTo(cv::countNonZero(changed) / double(changed.total()), 6)},
                      {"scene_component", component}, {"scene_tile_mae", roundTo(tileMae, 4)}};
    if (result.metrics["scene_mae"].get<double>() > rule(rules, "scene_mae") || result.metrics["scene_changed_fraction"].get<double>() > rule(rules, "scene_changed_fraction")
        || component > rule(rules, "scene_component_pixels") || tileMae > rule(rules, "scene_tile_mae")) { result.reason = "scene_change"; return result; }
    double maxMae = 0, minCorrelation = 1, minIou = 1;
    for (const auto& pair : pairs) {
        const cv::Mat old = ballPatch(a.image, a.boxes[size_t(pair.i)]), fresh = ballPatch(b.image, a.boxes[size_t(pair.i)]);   // the same spot in both frames
        cv::Mat delta; cv::absdiff(old, fresh, delta);
        const double mae = (cv::sum(delta)[0] + cv::sum(delta)[1] + cv::sum(delta)[2]) / (double(delta.total()) * delta.channels());
        maxMae = std::max(maxMae, mae); minCorrelation = std::min(minCorrelation, centeredCorrelation(old, fresh, mae));
        const cv::Mat& oldMask = a.masks[size_t(pair.i)]; const cv::Mat& newMask = b.masks[size_t(pair.j)];
        if (oldMask.empty() != newMask.empty()) { result.reason = "mask_change"; return result; }
        if (!oldMask.empty()) {
            cv::Mat both, either; cv::bitwise_and(oldMask, newMask, both); cv::bitwise_or(oldMask, newMask, either);
            minIou = std::min(minIou, cv::countNonZero(both) / double(std::max(1, cv::countNonZero(either))));
        }
    }
    result.metrics["ball_mae"] = roundTo(maxMae, 4); result.metrics["ball_correlation"] = roundTo(minCorrelation, 5); result.metrics["mask_iou"] = roundTo(minIou, 5);
    if (maxMae > rule(rules, "ball_mae") || minCorrelation < rule(rules, "ball_correlation")) { result.reason = "ball_appearance"; return result; }
    if (minIou < rule(rules, "mask_iou")) { result.reason = "mask_change"; return result; }
    result.duplicate = true; result.reason = "near_duplicate";
    return result;
}
Json buildSelection(const Catalog& catalog, const Json& snapshot, const SelectionProgress& progress, const Json& ruleOverrides) {
    const Json rules = selectionRules(ruleOverrides);
    if (snapshot.value("source_signature", "") != catalog.signature) throw std::runtime_error("라벨 프로젝트와 원본 촬영이 다릅니다.");
    const int total = catalog.totalFrames();
    Json decisions = Json::object(), counts = Json::array();
    int done = 0, selectedTotal = 0;
    const Json& frames = snapshot["frames"];
    for (const auto& recording : catalog.recordings) {
        for (const auto& stream : recording.cameras) {
            struct Anchor { std::string key; int index; Json item; };
            std::optional<Anchor> anchor; std::optional<FrameFeatures> anchorFeatures;
            int kept = 0;
            for (int index = 0; index < int(stream.frames.size()); ++index) {
                const std::string key = recording.id + "/" + stream.id + "/" + std::to_string(index);
                const Json item = frames.contains(key) ? frames[key] : Json();
                std::string reason = "first_frame"; Json metrics = Json::object(); bool selected = true;
                std::optional<FrameFeatures> imageFeatures;
                if (item.is_null()) reason = "unlabeled";
                else if (truthy(item.value("human_touched", Json(false))) || truthy(item.value("reviewed", Json(false)))) reason = "human_preserved";
                else if (index == int(stream.frames.size()) - 1) reason = "last_frame";
                else if (item.contains("automatic") && item["automatic"].is_object() && truthy(item["automatic"].value("notes", Json()))) reason = "uncertain_annotation";
                else if (anchor) {
                    const auto& info = stream.frames[size_t(index)]; const auto& oldInfo = stream.frames[size_t(anchor->index)];
                    const bool periodic = info.hasHostNs && oldInfo.hasHostNs ? double(info.hostNs - oldInfo.hostNs) >= rule(rules, "periodic_seconds") * 1e9
                                                                               : index - anchor->index >= rule(rules, "periodic_frames_without_timestamps");
                    const auto pairs = matchBoxes(anchor->item["boxes"], item["boxes"], rules);
                    if (periodic) reason = "periodic_static_sample";
                    else if (!pairs) reason = "ball_geometry";
                    else {
                        imageFeatures = frameFeatures(catalog.read(recording.id, stream.id, index), item["boxes"], rules);
                        if (!anchorFeatures) anchorFeatures = frameFeatures(catalog.read(recording.id, stream.id, anchor->index), anchor->item["boxes"], rules);
                        const auto comparison = compareFrames(*anchorFeatures, *imageFeatures, *pairs, rules);
                        reason = comparison.reason; metrics = comparison.metrics; selected = !comparison.duplicate;
                    }
                }
                Json decision = {{"selected", selected}, {"representative", selected ? key : anchor->key}, {"reason", reason}};
                if (!metrics.empty()) decision["metrics"] = metrics;
                decisions[key] = std::move(decision);
                if (selected) {
                    ++kept;
                    if (item.is_null()) anchor.reset(); else anchor = Anchor{key, index, item};
                    anchorFeatures = imageFeatures;
                }
                ++done;
                if (done % 200 == 0 && progress) progress(done, total, recording.name + "/" + stream.id);
            }
            counts.push_back({{"recording", recording.id}, {"name", recording.name}, {"camera", stream.id}, {"total", stream.frames.size()}, {"selected", kept}, {"excluded", int(stream.frames.size()) - kept}});
            selectedTotal += kept;
            if (progress) progress(done, total, recording.name + "/" + stream.id);
        }
    }
    return {{"version", 1}, {"algorithm", "within-camera-geometry-mask-appearance-v2"}, {"created_at", utcIso()}, {"source_signature", catalog.signature},
            {"project_revision", snapshot.value("revision", 0)}, {"rules", rules}, {"total", total}, {"selected", selectedTotal}, {"excluded", total - selectedTotal},
            {"cameras", counts}, {"frames", decisions}};
}
}
