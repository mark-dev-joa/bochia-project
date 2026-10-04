#include "native_labeler_model.hpp"
#include <opencv2/imgproc.hpp>
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
    box.erase("detection_bbox"); box.erase("contours");
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
    box["detection_bbox"] = {left, top, right - left, bottom - top}; return true;
}
Json annotationPayload(const Json& frame, int revision) {
    return {{"recording", frame.at("recording")}, {"camera", frame.at("camera")}, {"index", frame.at("index")},
            {"boxes", frame.at("boxes")}, {"reviewed", frame.value("reviewed", false)},
            {"review_status", frame.value("review_status", frame.value("reviewed", false) ? "reviewed" : "editing")},
            {"expected_revision", revision}};
}
}
