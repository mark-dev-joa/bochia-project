#pragma once
// A ball detector learned from the labeler's labels (YOLO26, exported to ONNX with the NMS inside), run with ONNX Runtime
// on the GPU through CUDA when asked and available, otherwise on the CPU. One instance per model file and device; a run
// is safe from several threads at once.
#include <opencv2/core.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct YoloBox { float x1 = 0, y1 = 0, x2 = 0, y2 = 0, confidence = 0; int label = 0; };   // source pixels

class YoloDetector {
public:
    // With `cuda` the session is tried on the GPU first and falls back to the CPU, saying why in provider().
    YoloDetector(const std::filesystem::path& model, bool cuda);
    ~YoloDetector();
    YoloDetector(const YoloDetector&) = delete;
    YoloDetector& operator=(const YoloDetector&) = delete;
    // Boxes with at least `confidence`, in the source image's pixels. The image is letterboxed to the model's input size.
    std::vector<YoloBox> detect(const cv::Mat& bgr, float confidence) const;
    int inputSize() const;
    const std::filesystem::path& path() const;
    bool onCuda() const;                  // whether the session runs on the GPU
    const std::string& provider() const;  // "CUDA", "CPU", or "CPU (CUDA 사용 불가: ...)" when the GPU was asked for
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The detector for a model file on a device, made on first use and kept; the same one for every caller.
std::shared_ptr<const YoloDetector> yoloDetectorFor(const std::filesystem::path& model, bool cuda);
// The kept detector, or null when none has been made yet: for status displays, which must not load anything.
std::shared_ptr<const YoloDetector> yoloDetectorIfLoaded(const std::filesystem::path& model, bool cuda);
// A folder holding the CUDA runtime DLLs (cudart, cublas, cudnn, cufft, curand ...) when they are not next to the program:
// put on the DLL search path and the process PATH before the first CUDA session. Empty leaves the search path alone.
void yoloCudaLibraryDirectory(const std::filesystem::path& directory);
