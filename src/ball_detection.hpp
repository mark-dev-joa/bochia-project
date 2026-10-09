#pragma once
#include <opencv2/core.hpp>
#include "yolo_detector.hpp"
#include <nlohmann/json.hpp>
#include <map>
#include <stop_token>
#include <string>
#include <vector>

struct BallOptions {
    bool color=false,detect=true,overlay=true,roiEnabled=true,ransac=true;
    bool showCandidates=false,showCenter=true,showRoi=true;
    int stage=0,blur=1,kernel=5,minRadius=4,maxRadius=53,minDistance=18;
    int cannyLow=90,cannyHigh=180,houghEdge=180,houghVotes=14,iterations=60;
    double brightness=0,contrast=1,scale=.5,dp=1,reach=1.5,tolerance=2,minCoverage=.5;
    int redWidth=12,blueLow=90,blueHigh=135,minSaturation=80,minValue=40,whiteSaturation=60,whiteValue=160;
    double colorFraction=.35;
    std::string model;            // A YOLO detector exported to ONNX; empty means Hough + RANSAC below. Applies before every other setting.
    double modelConfidence=.25;   // The model's confidence a ball needs.
    bool cuda=true;               // Run the model on the GPU through CUDA when available (falls back to the CPU).
    std::vector<cv::Point2f> roi; // Normalized source image coordinates.
    void validate()const;
    bool operator==(const BallOptions&)const=default;
};
struct BallSettings {
    bool run=false,overlay=true;
    std::map<std::string,BallOptions> cameras;
    std::string model;            // The learned detector (ONNX) for every camera, copied into each camera's options; empty means Hough + RANSAC.
    double modelConfidence=.25;
    bool cuda=true;               // GPU through CUDA when available, else the CPU; copied into every camera's options too.
    std::string cudaDlls;         // A folder with the CUDA runtime DLLs when they are not beside the program (see yoloCudaLibraryDirectory).
    void validate()const;
};
struct BallCircle {cv::Point2f center;float radius=0;double coverage=0;std::string color;};
struct BallDetection {
    std::vector<BallCircle> candidates,balls;
    cv::Mat gray,blurred,edges; // Native source dimensions, suitable for display.
    double milliseconds=0;
    bool candidateLimit=false;
};
void to_json(nlohmann::json& j,const BallOptions& o);
void from_json(const nlohmann::json& j,BallOptions& o);
void to_json(nlohmann::json& j,const BallSettings& o);
void from_json(const nlohmann::json& j,BallSettings& o);
BallDetection detectBalls(const cv::Mat& bgr,const BallOptions& options,std::stop_token stop={});
std::string classifyBallColor(const cv::Mat& bgr,const BallCircle& circle,const BallOptions& options);
