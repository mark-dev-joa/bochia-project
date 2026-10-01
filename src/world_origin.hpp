#pragma once
#include "rig.hpp"

struct BoardPose {
    cv::Mat R,T; // Board -> reference camera A, metres.
    std::vector<double> errors;
    std::vector<int> cornerRotations;
    std::vector<int> measuredCameras; // Empty means every camera (legacy board observations).
};
struct RectangleOriginConfig {
    double widthMm=297,heightMm=210;
    void validate()const;
};
struct WorldOrigin {
    cv::Mat R,T; // World -> reference camera A: X_A = R * X_world + T.
    std::vector<double> errors;
    int quarterTurns=0;
    double surfaceHeightMm=0;
    std::vector<int> measuredCameras; // Empty means every camera; unmeasured errors are not RMS observations.
    bool measured(size_t camera)const;
    void validate()const;
    cv::Mat cameraCenter(const RigCalibration& rig,size_t camera)const;
};
BoardPose locateBoard(const RigSample& sample,const BoardConfig& board,
                      const std::vector<Calibration>& intrinsics,const RigCalibration& rig,std::stop_token stop={});
WorldOrigin makeWorldOrigin(const BoardPose& pose,const BoardConfig& board,const RigCalibration& rig,
                            int quarterTurns=0,double surfaceHeightMm=0);
BoardPose locateRectangle(const std::vector<cv::Point2f>& corners,const RectangleOriginConfig& rectangle,
                           const Calibration& intrinsics,const RigCalibration& rig,size_t camera);
WorldOrigin makeRectangleWorldOrigin(const BoardPose& pose,const RectangleOriginConfig& rectangle,const RigCalibration& rig,
                                     int quarterTurns=0,double surfaceHeightMm=0);
cv::Mat drawWorldAxes(const cv::Mat& image,const Calibration& intrinsics,const RigCalibration& rig,
                      const WorldOrigin& origin,size_t camera,double lengthM);
