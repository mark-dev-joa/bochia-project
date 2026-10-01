#pragma once
#include <opencv2/core.hpp>

struct BrightnessOptions {
    bool enabled=false;
    bool fitted=false;
    int blackLevel=0,whiteLevel=255;
    void validate() const;
    bool operator==(const BrightnessOptions&) const = default;
};
struct Observation;
BrightnessOptions estimateBoardBrightness(const Observation& observation);
cv::Mat applySoftwareBrightness(const cv::Mat& image,const BrightnessOptions& options);
// Display only: contrast around 128, then additive brightness in 8-bit levels.
cv::Mat adjustDisplay(const cv::Mat& image,double brightness,double contrast);
