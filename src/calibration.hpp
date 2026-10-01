#pragma once
#include "brightness.hpp"
#include <opencv2/core.hpp>
#include <opencv2/objdetect/charuco_detector.hpp>
#include <string>
#include <vector>

enum class Pattern { Chessboard, Charuco };
struct BoardConfig {
    Pattern pattern = Pattern::Chessboard;
    int columns = 9, rows = 6; // chessboard: INNER corners; Charuco: squares
    double squareMm = 25, markerMm = 17.5;
    bool legacy = false;
    void validate() const;
    cv::aruco::CharucoBoard charucoBoard() const;
};
struct Observation {
    cv::Mat image;
    std::vector<cv::Point2f> imagePoints;
    std::vector<cv::Point3f> objectPoints;
    std::vector<int> ids;
    std::string source;
    uint64_t frameId = 0, timestamp = 0;
    BrightnessOptions softwareBrightness;
};
struct Calibration {
    cv::Mat K, distortion;
    cv::Size imageSize;
    double rms = 0;
    std::vector<double> perViewErrors;
};
Observation detectBoard(const cv::Mat& image, const BoardConfig& config, const BrightnessOptions& brightness={});
void validateSample(const Observation& sample, const std::vector<Observation>& existing);
Calibration calibrate(const std::vector<Observation>& samples);
cv::Mat undistortFrame(const cv::Mat& image,const Calibration& calibration);
void saveCalibration(const std::string& directory, const std::string& cameraIdentity,
                     const BoardConfig& config, const std::vector<Observation>& samples,
                     const Calibration& result);
