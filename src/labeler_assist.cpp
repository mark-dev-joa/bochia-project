#include "labeler_assist.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <format>
#include <random>
#include <stdexcept>
#include <utility>

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
namespace {
struct FitPoint { double x, y; };
// Gauss-Newton on the geometric distance: a few steps from a good start are enough for a near-complete arc.
void geometricFit(const std::vector<FitPoint>& points, double& cx, double& cy, double& r) {
    for (int step = 0; step < 8; ++step) {
        cv::Matx33d normal = cv::Matx33d::zeros(); cv::Vec3d rhs(0, 0, 0);
        for (const auto& p : points) {
            const double dx = p.x - cx, dy = p.y - cy, d = std::max(1e-9, std::hypot(dx, dy));
            const double j[3] = {-dx / d, -dy / d, -1}, residual = d - r;
            for (int a = 0; a < 3; ++a) { rhs[a] -= j[a] * residual; for (int b = 0; b < 3; ++b) normal(a, b) += j[a] * j[b]; }
        }
        cv::Vec3d delta;
        if (!cv::solve(normal, rhs, delta, cv::DECOMP_SVD)) break;
        cx += delta[0]; cy += delta[1]; r += delta[2];
        if (std::abs(delta[0]) + std::abs(delta[1]) + std::abs(delta[2]) < 1e-4) break;
    }
}
}
CircleFit refineCircle(const cv::Mat& image, const cv::Mat& mask, int band, double low, double high) {
    CircleFit fit; fit.low = low; fit.high = high;
    const int area = cv::countNonZero(mask);
    if (area < 12) return fit;
    const cv::Rect bounds = cv::boundingRect(mask);
    const double r0 = std::sqrt(area / CV_PI);
    const cv::Moments moments = cv::moments(mask, true);
    const double mx = moments.m10 / moments.m00, my = moments.m01 / moments.m00;
    const int margin = std::max(6, int(std::ceil(r0 * .5)) + band);
    const cv::Rect crop = cv::Rect(bounds.x - margin, bounds.y - margin, bounds.width + 2 * margin, bounds.height + 2 * margin) & cv::Rect(0, 0, image.cols, image.rows);
    cv::Mat gray;
    if (image.channels() == 3) cv::cvtColor(image(crop), gray, cv::COLOR_BGR2GRAY); else gray = image(crop).clone();
    cv::GaussianBlur(gray, gray, {3, 3}, 0);
    cv::Mat edges; cv::Canny(gray, edges, low, high, 3, true);
    // Only the edges near the mask's outline count: not the seams inside the ball, not the floor further out.
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, {2 * band + 1, 2 * band + 1});
    cv::Mat local = mask(crop), outer, inner, ring;
    cv::dilate(local, outer, kernel); cv::erode(local, inner, kernel); cv::subtract(outer, inner, ring);
    std::vector<FitPoint> points;
    for (int y = 0; y < edges.rows; ++y) for (int x = 0; x < edges.cols; ++x) if (edges.at<uchar>(y, x) && ring.at<uchar>(y, x)) points.push_back({crop.x + x + .5, crop.y + y + .5});
    fit.points = int(points.size());
    if (points.size() < 8) return fit;
    // RANSAC: the shadow under the ball and the floor's wood grain also leave edges in the band.
    std::mt19937 random(7);
    std::uniform_int_distribution<size_t> pick(0, points.size() - 1);
    double bestX = 0, bestY = 0, bestR = 0; int bestCount = 0; double bestError = 1e18;
    const double threshold = 1.0;
    for (int iteration = 0; iteration < 300; ++iteration) {
        const auto& a = points[pick(random)]; const auto& b = points[pick(random)]; const auto& c = points[pick(random)];
        const double d = 2 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
        if (std::abs(d) < 1e-6) continue;
        const double a2 = a.x * a.x + a.y * a.y, b2 = b.x * b.x + b.y * b.y, c2 = c.x * c.x + c.y * c.y;
        const double ux = (a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d, uy = (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d;
        const double r = std::hypot(a.x - ux, a.y - uy);
        if (r < .6 * r0 || r > 1.6 * r0 || std::hypot(ux - mx, uy - my) > .6 * r0) continue;
        int count = 0; double error = 0;
        for (const auto& p : points) { const double e = std::abs(std::hypot(p.x - ux, p.y - uy) - r); if (e <= threshold) { ++count; error += e; } }
        if (count > bestCount || (count == bestCount && error < bestError)) { bestCount = count; bestError = error; bestX = ux; bestY = uy; bestR = r; }
    }
    if (bestCount < 6) return fit;
    double cx = bestX, cy = bestY, r = bestR;
    for (int round = 0; round < 2; ++round) {
        std::vector<FitPoint> inliers;
        for (const auto& p : points) if (std::abs(std::hypot(p.x - cx, p.y - cy) - r) <= threshold) inliers.push_back(p);
        if (inliers.size() < 6) return fit;
        geometricFit(inliers, cx, cy, r);
    }
    // Canny marks whole pixels, half a pixel inside a sharp rim on average. The rim's true place along each inlier's
    // radial line is the vertex of a parabola through the gradient magnitude one pixel inward, at the pixel and one
    // pixel outward; the circle is fitted once more through those sub-pixel points.
    cv::Mat gx, gy, magnitude; cv::Sobel(gray, gx, CV_32F, 1, 0, 3); cv::Sobel(gray, gy, CV_32F, 0, 1, 3); cv::magnitude(gx, gy, magnitude);
    auto sample = [&](double x, double y) {   // bilinear, continuous image coordinates (pixel i centred at i + .5)
        const double fx = x - crop.x - .5, fy = y - crop.y - .5;
        const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy)); const double tx = fx - x0, ty = fy - y0;
        auto at = [&](int px, int py) { return double(magnitude.at<float>(std::clamp(py, 0, magnitude.rows - 1), std::clamp(px, 0, magnitude.cols - 1))); };
        return (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty) + (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
    };
    {
        std::vector<FitPoint> refined;
        for (const auto& p : points) {
            const double d = std::hypot(p.x - cx, p.y - cy);
            if (std::abs(d - r) > threshold || d < 1e-6) continue;
            const double ux = (p.x - cx) / d, uy = (p.y - cy) / d;
            const double before = sample(p.x - ux, p.y - uy), here = sample(p.x, p.y), beyond = sample(p.x + ux, p.y + uy);
            const double curvature = before - 2 * here + beyond;
            const double shift = std::abs(curvature) > 1e-6 ? std::clamp(.5 * (before - beyond) / curvature, -1.0, 1.0) : 0.0;
            refined.push_back({p.x + shift * ux, p.y + shift * uy});
        }
        if (refined.size() >= 6) { geometricFit(refined, cx, cy, r); points = refined; }
    }
    std::vector<bool> sector(36, false); int inliers = 0; double residual = 0;
    for (const auto& p : points) {
        const double e = std::abs(std::hypot(p.x - cx, p.y - cy) - r);
        if (e > threshold) continue;
        ++inliers; residual += e;
        sector[size_t((int(std::floor((std::atan2(p.y - cy, p.x - cx) + CV_PI) / (2 * CV_PI) * 36)) % 36 + 36) % 36)] = true;
    }
    fit.x = cx; fit.y = cy; fit.r = r; fit.inliers = inliers; fit.residual = inliers ? residual / inliers : 0;
    fit.coverage = double(std::count(sector.begin(), sector.end(), true)) / 36;
    fit.ok = inliers >= 12 && fit.coverage >= .5 && std::abs(r - r0) <= .35 * r0 && std::hypot(cx - mx, cy - my) <= .5 * r0;
    return fit;
}
bool refinable(const Json& box, bool remeasure) {
    const auto source = box.value("source", "manual");
    // A box corrected by hand (detection_bbox without a measured circle) is a decision a person made; it stays.
    const bool handCorrected = box.contains("detection_bbox") && !box.contains("circle");
    return box.contains("segmentation") && !box.value("occluded", false) && (remeasure || !box.contains("circle")) && !handCorrected
        && source != "manual" && source != "manual_brush" && source != "manual_polygon";
}
cv::Mat discMask(double x, double y, double r, int width, int height) {
    cv::Mat disc(height, width, CV_8UC1, cv::Scalar(0));
    const int x0 = std::max(0, int(std::floor(x - r - 1))), x1 = std::min(width - 1, int(std::ceil(x + r + 1)));
    const int y0 = std::max(0, int(std::floor(y - r - 1))), y1 = std::min(height - 1, int(std::ceil(y + r + 1)));
    for (int py = y0; py <= y1; ++py) for (int px = x0; px <= x1; ++px) if (std::hypot(px + .5 - x, py + .5 - y) <= r) disc.at<uchar>(py, px) = 1;
    return disc;
}
bool refineBallBox(Json& box, const cv::Mat& image, double margin) {
    if (!box.contains("segmentation")) return false;
    CircleFit fit = refineCircle(image, decodeMaskChecked(box["segmentation"], image.cols, image.rows));
    if (!fit.ok) return false;
    fit.r = std::max(1.0, fit.r + margin);   // the rim margin: the gradient's peak lies a little inside the ball's outer edge
    auto round4 = [](double value) { return std::nearbyint(value * 10000) / 10000; };
    const double left = std::max(0.0, fit.x - fit.r), top = std::max(0.0, fit.y - fit.r);
    const double right = std::min(double(image.cols), fit.x + fit.r), bottom = std::min(double(image.rows), fit.y + fit.r);
    if (right - left < 1 || bottom - top < 1) return false;
    const cv::Mat disc = discMask(fit.x, fit.y, fit.r, image.cols, image.rows);
    if (cv::countNonZero(disc) < 4) return false;
    const cv::Rect bounds = cv::boundingRect(disc);
    box["segmentation"] = encodeMask(disc);   // the pixels inside the measured circle
    box["bbox"] = {bounds.x, bounds.y, bounds.width, bounds.height};
    if (box.contains("area")) box["area"] = cv::countNonZero(disc);
    if (box.contains("contours")) box["contours"] = maskContours(disc);
    box["detection_bbox"] = {round4(left), round4(top), round4(right - left), round4(bottom - top)};
    box["circle"] = {round4(fit.x), round4(fit.y), round4(fit.r)};
    return true;
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
        if (box.contains("circle") && box["circle"].is_array() && box["circle"].size() == 3 && finiteNumber(box["circle"][0]) && finiteNumber(box["circle"][1]) && finiteNumber(box["circle"][2]) && box["circle"][2].get<double>() > 0)
            item["circle"] = {box["circle"][0].get<double>(), box["circle"][1].get<double>(), box["circle"][2].get<double>()};   // the ball measured from the image
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
BallAssist::BallAssist(std::filesystem::path dir, std::unique_ptr<SamBackend> backend) : modelDir(std::move(dir)), backend_(std::move(backend)) {}
Json BallAssist::status() const {
    const bool files = samModelFilesExist(modelDir);
    return {{"ready", files || backend_ != nullptr}, {"model", "EfficientSAM-Ti"}, {"device", "CPU"}, {"license", "Apache-2.0"}, {"model_files", files}, {"runtime", true}};
}
Json BallAssist::segment(const cv::Mat& image, const std::string& key, const Json& points, const Json& radiusValue, const Json& seedBox) {
    const int height = image.rows, width = image.cols;
    const double radius = boundedNumber(radiusValue, 3, 300, "예상 공 반지름");
    if (!points.is_array() || points.empty() || points.size() > 32) throw std::runtime_error("분할 클릭은 1~32개여야 합니다.");
    std::vector<cv::Point2d> coords; std::vector<int> labels;
    for (const auto& point : points) {
        if (!point.is_object() || !point.contains("label") || !point["label"].is_number_integer() || (point["label"] != 0 && point["label"] != 1))
            throw std::runtime_error("포함/제외 클릭 형식이 올바르지 않습니다.");
        coords.emplace_back(boundedNumber(point.value("x", Json()), 0, width - 1, "클릭 X"), boundedNumber(point.value("y", Json()), 0, height - 1, "클릭 Y"));
        labels.push_back(point["label"].get<int>());
    }
    if (labels[0] != 1) throw std::runtime_error("첫 클릭은 공 내부의 포함 클릭이어야 합니다.");
    // A local crop gives the model enough pixels for the small distant balls.
    const double cx = coords[0].x, cy = coords[0].y;
    const int half = int(std::max(32.0, radius * 3.5));
    const int left = std::max(0, int(cx) - half), top = std::max(0, int(cy) - half);
    const int right = std::min(width, int(cx) + half + 1), bottom = std::min(height, int(cy) + half + 1);
    for (const auto& point : coords) if (!(left <= point.x && point.x < right && top <= point.y && point.y < bottom))
        throw std::runtime_error("추가 클릭은 첫 공 주변에서 해주세요. 새 공은 현재 제안을 추가/취소한 뒤 클릭하세요.");
    const cv::Mat crop = image(cv::Rect(left, top, right - left, bottom - top));
    std::vector<cv::Point2f> prompts; std::vector<float> promptLabels;
    for (size_t i = 0; i < coords.size(); ++i) { prompts.emplace_back(float(coords[i].x - left), float(coords[i].y - top)); promptLabels.push_back(float(labels[i])); }
    if (!seedBox.is_null()) {
        if (!seedBox.is_array() || seedBox.size() != 4) throw std::runtime_error("자동 분할의 시작 박스가 올바르지 않습니다.");
        double box[4];
        for (int i = 0; i < 4; ++i) box[i] = boundedNumber(seedBox[size_t(i)], 0, std::max(width, height), "시작 박스");
        const double x = box[0], y = box[1], w = box[2], h = box[3];
        if (w < 1 || h < 1 || x + w > width || y + h > height) throw std::runtime_error("자동 분할의 시작 박스는 이미지 안에 있어야 합니다.");
        prompts.emplace_back(float(x - left), float(y - top)); promptLabels.push_back(2);                                                        // EfficientSAM box corners,
        prompts.emplace_back(float(std::min(double(width - 1), x + w) - left), float(std::min(double(height - 1), y + h) - top)); promptLabels.push_back(3);   // plus the positive center
    }
    const std::string cacheKey = key + "|" + std::to_string(left) + "," + std::to_string(top) + "," + std::to_string(right) + "," + std::to_string(bottom);
    SamOutput output;
    {
        std::lock_guard guard(lock_);
        if (!backend_) {
            if (!samModelFilesExist(modelDir)) throw std::runtime_error("분할 모델이 준비되지 않았습니다. models/efficientsam 폴더에 efficientsam_ti_encoder.onnx와 efficientsam_ti_decoder.onnx를 두세요.");
            backend_ = openSamModel(modelDir);
        }
        if (!cache_ || cache_->first != cacheKey) cache_ = std::make_pair(cacheKey, backend_->encode(crop));
        output = backend_->decode(cache_->second, prompts, promptLabels, crop.size());
    }
    struct Candidate { float iou; cv::Mat mask; cv::Rect bounds; int area; };
    std::vector<Candidate> candidates;
    for (size_t i = 0; i < output.logits.size() && i < output.ious.size(); ++i) {
        cv::Mat positive = output.logits[i] >= 0, components;
        positive.convertTo(positive, CV_8UC1, 1.0 / 255);
        cv::connectedComponents(positive, components, 8, CV_32S);
        const int anchor = components.at<int>(int(cy) - top, int(cx) - left);
        if (anchor == 0) continue;
        cv::Mat mask = components == anchor;
        mask.convertTo(mask, CV_8UC1, 1.0 / 255);
        bool consistent = true;
        for (size_t p = 0; p < coords.size(); ++p) if ((mask.at<uchar>(int(coords[p].y) - top, int(coords[p].x) - left) != 0) != (labels[p] != 0)) consistent = false;
        if (!consistent) continue;
        const cv::Rect bounds = cv::boundingRect(mask);
        const int area = cv::countNonZero(mask);
        if (area < 4 || area > mask.total() * .7 || std::max(bounds.width, bounds.height) > 2 * half * .95) continue;
        candidates.push_back({output.ious[i], mask, bounds, area});
    }
    if (candidates.empty()) throw std::runtime_error("공 영역을 분리하지 못했습니다. 공 중심을 다시 클릭하거나 반지름을 조정하고, Shift+클릭으로 주변을 제외하세요.");
    const auto& best = *std::max_element(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.iou < b.iou; });
    cv::Mat full(height, width, CV_8UC1, cv::Scalar(0));
    best.mask.copyTo(full(cv::Rect(left, top, right - left, bottom - top)));
    return {{"bbox", {left + best.bounds.x, top + best.bounds.y, best.bounds.width, best.bounds.height}}, {"occluded", false}, {"segmentation", encodeMask(full)},
            {"contours", maskContours(full)}, {"source", "efficientsam"}, {"quality", std::nearbyint(double(best.iou) * 10000) / 10000}, {"area", best.area}};
}
Json BallAssist::circles(const cv::Mat& image, const Json& minimumValue, const Json& maximumValue, const Json& existing) {
    const int minimum = int(boundedNumber(minimumValue, 2, 300, "최소 반지름"));
    const int maximum = int(boundedNumber(maximumValue, minimum, 300, "최대 반지름"));
    const int height = image.rows, width = image.cols;
    cv::Mat gray; cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    cv::createCLAHE(2, {8, 8})->apply(gray, gray);
    cv::GaussianBlur(gray, gray, {5, 5}, 1);
    std::vector<cv::Vec3f> found;
    cv::HoughCircles(gray, found, cv::HOUGH_GRADIENT, 1.2, minimum * 2, 90, 22, minimum, maximum);
    Json proposals = Json::array();
    for (const auto& circle : found) {
        const double cx = circle[0], cy = circle[1], radius = circle[2];
        const int left = std::max(0, int(std::floor(cx - radius))), top = std::max(0, int(std::floor(cy - radius)));
        const int right = std::min(width, int(std::ceil(cx + radius))), bottom = std::min(height, int(std::ceil(cy + radius)));
        const Json bbox = {left, top, right - left, bottom - top};
        bool taken = false;
        for (const auto& box : existing) if (overlap(bbox, box["bbox"]) > .4) taken = true;
        if (taken) continue;
        proposals.push_back({{"bbox", bbox}, {"occluded", false}, {"source", "circle"}});
        if (proposals.size() >= 100) break;
    }
    return proposals;
}
namespace {
// Python arithmetic on the stored box: an integer box moved by whole pixels stays integer, a float box stays float,
// and min()/max() hand back whichever operand wins, so the proposal reads exactly as the Python engine's.
Json plus(const Json& a, int b) { return a.is_number_integer() ? Json(a.get<long long>() + b) : Json(a.get<double>() + b); }
Json minus(int a, const Json& b) { return b.is_number_integer() ? Json(a - b.get<long long>()) : Json(a - b.get<double>()); }
Json pyMin(const Json& a, const Json& b) { return a.get<double>() <= b.get<double>() ? a : b; }
Json pyMax(const Json& a, const Json& b) { return a.get<double>() >= b.get<double>() ? a : b; }
}
Json BallAssist::propagate(const cv::Mat& previous, const cv::Mat& image, const Json& boxes, const Json& existing, int search) {
    if (previous.size() != image.size() || previous.channels() != image.channels()) throw std::runtime_error("이전 프레임과 이미지 크기가 다릅니다.");
    const int height = image.rows, width = image.cols;
    cv::Mat old, now; cv::cvtColor(previous, old, cv::COLOR_BGR2GRAY); cv::cvtColor(image, now, cv::COLOR_BGR2GRAY);
    Json proposals = Json::array();
    for (const auto& box : boxes) {
        const double x = box["bbox"][0], y = box["bbox"][1], w = box["bbox"][2], h = box["bbox"][3];
        const int left = std::max(0, int(std::floor(x)) - 3), top = std::max(0, int(std::floor(y)) - 3);
        const int right = std::min(width, int(std::ceil(x + w)) + 3), bottom = std::min(height, int(std::ceil(y + h)) + 3);
        if (right - left < 2 || bottom - top < 2) continue;
        const cv::Mat patch = old(cv::Rect(left, top, right - left, bottom - top));
        cv::Scalar mean, deviation; cv::meanStdDev(patch, mean, deviation);
        if (deviation[0] < 3) continue;
        const int sx = std::max(0, left - search), sy = std::max(0, top - search), ex = std::min(width, right + search), ey = std::min(height, bottom + search);
        cv::Mat response; cv::matchTemplate(now(cv::Rect(sx, sy, ex - sx, ey - sy)), patch, response, cv::TM_CCOEFF_NORMED);
        double low, quality; cv::Point lowAt, location; cv::minMaxLoc(response, &low, &quality, &lowAt, &location);
        if (quality < .7) continue;
        const int dx = sx + location.x - left, dy = sy + location.y - top;
        const Json bbox = {pyMax(0, pyMin(minus(width, box["bbox"][2]), plus(box["bbox"][0], dx))), pyMax(0, pyMin(minus(height, box["bbox"][3]), plus(box["bbox"][1], dy))), box["bbox"][2], box["bbox"][3]};
        bool taken = false;
        for (const auto& item : existing) if (overlap(bbox, item["bbox"]) > .4) taken = true;
        for (const auto& item : proposals) if (overlap(bbox, item["bbox"]) > .4) taken = true;
        if (taken) continue;
        // A mask from another frame would be inaccurate; only propose the box.
        proposals.push_back({{"bbox", bbox}, {"occluded", box.value("occluded", false)}, {"source", "tracking"}, {"quality", std::nearbyint(quality * 10000) / 10000}});
    }
    return proposals;
}
}
