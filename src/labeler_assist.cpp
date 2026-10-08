#include "labeler_assist.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace boccia::labeler {
namespace {
bool finiteNumber(const Json& value) { return value.is_number() && !value.is_boolean() && std::isfinite(value.get<double>()); }
double roundHalfEven(double value) { return std::nearbyint(value); }   // Python's round(): ties go to the even digit
}
double boundedNumber(const Json& value, double low, double high, const std::string& name) {
    if (!finiteNumber(value) || value.get<double>() < low || value.get<double>() > high)
        throw std::runtime_error(std::format("{}: {:g}~{:g} 사이의 숫자를 입력하세요.", name, low, high));
    return value.get<double>();
}
cv::Mat decodeMaskChecked(const Json& rle, int width, int height) {
    if (!rle.is_object() || !rle.contains("size") || rle["size"] != Json::array({height, width})) throw std::runtime_error("분할 마스크의 크기가 원본과 다릅니다.");
    const auto counts = rle.contains("counts") ? rle["counts"] : Json();
    const size_t total = size_t(width) * size_t(height);
    if (!counts.is_array() || counts.empty() || counts.size() > total + 1) throw std::runtime_error("분할 마스크의 RLE 형식이 올바르지 않습니다.");
    long long sum = 0, filled = 0; size_t position = 0;
    for (const auto& run : counts) {
        if (!run.is_number_integer() || run.get<long long>() < 0) throw std::runtime_error("분할 마스크의 RLE 길이가 올바르지 않습니다.");
        sum += run.get<long long>(); if (position % 2) filled += run.get<long long>(); ++position;
    }
    if (sum != (long long)total) throw std::runtime_error("분할 마스크의 RLE 길이가 올바르지 않습니다.");
    if (!filled) throw std::runtime_error("분할 마스크가 비어 있습니다.");
    cv::Mat mask(height, width, CV_8UC1, cv::Scalar(0));
    size_t offset = 0; bool on = false;
    for (const auto& run : counts) {
        const size_t count = run.get<size_t>();
        if (on) for (size_t i = offset; i < offset + count; ++i) mask.at<uchar>(int(i % size_t(height)), int(i / size_t(height))) = 1;   // column-major, like COCO
        offset += count; on = !on;
    }
    return mask;
}
Json maskContours(const cv::Mat& mask) {
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask.clone(), contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    Json result = Json::array();
    for (const auto& contour : contours) {
        if (contour.size() < 3) continue;
        Json points = Json::array();
        for (const auto& point : contour) points.push_back({point.x, point.y});
        result.push_back(std::move(points));
    }
    return result;
}
double overlap(const Json& a, const Json& b) {
    const double ax = a.at(0).get<double>(), ay = a.at(1).get<double>(), aw = a.at(2).get<double>(), ah = a.at(3).get<double>();
    const double bx = b.at(0).get<double>(), by = b.at(1).get<double>(), bw = b.at(2).get<double>(), bh = b.at(3).get<double>();
    const double x = std::max(ax, bx), y = std::max(ay, by);
    const double w = std::max(0.0, std::min(ax + aw, bx + bw) - x), h = std::max(0.0, std::min(ay + ah, by + bh) - y);
    const double area = w * h;
    return area / std::max(1.0, aw * ah + bw * bh - area);
}
std::string ballColor(const cv::Mat& image, const cv::Mat& mask) {
    // Thresholds from the 2026-10-02 recordings, dim under warm light: a red ball has hue 5-6 and saturation about 190,
    // the white jack hue 18 (the warm cast) and saturation about 100 at value 65, a black ball value about 11.
    if (image.channels() != 3) return "unknown";
    cv::Mat inner; cv::erode(mask, inner, cv::Mat::ones(5, 5, CV_8UC1));   // the edge pixels mix with the floor
    if (cv::countNonZero(inner) < 20) inner = mask;
    cv::Mat hsv; cv::cvtColor(image, hsv, cv::COLOR_BGR2HSV);
    std::vector<cv::Vec3b> pixels;
    for (int y = 0; y < mask.rows; ++y) for (int x = 0; x < mask.cols; ++x) if (inner.at<uchar>(y, x)) pixels.push_back(hsv.at<cv::Vec3b>(y, x));
    if (pixels.size() < 10) return "unknown";
    std::vector<int> hues(180, 0); size_t saturated = 0;
    for (const auto& pixel : pixels) if (pixel[1] > 120) { ++saturated; ++hues[pixel[0] % 180]; }
    if (double(saturated) >= double(pixels.size()) * .4) {
        const int hue = int(std::max_element(hues.begin(), hues.end()) - hues.begin());   // the first of equal counts, like argmax
        if (hue < 12 || hue > 165) return "red";
        if (95 <= hue && hue <= 135) return "blue";
    }
    std::vector<int> values; values.reserve(pixels.size());
    for (const auto& pixel : pixels) values.push_back(pixel[2]);
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    const double median = n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
    return median < 22 ? "black" : "white";
}
Json polygonProposal(const Json& points, int width, int height) {
    if (!points.is_array() || points.size() < 3 || points.size() > 2048) throw std::runtime_error("수동 윤곽은 꼭짓점이 3~2048개여야 합니다.");
    std::vector<cv::Point> polygon;
    for (const auto& point : points) {
        if (!point.is_array() || point.size() != 2) throw std::runtime_error("윤곽 좌표는 [x, y]여야 합니다.");
        polygon.emplace_back(int(roundHalfEven(boundedNumber(point[0], 0, width - 1, "윤곽 X"))), int(roundHalfEven(boundedNumber(point[1], 0, height - 1, "윤곽 Y"))));
    }
    if (std::abs(cv::contourArea(polygon)) < 1) throw std::runtime_error("윤곽의 면적이 너무 작습니다. 공 둘레를 따라 점을 찍으세요.");
    cv::Mat mask(height, width, CV_8UC1, cv::Scalar(0));
    cv::fillPoly(mask, std::vector<std::vector<cv::Point>>{polygon}, cv::Scalar(1));
    const auto bounds = cv::boundingRect(mask);
    return {{"bbox", {bounds.x, bounds.y, bounds.width, bounds.height}}, {"occluded", false}, {"source", "manual_polygon"},
            {"segmentation", encodeMask(mask)}, {"contours", maskContours(mask)}};
}
Json validateBoxes(const Json& boxes, int width, int height) {
    if (!boxes.is_array() || boxes.size() > 500) throw std::runtime_error("공 박스 목록이 올바르지 않습니다.");
    static const char* sources[] = {"manual", "manual_brush", "efficientsam", "circle", "tracking", "manual_polygon", "auto_sam", "auto_tracking", "auto_geometry"};
    Json validated = Json::array();
    for (const auto& box : boxes) {
        if (!box.is_object() || !box.contains("bbox") || !box["bbox"].is_array() || box["bbox"].size() != 4) throw std::runtime_error("박스 좌표는 [x, y, 너비, 높이]여야 합니다.");
        for (const auto& value : box["bbox"]) if (!finiteNumber(value)) throw std::runtime_error("박스 좌표는 유한한 숫자여야 합니다.");
        const double x = box["bbox"][0].get<double>(), y = box["bbox"][1].get<double>(), w = box["bbox"][2].get<double>(), h = box["bbox"][3].get<double>();
        if (x < 0 || y < 0 || w < .5 || h < .5 || x + w > width + .01 || y + h > height + .01) throw std::runtime_error("박스는 이미지 안에 있어야 하고 크기가 0.5픽셀 이상이어야 합니다.");
        const Json occluded = box.value("occluded", Json(false));
        if (!occluded.is_boolean()) throw std::runtime_error("가림 상태가 올바르지 않습니다.");
        Json item = {{"bbox", Json::array()}, {"occluded", occluded.get<bool>()}};
        for (const auto& value : box["bbox"]) item["bbox"].push_back(roundHalfEven(value.get<double>() * 10000) / 10000);
        if (box.contains("detection_bbox")) item["detection_bbox"] = validateBoxes(Json::array({{{"bbox", box["detection_bbox"]}}}), width, height)[0]["bbox"];   // independent of the mask
        const Json color = box.value("color", Json("unknown"));
        if (!color.is_string() || (color != "unknown" && std::find(std::begin(ballColors), std::end(ballColors), color.get<std::string>()) == std::end(ballColors))) throw std::runtime_error("공 색상 속성이 올바르지 않습니다.");
        if (color != "unknown") item["color"] = color;
        if (box.contains("source") && box["source"].is_string() && std::find(std::begin(sources), std::end(sources), box["source"].get<std::string>()) != std::end(sources)) item["source"] = box["source"];
        if (box.contains("segmentation")) {
            const cv::Mat mask = decodeMaskChecked(box["segmentation"], width, height);
            const auto bounds = cv::boundingRect(mask);
            const double expected[] = {double(bounds.x), double(bounds.y), double(bounds.width), double(bounds.height)};
            for (int i = 0; i < 4; ++i) if (std::abs(expected[i] - item["bbox"][i].get<double>()) > .01) throw std::runtime_error("분할 윤곽과 박스 좌표가 다릅니다. 박스 수정 시 기존 분할 윤곽을 제거하세요.");
            item["segmentation"] = encodeMask(mask);
        }
        validated.push_back(std::move(item));
    }
    return validated;
}
}
