#pragma once
#include "calibration.hpp"
#include <array>
#include <optional>

struct AutoCaptureConfig {
    int target=25;
    double holdSeconds=0.7, intervalSeconds=1.5;
    void validate() const;
};
enum class CaptureStatus { Stopped, Waiting, Hold, MoveBoard, Cooldown, Stale, Complete };

// Feed each completed detection once, using the frame's host receipt time.
// No image processing or camera SDK calls run from the UI's per-frame tick.
class AutoCapture {
public:
    void start(const AutoCaptureConfig& config);
    void stop();
    void tick(double now);
    bool observe(const Observation& frame,const BoardConfig& board,
                 const std::vector<Observation>& samples,double capturedAt,double now);
    void captured(double now,size_t sampleCount);
    bool running() const { return enabled; }
    CaptureStatus status() const { return state; }
    float progress() const { return holdProgress; }
private:
    using Pose=std::array<cv::Point2f,4>;
    static std::optional<Pose> pose(const Observation& frame,const BoardConfig& board);
    static double distance(const Pose& a,const Pose& b);
    void resetHold();
    AutoCaptureConfig options;
    bool enabled=false;
    CaptureStatus state=CaptureStatus::Stopped;
    std::optional<Pose> anchor;
    double holdStart=0,lastFrame=-1,lastCapture=-1e9;
    int stableFrames=0;
    float holdProgress=0;
};
