#pragma once
// Annotation helpers shared by the store, the review rules and the proposals: the checks of labeler_assist.py
// and ball_labeler.validate_boxes with the same messages. All outputs are proposals or validated copies.
#include "labeler_sam.hpp"
#include "native_labeler_model.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace boccia::labeler {
inline constexpr const char* ballColors[] = {"red", "blue", "white", "black"};
// A finite number inside [low, high]; booleans and strings are refused like Python's bounded_number.
double boundedNumber(const Json& value, double low, double high, const std::string& name);
// Python decode_mask: the size, the run count, the run values and the total must fit the image; an empty mask is refused.
cv::Mat decodeMaskChecked(const Json& rle, int width, int height);
Json maskContours(const cv::Mat& mask);              // [[[x, y], …], …] of every contour with three or more points
double overlap(const Json& a, const Json& b);        // intersection over union of two [x, y, w, h] boxes
std::string ballColor(const cv::Mat& image, const cv::Mat& mask);   // red, blue, white, black or unknown
Json polygonProposal(const Json& points, int width, int height);
Json validateBoxes(const Json& boxes, int width, int height);
// A ball's outline measured again from the image: the Canny edges within `band` pixels of the mask's outline, fitted
// with a circle (RANSAC, then least squares). ok when enough of the circumference was seen and the circle agrees
// with the mask in size and place; the box of a ball is then [x - r, y - r, 2r, 2r].
struct CircleFit {
    double x = 0, y = 0, r = 0;
    double coverage = 0;      // share of the 36 ten-degree sectors with an inlier edge pixel
    double residual = 0;      // mean distance of the inliers from the circle, in pixels
    int points = 0, inliers = 0;
    bool ok = false;
    double low = 0, high = 0; // the Canny thresholds used
};
CircleFit refineCircle(const cv::Mat& image, const cv::Mat& mask, int band = 3, double low = 20, double high = 60);
// Applies the refinement to a masked box: the mask becomes the circle's disc (so bbox follows it), detection_bbox the
// circle's box clipped to the image, and circle records [x, y, r] in continuous image coordinates. false, and the
// box untouched, when the fit is not trusted.
bool refineBallBox(Json& box, const cv::Mat& image);
cv::Mat discMask(double x, double y, double r, int width, int height);   // the pixels whose centres lie inside the circle
bool refinable(const Json& box);   // masked, not occluded, not drawn or corrected by hand, and not refined yet
// Local annotation proposals: EfficientSAM clicks, circles and frame matching. All outputs are proposals.
class BallAssist {
public:
    explicit BallAssist(std::filesystem::path modelDir, std::unique_ptr<SamBackend> backend = nullptr);
    Json status() const;   // ready, model, device, license, model_files, runtime
    // One ball from clicks inside (label 1) and outside (label 0) it; an optional [x, y, w, h] box prompt from a drag.
    Json segment(const cv::Mat& image, const std::string& key, const Json& points, const Json& radius = 14, const Json& seedBox = nullptr);
    static Json circles(const cv::Mat& image, const Json& minimum = 6, const Json& maximum = 45, const Json& existing = Json::array());
    static Json propagate(const cv::Mat& previous, const cv::Mat& image, const Json& boxes, const Json& existing = Json::array(), int search = 64);
    const std::filesystem::path modelDir;
private:
    std::mutex lock_;
    std::unique_ptr<SamBackend> backend_;
    std::optional<std::pair<std::string, std::vector<float>>> cache_;   // the embeddings of the last crop
};
}
