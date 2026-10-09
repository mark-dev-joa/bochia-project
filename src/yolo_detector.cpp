#include "yolo_detector.hpp"
#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <stdexcept>

namespace fs = std::filesystem;

struct YoloDetector::Impl {
    explicit Impl(const fs::path& model)
        : path(model), env(ORT_LOGGING_LEVEL_WARNING, "boccia-yolo"), options(makeOptions()), session(env, model.c_str(), options),
          memory(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) {
        Ort::AllocatorWithDefaultOptions allocator;
        inputName = session.GetInputNameAllocated(0, allocator).get();
        outputName = session.GetOutputNameAllocated(0, allocator).get();
        const auto shape = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 4 || shape[1] != 3) throw std::runtime_error("The model's input is not an image (1x3xHxW).");
        size = shape[2] > 0 ? int(shape[2]) : 1280;
        if (shape[3] > 0 && shape[3] != shape[2]) throw std::runtime_error("The model's input must be square.");
    }
    static Ort::SessionOptions makeOptions() {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(4);
        return options;
    }
    fs::path path;
    Ort::Env env;
    Ort::SessionOptions options;
    Ort::Session session;
    Ort::MemoryInfo memory;
    std::string inputName, outputName;
    int size = 1280;
};

YoloDetector::YoloDetector(const fs::path& model) : impl_(std::make_unique<Impl>(model)) {}
YoloDetector::~YoloDetector() = default;
int YoloDetector::inputSize() const { return impl_->size; }
const fs::path& YoloDetector::path() const { return impl_->path; }

namespace {
float iou(const YoloBox& a, const YoloBox& b) {
    const float w = std::max(0.f, std::min(a.x2, b.x2) - std::max(a.x1, b.x1)), h = std::max(0.f, std::min(a.y2, b.y2) - std::max(a.y1, b.y1));
    const float inter = w * h, uni = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    return uni > 0 ? inter / uni : 0;
}
}

std::vector<YoloBox> YoloDetector::detect(const cv::Mat& bgr, float confidence) const {
    if (bgr.empty() || bgr.type() != CV_8UC3) throw std::runtime_error("The detector needs an 8-bit BGR image.");
    const int size = impl_->size;
    // Letterbox as the training did: scaled to fit, centred, grey borders; the boxes come back through the same numbers.
    const double scale = double(size) / std::max(bgr.cols, bgr.rows);
    const int width = std::max(1, int(std::lround(bgr.cols * scale))), height = std::max(1, int(std::lround(bgr.rows * scale)));
    const int padX = (size - width) / 2, padY = (size - height) / 2;
    cv::Mat resized; cv::resize(bgr, resized, {width, height}, 0, 0, scale < 1 ? cv::INTER_AREA : cv::INTER_LINEAR);
    cv::Mat canvas(size, size, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(canvas(cv::Rect(padX, padY, width, height)));
    cv::Mat rgb; cv::cvtColor(canvas, rgb, cv::COLOR_BGR2RGB);
    std::vector<float> input(size_t(3) * size * size);
    for (int c = 0; c < 3; ++c) for (int y = 0; y < size; ++y) {
        const uchar* row = rgb.ptr<uchar>(y);
        float* out = input.data() + (size_t(c) * size + y) * size;
        for (int x = 0; x < size; ++x) out[x] = row[x * 3 + c] / 255.f;
    }
    const int64_t shape[4] = {1, 3, size, size};
    Ort::Value tensor = Ort::Value::CreateTensor<float>(impl_->memory, input.data(), input.size(), shape, 4);
    const char* inputs[] = {impl_->inputName.c_str()}; const char* outputs[] = {impl_->outputName.c_str()};
    auto results = impl_->session.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
    const auto info = results[0].GetTensorTypeAndShapeInfo();
    const auto dims = info.GetShape();
    const float* data = results[0].GetTensorData<float>();
    std::vector<YoloBox> boxes;
    auto toSource = [&](float x1, float y1, float x2, float y2, float conf, int label) {
        YoloBox box{float((x1 - padX) / scale), float((y1 - padY) / scale), float((x2 - padX) / scale), float((y2 - padY) / scale), conf, label};
        box.x1 = std::clamp(box.x1, 0.f, float(bgr.cols)); box.x2 = std::clamp(box.x2, 0.f, float(bgr.cols));
        box.y1 = std::clamp(box.y1, 0.f, float(bgr.rows)); box.y2 = std::clamp(box.y2, 0.f, float(bgr.rows));
        if (box.x2 - box.x1 >= 1 && box.y2 - box.y1 >= 1) boxes.push_back(box);
    };
    if (dims.size() == 3 && dims[2] == 6) {   // exported with the NMS inside: rows of x1, y1, x2, y2, confidence, class
        for (int64_t i = 0; i < dims[1]; ++i) {
            const float* row = data + i * 6;
            if (row[4] >= confidence) toSource(row[0], row[1], row[2], row[3], row[4], int(row[5]));
        }
    } else if (dims.size() == 3 && dims[1] >= 5) {   // the raw head: columns of cx, cy, w, h and one score per class; greedy NMS here
        const int64_t channels = dims[1], count = dims[2];
        std::vector<YoloBox> candidates;
        for (int64_t i = 0; i < count; ++i) {
            float best = 0; int label = 0;
            for (int64_t c = 4; c < channels; ++c) { const float s = data[c * count + i]; if (s > best) { best = s; label = int(c - 4); } }
            if (best < confidence) continue;
            const float cx = data[i], cy = data[count + i], w = data[2 * count + i], h = data[3 * count + i];
            candidates.push_back({cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, best, label});
        }
        std::sort(candidates.begin(), candidates.end(), [](const YoloBox& a, const YoloBox& b) { return a.confidence > b.confidence; });
        std::vector<YoloBox> kept;
        for (const auto& c : candidates) { bool covered = false; for (const auto& k : kept) if (iou(c, k) > .5f) { covered = true; break; } if (!covered) kept.push_back(c); }
        for (const auto& k : kept) toSource(k.x1, k.y1, k.x2, k.y2, k.confidence, k.label);
    } else throw std::runtime_error("The model's output has an unexpected shape.");
    return boxes;
}

std::shared_ptr<const YoloDetector> yoloDetectorFor(const fs::path& model) {
    static std::mutex lock; static std::map<std::wstring, std::shared_ptr<const YoloDetector>> detectors;
    std::lock_guard guard(lock);
    const auto key = fs::absolute(model).native();
    auto found = detectors.find(key);
    if (found != detectors.end()) return found->second;
    if (!fs::is_regular_file(model)) throw std::runtime_error("The detector model file is missing: " + model.string());
    auto detector = std::make_shared<const YoloDetector>(model);
    detectors[key] = detector;
    return detector;
}
