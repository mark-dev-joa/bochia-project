#include "native_labeler_view.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <cstdio>

namespace boccia::labeler {
std::array<Span, 3> tileSpans(int focus, float width, float gap, float narrow) {
    std::array<Span, 3> spans;
    if (focus < 0 || focus > 2) {
        const float each = (width - 2 * gap) / 3;
        for (int i = 0; i < 3; ++i) spans[i] = {i * (each + gap), each};
        return spans;
    }
    narrow = std::min(narrow, (width - 2 * gap) / 4);
    const float wide = width - 2 * gap - 2 * narrow;
    float x = 0;
    for (int i = 0; i < 3; ++i) { spans[i] = {x, i == focus ? wide : narrow}; x += spans[i].width + gap; }
    return spans;
}
View fitView(int imageWidth, int imageHeight, float viewWidth, float viewHeight) {
    const float scale = std::min(viewWidth / imageWidth, viewHeight / imageHeight);
    return {scale, (viewWidth - imageWidth * scale) / 2, (viewHeight - imageHeight * scale) / 2};
}
View ballView(const cv::Rect2d& box, int imageWidth, int imageHeight, float viewWidth, float viewHeight) {
    const float fit = fitView(imageWidth, imageHeight, viewWidth, viewHeight).scale;
    const float scale = std::clamp(float(viewHeight * .34 / std::max(box.width, box.height)), fit, 14.f);
    return {scale, float(viewWidth / 2 - (box.x + box.width / 2) * scale), float(viewHeight / 2 - (box.y + box.height / 2) * scale)};
}
cv::Rect2d cropRegion(const cv::Rect2d& box, int imageWidth, int imageHeight, double factor) {
    const double side = std::min({std::max(box.width, box.height) * factor, double(imageWidth), double(imageHeight)});
    const double x = std::clamp(box.x + box.width / 2 - side / 2, 0., imageWidth - side);
    const double y = std::clamp(box.y + box.height / 2 - side / 2, 0., imageHeight - side);
    return {x, y, side, side};
}
MemberState memberState(const Json& member) {
    if (!member.value("selected", true)) return MemberState::Duplicate;
    const auto status = member.value("status", "draft");
    return status == "reviewed" ? MemberState::Done : status == "hold" ? MemberState::Hold : status == "editing" ? MemberState::Editing : MemberState::Todo;
}
bool countsDiffer(const Json& scene) {
    int first = -1;
    for (const auto& member : scene.at("members")) {
        if (!member.value("selected", true)) continue;
        const int count = member.value("count", 0);
        if (first < 0) first = count; else if (count != first) return true;
    }
    return false;
}
std::vector<BallRef> ballOrder(const std::vector<Json>& annotations) {
    std::vector<BallRef> order;
    for (int camera = 0; camera < int(annotations.size()); ++camera) {
        const auto& boxes = annotations[camera].at("boxes");
        std::vector<int> indexes(boxes.size());
        std::iota(indexes.begin(), indexes.end(), 0);
        std::stable_sort(indexes.begin(), indexes.end(), [&](int a, int b) { return detectionBox(boxes[a]).x < detectionBox(boxes[b]).x; });
        for (int index : indexes) order.push_back({camera, index});
    }
    return order;
}
BallRef nextBall(const std::vector<BallRef>& order, BallRef current, int step) {
    if (order.empty()) return {};
    const auto found = std::find_if(order.begin(), order.end(), [&](BallRef ball) { return ball.camera == current.camera && ball.index == current.index; });
    const int size = int(order.size()), at = found == order.end() ? (step > 0 ? -1 : 0) : int(found - order.begin());
    return order[((at + step) % size + size) % size];
}
int replaceTarget(const std::vector<cv::Rect2d>& boxes, cv::Point2d point, int selected) {
    if (selected >= 0 && selected < int(boxes.size()) && boxes[selected].contains(point)) return selected;
    for (int i = int(boxes.size()) - 1; i >= 0; --i) if (boxes[i].contains(point)) return i;
    return -1;
}
cv::Point2d boundsCenter(const std::vector<cv::Point2d>& points) {
    if (points.empty()) return {};
    auto low = points.front(), high = low;
    for (const auto& point : points) { low.x = std::min(low.x, point.x); low.y = std::min(low.y, point.y); high.x = std::max(high.x, point.x); high.y = std::max(high.y, point.y); }
    return (low + high) * .5;
}
Circle fitCircle(const std::vector<cv::Point>& points) {
    if (points.size() < 3) return {};
    // Algebraic fit of x^2 + y^2 + a x + b y + c = 0 around the centroid, so the normal equations stay well conditioned.
    double mx = 0, my = 0;
    for (const auto& p : points) { mx += p.x; my += p.y; }
    mx /= double(points.size()); my /= double(points.size());
    double suu = 0, suv = 0, svv = 0, suuu = 0, svvv = 0, suvv = 0, svuu = 0;
    for (const auto& p : points) {
        const double u = p.x - mx, v = p.y - my;
        suu += u * u; suv += u * v; svv += v * v; suuu += u * u * u; svvv += v * v * v; suvv += u * v * v; svuu += v * u * u;
    }
    const double det = suu * svv - suv * suv;
    if (std::abs(det) < 1e-9) return {};
    const double right1 = (suuu + suvv) / 2, right2 = (svvv + svuu) / 2;
    const double uc = (right1 * svv - right2 * suv) / det, vc = (right2 * suu - right1 * suv) / det;
    return {uc + mx, vc + my, std::sqrt(uc * uc + vc * vc + (suu + svv) / double(points.size()))};
}
std::map<std::string, SplitTotal> splitTotals(const Json& stats, const Json& splits) {
    std::map<std::string, SplitTotal> totals{{"train", {}}, {"val", {}}, {"test", {}}};
    for (const auto& [recording, item] : stats.items()) {
        const auto found = totals.find(splits.value(recording, "unassigned"));
        if (found == totals.end()) continue;
        auto& total = found->second;
        ++total.recordings; total.images += item.value("images", 0); total.balls += item.value("balls", 0); total.empty += item.value("empty", 0);
    }
    return totals;
}
std::vector<SplitCheck> splitChecks(const Json& stats, const Json& splits, const Json& groups) {
    static const std::pair<const char*, const char*> names[] = {{"train", "학습"}, {"val", "검증"}, {"test", "평가"}};
    auto totals = splitTotals(stats, splits);
    auto join = [](const std::vector<std::string>& parts) { std::string text; for (const auto& part : parts) text += (text.empty() ? "" : ", ") + part; return text; };
    std::vector<SplitCheck> checks;
    std::map<std::string, std::string> owner; bool crossing = false;
    for (const auto& [recording, group] : groups.items()) {
        const auto split = splits.value(recording, "unassigned");
        const auto [found, added] = owner.emplace(group.get<std::string>(), split);
        crossing |= !added && found->second != split;
    }
    checks.push_back({!crossing, crossing ? "같은 묶음의 촬영이 서로 다른 쪽에 들어 있습니다" : "같은 촬영과 같은 묶음이 두 쪽에 걸치지 않습니다", ""});
    std::vector<std::string> missing, noEmpty, single;
    for (const auto& [key, label] : names) {
        const auto& total = totals[key];
        if (!total.images) { missing.push_back(label); continue; }
        if (!total.empty) noEmpty.push_back(label);
        if (total.recordings < 2) single.push_back(label);
    }
    checks.push_back({missing.empty(), missing.empty() ? "학습·검증·평가에 모두 이미지가 있습니다" : join(missing) + "에 이미지가 없습니다", ""});
    std::vector<std::string> withEmpty; std::set<std::string> days;
    for (const auto& [recording, item] : stats.items()) {
        if (item.value("empty", 0)) { char number[8]; std::snprintf(number, sizeof number, "%02d", item.value("number", 0)); withEmpty.push_back(number); }
        if (item.contains("day") && item["day"].is_string()) days.insert(item["day"].get<std::string>());
    }
    std::sort(withEmpty.begin(), withEmpty.end());
    checks.push_back({noEmpty.empty(), noEmpty.empty() ? "세 쪽 모두 공 없는 이미지가 들어 있습니다" : join(noEmpty) + "에 공 없는 이미지가 없습니다",
                      noEmpty.empty() ? "" : "공 없는 이미지가 있는 촬영: " + join(withEmpty)});
    checks.push_back({single.empty(), single.empty() ? "모든 쪽에 촬영이 2개 이상입니다" : join(single) + "의 촬영이 1개뿐입니다",
                      single.empty() ? "" : "장면 다양성이 낮아 점수가 그 촬영에 좌우됩니다"});
    checks.push_back({days.size() != 1, days.size() == 1 ? "모든 촬영이 같은 날입니다" : "여러 날의 촬영이 섞여 있습니다",
                      days.size() == 1 ? "다른 날 촬영이 들어오면 평가에 넣는 것을 권합니다" : ""});
    return checks;
}
namespace {
// 묶음 ID를 그 묶음의 첫 촬영 ID로 바꾼다. 묶음을 떠난 촬영의 ID가 묶음 이름으로 남지 않게 한다.
void renameGroups(Json& groups) {
    std::map<std::string, std::string> first;
    for (const auto& [recording, group] : groups.items()) first.emplace(group.get<std::string>(), recording);   // items()는 ID 순서
    for (auto& [recording, group] : groups.items()) group = first.at(group.get<std::string>());
}
}
void mergeGroups(Json& groups, const std::vector<std::string>& recordings) {
    std::set<std::string> joined;
    for (const auto& recording : recordings) joined.insert(groups.value(recording, recording));
    if (joined.empty()) return;
    for (auto& [recording, group] : groups.items()) if (joined.count(group.get<std::string>())) group = *joined.begin();
    renameGroups(groups);
}
void leaveGroup(Json& groups, const std::string& recording) {
    groups[recording] = "";   // 다른 묶음 ID와 겹치지 않는 임시 값
    renameGroups(groups);
}
}
