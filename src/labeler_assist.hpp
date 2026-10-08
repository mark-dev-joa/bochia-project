#pragma once
// Annotation helpers shared by the store, the review rules and the proposals: the checks of labeler_assist.py
// and ball_labeler.validate_boxes with the same messages. All outputs are proposals or validated copies.
#include "native_labeler_model.hpp"
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
}
