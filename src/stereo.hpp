#pragma once
#include "calibration.hpp"
#include <array>
#include <stop_token>

struct StereoPair {
    Observation first,second;
    double receiptSkewMs=0; // Host receipt timing only; not exposure synchrony.
};
struct StereoCalibration {
    cv::Mat R,T; // X_B = R * X_A + T; metres, OpenCV camera axes.
    double rms=0;
    std::vector<double> perPairErrors;
    std::vector<int> secondRotations;
    void validate() const;
    cv::Mat secondCenterInFirst() const;
};
struct StereoPoints {
    std::vector<cv::Point3f> object;
    std::vector<cv::Point2f> first,second;
};
struct StereoRectification {
    std::array<cv::Mat,2> mapX,mapY;
    std::array<cv::Size,2> inputSize;
    cv::Size outputSize;
    // R maps native camera axes into rectified axes. P's first three columns
    // project those axes into final output pixels, including a possible 90-degree
    // clockwise image rotation. Full P uses the first rectified camera frame.
    std::array<cv::Mat,2> R,P;
    bool rotated=false;
};
StereoRectification makeStereoRectification(const Calibration& first,const Calibration& second,
                                             const StereoCalibration& relation);
cv::Mat rectifiedFrame(const cv::Mat& input,const StereoRectification& maps,int cameraIndex);
// Each index identifies the same epipolar plane in both images and every mode.
// Modes: 1 = undistorted, 2 = rectified. Empty paths mean off-image lines.
using EpipolarGuide = std::vector<std::vector<cv::Point2f>>;
using StereoEpipolarGuides = std::array<std::vector<EpipolarGuide>,2>;
StereoEpipolarGuides makeEpipolarGuides(const Calibration& first,const Calibration& second,
                                       const StereoRectification& maps,int mode,int count);
StereoPoints matchStereoPoints(const StereoPair& pair,const BoardConfig& board,int secondRotation=0);
bool usableStereoTiming(double firstAt,double secondAt,double now);
StereoCalibration calibrateStereo(const std::vector<StereoPair>& pairs,const BoardConfig& board,
                                  const Calibration& first,const Calibration& second,std::stop_token stop={});
void saveStereoArchive(const std::string& directory,const std::string& firstId,const std::string& secondId,
                       const BoardConfig& board,const std::vector<StereoPair>& pairs,
                       const Calibration& first,const Calibration& second,const StereoCalibration& result);
