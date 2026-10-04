#include "native_labeler_view.hpp"
#include <algorithm>
#include <numeric>

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
}
