#pragma once
#include "stereo.hpp"
#include "auto_capture.hpp"

struct RigSample {
    std::vector<Observation> cameras; // A, B, optionally C; one indivisible capture.
    double receiptSkewMs=0; // Max minus min host receipt time across ALL cameras.
};
struct RigCalibration {
    std::vector<StereoCalibration> fromReference; // A->B, A->C; same sample indices.
    void validate() const;
};
bool usableRigTiming(const std::vector<double>& receiptTimes,double now);
void validateRigSample(const RigSample& sample,const BoardConfig& board,size_t cameraCount);
RigCalibration calibrateRig(const std::vector<RigSample>& samples,const BoardConfig& board,
                            const std::vector<Calibration>& intrinsics,std::stop_token stop={});
void saveRigArchive(const std::string& directory,const std::vector<std::string>& serials,
                    const BoardConfig& board,const std::vector<RigSample>& samples,
                    const std::vector<Calibration>& intrinsics,const RigCalibration& result);
class RigCapture {
public:
    void start(size_t cameraCount,const AutoCaptureConfig& options);
    void stop();
    void tick(double now);
    bool running() const;
    float progress() const;
    bool observe(const RigSample& sample,const BoardConfig& board,const std::vector<RigSample>& existing,
                 const std::vector<double>& receiptTimes,double now);
    void captured(double now,size_t count);
private:
    std::vector<AutoCapture> captures;
};
