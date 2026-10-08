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
