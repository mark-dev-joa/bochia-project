#include "labeler_sam.hpp"
#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>
#include <stdexcept>

namespace boccia::labeler {
namespace fs = std::filesystem;
namespace {
class OrtBackend : public SamBackend {
public:
    explicit OrtBackend(const fs::path& modelDir)
        : env_(ORT_LOGGING_LEVEL_WARNING, "boccia-labeler"), options_(makeOptions()),
          encoder_(env_, (modelDir / "efficientsam_ti_encoder.onnx").c_str(), options_),
          decoder_(env_, (modelDir / "efficientsam_ti_decoder.onnx").c_str(), options_),
          memory_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) {}
    std::vector<float> encode(const cv::Mat& bgrCrop) override {
        cv::Mat rgb; cv::cvtColor(bgrCrop, rgb, cv::COLOR_BGR2RGB);
        const int rows = rgb.rows, cols = rgb.cols;
        std::vector<float> input(size_t(3) * rows * cols);
        for (int y = 0; y < rows; ++y) for (int x = 0; x < cols; ++x) {
            const auto pixel = rgb.at<cv::Vec3b>(y, x);
            for (int c = 0; c < 3; ++c) input[size_t(c) * rows * cols + size_t(y) * cols + x] = pixel[c] / 255.f;
        }
        const int64_t shape[] = {1, 3, rows, cols};
        Ort::Value tensor = Ort::Value::CreateTensor<float>(memory_, input.data(), input.size(), shape, 4);
        const char* inputs[] = {"batched_images"}; const char* outputs[] = {"image_embeddings"};
        auto results = encoder_.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        const float* data = results[0].GetTensorData<float>();
        return std::vector<float>(data, data + results[0].GetTensorTypeAndShapeInfo().GetElementCount());
    }
    SamOutput decode(const std::vector<float>& embeddings, const std::vector<cv::Point2f>& coords, const std::vector<float>& labels, cv::Size cropSize) override {
        std::vector<float> points(coords.size() * 2), pointLabels(labels);
        for (size_t i = 0; i < coords.size(); ++i) { points[2 * i] = coords[i].x; points[2 * i + 1] = coords[i].y; }
        std::vector<float> embedding(embeddings);
        int64_t size[] = {cropSize.height, cropSize.width};
        const int64_t embeddingShape[] = {1, 256, 64, 64}, pointShape[] = {1, 1, int64_t(coords.size()), 2}, labelShape[] = {1, 1, int64_t(labels.size())}, sizeShape[] = {2};
        Ort::Value inputs[] = {
            Ort::Value::CreateTensor<float>(memory_, embedding.data(), embedding.size(), embeddingShape, 4),
            Ort::Value::CreateTensor<float>(memory_, points.data(), points.size(), pointShape, 4),
            Ort::Value::CreateTensor<float>(memory_, pointLabels.data(), pointLabels.size(), labelShape, 3),
            Ort::Value::CreateTensor<int64_t>(memory_, size, 2, sizeShape, 1)};
        const char* names[] = {"image_embeddings", "batched_point_coords", "batched_point_labels", "orig_im_size"};
        const char* outputs[] = {"output_masks", "iou_predictions"};
        auto results = decoder_.Run(Ort::RunOptions{nullptr}, names, inputs, 4, outputs, 2);
        const auto maskShape = results[0].GetTensorTypeAndShapeInfo().GetShape();   // 1 x 1 x 3 x h x w
        if (maskShape.size() != 5) throw std::runtime_error("분할 모델의 출력 형태가 다릅니다.");
        const int count = int(maskShape[2]), rows = int(maskShape[3]), cols = int(maskShape[4]);
        const float* masks = results[0].GetTensorData<float>();
        const float* ious = results[1].GetTensorData<float>();
        SamOutput output;
        for (int i = 0; i < count; ++i) {
            output.logits.push_back(cv::Mat(rows, cols, CV_32F, const_cast<float*>(masks + size_t(i) * rows * cols)).clone());
            output.ious.push_back(ious[i]);
        }
        return output;
    }
private:
    static Ort::SessionOptions makeOptions() {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(4);
        options.SetInterOpNumThreads(1);
        return options;
    }
    Ort::Env env_;
    Ort::SessionOptions options_;
    Ort::Session encoder_, decoder_;
    Ort::MemoryInfo memory_;
};
}
bool samModelFilesExist(const fs::path& modelDir) {
    return fs::is_regular_file(modelDir / "efficientsam_ti_encoder.onnx") && fs::is_regular_file(modelDir / "efficientsam_ti_decoder.onnx");
}
std::unique_ptr<SamBackend> openSamModel(const fs::path& modelDir) {
    try { return std::make_unique<OrtBackend>(modelDir); }
    catch (const Ort::Exception& error) { throw std::runtime_error(std::string("분할 모델을 읽지 못했습니다. models/efficientsam 폴더의 파일을 다시 확인하세요. (") + error.what() + ")"); }
}
}
