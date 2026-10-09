#pragma once
// A ball detector learned from the labeler's labels (YOLO26, exported to ONNX with the NMS inside), run with ONNX Runtime
// on the CPU. One instance per model file; a run is safe from several threads at once.
#include <opencv2/core.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct YoloBox { float x1 = 0, y1 = 0, x2 = 0, y2 = 0, confidence = 0; int label = 0; };   // source pixels

class YoloDetector {
public:
    explicit YoloDetector(const std::filesystem::path& model);
    ~YoloDetector();
    YoloDetector(const YoloDetector&) = delete;
    YoloDetector& operator=(const YoloDetector&) = delete;
    // Boxes with at least `confidence`, in the source image's pixels. The image is letterboxed to the model's input size.
    std::vector<YoloBox> detect(const cv::Mat& bgr, float confidence) const;
    int inputSize() const;
    const std::filesystem::path& path() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The detector for a model file, made on first use and kept; the same one for every caller.
std::shared_ptr<const YoloDetector> yoloDetectorFor(const std::filesystem::path& model);
