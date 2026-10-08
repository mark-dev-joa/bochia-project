#pragma once
// EfficientSAM-Ti through ONNX Runtime: the image encoder and the prompt decoder as labeler_assist.py runs them.
#include <opencv2/core.hpp>
#include <filesystem>
#include <memory>
#include <vector>

namespace boccia::labeler {
struct SamOutput {
    std::vector<cv::Mat> logits;   // three CV_32F masks of the crop size
    std::vector<float> ious;       // the model's quality guess for each
};
// The two model runs, behind an interface so tests can stand in for the model.
class SamBackend {
public:
    virtual ~SamBackend() = default;
    virtual std::vector<float> encode(const cv::Mat& bgrCrop) = 0;   // image embeddings, 256 x 64 x 64
    virtual SamOutput decode(const std::vector<float>& embeddings, const std::vector<cv::Point2f>& coords, const std::vector<float>& labels, cv::Size cropSize) = 0;
};
bool samModelFilesExist(const std::filesystem::path& modelDir);
std::unique_ptr<SamBackend> openSamModel(const std::filesystem::path& modelDir);   // throws when the files cannot be loaded
}
