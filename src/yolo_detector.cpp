#include "yolo_detector.hpp"
#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {
// The fp16 twin of a model file, <stem>-fp16<ext>, when it exists beside it.
fs::path fp16Sibling(const fs::path& model) {
    const fs::path twin = model.parent_path() / (model.stem().native() + fs::path("-fp16").native() + model.extension().native());
    return fs::is_regular_file(twin) ? twin : fs::path{};
}
}

struct YoloDetector::Impl {
    Impl(const fs::path& model, bool cuda)
        : path(model), env(ORT_LOGGING_LEVEL_WARNING, "boccia-yolo"), memory(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) {
        if (cuda) {
            // The CPU-only build of ONNX Runtime, a missing onnxruntime_providers_cuda.dll or missing CUDA/cuDNN DLLs all
            // surface here; the detector then runs on the CPU with the chosen (fp32) model and says why.
            const fs::path half = fp16Sibling(model);
            try {
                session = open(half.empty() ? model : half, true);
                onCuda = true; provider = half.empty() ? "CUDA" : "CUDA (fp16)"; if (!half.empty()) path = half;
            } catch (const std::exception& e) { provider = std::string("CPU (CUDA 사용 불가: ") + e.what() + ")"; }
        }
        if (!session) { session = open(model, false); if (provider.empty()) provider = "CPU"; }
        Ort::AllocatorWithDefaultOptions allocator;
        inputName = session->GetInputNameAllocated(0, allocator).get();
        outputName = session->GetOutputNameAllocated(0, allocator).get();
        const auto info = session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
        const auto shape = info.GetShape();
        if (shape.size() != 4) throw std::runtime_error("The model's input is not an image.");
        if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8) {   // bytes, [1,S,S,3] RGB (the -u8 variants)
            if (shape[3] != 3) throw std::runtime_error("The model's byte input is not an RGB image (1xHxWx3).");
            bytes = true; size = shape[1] > 0 ? int(shape[1]) : 1280;
            if (shape[2] > 0 && shape[2] != shape[1]) throw std::runtime_error("The model's input must be square.");
        } else if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {   // floats, [1,3,S,S]
            if (shape[1] != 3) throw std::runtime_error("The model's input is not an image (1x3xHxW).");
            size = shape[2] > 0 ? int(shape[2]) : 1280;
            if (shape[3] > 0 && shape[3] != shape[2]) throw std::runtime_error("The model's input must be square.");
        } else throw std::runtime_error("The model's input must be 8-bit or float.");
    }
    // 8 CPU threads (measured on an i7-13700K at 1280: 4 threads 62 ms, 8 threads 38 ms an image); CUDA device 0 when asked.
    std::unique_ptr<Ort::Session> open(const fs::path& model, bool cuda) {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(8);
        if (cuda) {
            // Device 0 with the provider's defaults; BOCCIA_CUDA_OPTIONS="key=value;key=value" (the CUDA provider's option
            // names, e.g. prefer_nhwc=1) overrides them for experiments.
            const OrtApi& api = Ort::GetApi();
            OrtCUDAProviderOptionsV2* raw = nullptr;
            Ort::ThrowOnError(api.CreateCUDAProviderOptions(&raw));
            std::unique_ptr<OrtCUDAProviderOptionsV2, decltype(api.ReleaseCUDAProviderOptions)> cudaOptions(raw, api.ReleaseCUDAProviderOptions);
            std::vector<std::string> keys{"device_id"}, values{"0"};
            if (const char* extra = std::getenv("BOCCIA_CUDA_OPTIONS")) {
                std::string text(extra);
                for (size_t start = 0; start < text.size();) {
                    const size_t end = std::min(text.find(';', start), text.size()), equals = text.find('=', start);
                    if (equals < end) { keys.push_back(text.substr(start, equals - start)); values.push_back(text.substr(equals + 1, end - equals - 1)); }
                    start = end + 1;
                }
            }
            std::vector<const char*> keyPointers, valuePointers;
            for (size_t i = 0; i < keys.size(); ++i) { keyPointers.push_back(keys[i].c_str()); valuePointers.push_back(values[i].c_str()); }
            Ort::ThrowOnError(api.UpdateCUDAProviderOptions(cudaOptions.get(), keyPointers.data(), valuePointers.data(), keys.size()));
            options.AppendExecutionProvider_CUDA_V2(*cudaOptions);
        }
        if (const char* prefix = std::getenv("BOCCIA_ORT_PROFILE")) {   // per-node timings and providers, written as <prefix>_<time>.json when the session closes
            const std::string text(prefix); options.EnableProfiling(fs::path(text).c_str());
        }
        return std::make_unique<Ort::Session>(env, model.c_str(), options);
    }
    fs::path path;
    Ort::Env env;
    Ort::MemoryInfo memory;
    std::unique_ptr<Ort::Session> session;
    std::string inputName, outputName, provider;
    bool onCuda = false, bytes = false;
    int size = 1280;
};

YoloDetector::YoloDetector(const fs::path& model, bool cuda) : impl_(std::make_unique<Impl>(model, cuda)) {
    // The first CUDA run picks its convolution algorithms and takes seconds: done here rather than on the first frame.
    if (impl_->onCuda) detect(cv::Mat(impl_->size, impl_->size, CV_8UC3, cv::Scalar(114, 114, 114)), .5f);
}
YoloDetector::~YoloDetector() = default;
int YoloDetector::inputSize() const { return impl_->size; }
const fs::path& YoloDetector::path() const { return impl_->path; }
bool YoloDetector::onCuda() const { return impl_->onCuda; }
const std::string& YoloDetector::provider() const { return impl_->provider; }

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
    CV_Assert(rgb.isContinuous());
    std::vector<float> input;
    Ort::Value tensor{nullptr};
    if (impl_->bytes) {   // the bytes go as they are; the model casts and scales them
        const int64_t shape[4] = {1, size, size, 3};
        tensor = Ort::Value::CreateTensor<uint8_t>(impl_->memory, rgb.data, size_t(3) * size * size, shape, 4);
    } else {
        input.resize(size_t(3) * size * size);
        for (int c = 0; c < 3; ++c) for (int y = 0; y < size; ++y) {
            const uchar* row = rgb.ptr<uchar>(y);
            float* out = input.data() + (size_t(c) * size + y) * size;
            for (int x = 0; x < size; ++x) out[x] = row[x * 3 + c] / 255.f;
        }
        const int64_t shape[4] = {1, 3, size, size};
        tensor = Ort::Value::CreateTensor<float>(impl_->memory, input.data(), input.size(), shape, 4);
    }
    const char* inputs[] = {impl_->inputName.c_str()}; const char* outputs[] = {impl_->outputName.c_str()};
    auto results = impl_->session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
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

namespace {
std::mutex& registryLock() { static std::mutex lock; return lock; }
std::map<std::wstring, std::shared_ptr<const YoloDetector>>& registry() { static std::map<std::wstring, std::shared_ptr<const YoloDetector>> detectors; return detectors; }
std::wstring registryKey(const fs::path& model, bool cuda) { return fs::absolute(model).wstring() + (cuda ? L"|cuda" : L"|cpu"); }
}

std::shared_ptr<const YoloDetector> yoloDetectorFor(const fs::path& model, bool cuda) {
    std::lock_guard guard(registryLock());
    const auto key = registryKey(model, cuda);
    auto& detectors = registry();
    auto found = detectors.find(key);
    if (found != detectors.end()) return found->second;
    if (!fs::is_regular_file(model)) throw std::runtime_error("The detector model file is missing: " + model.string());
    auto detector = std::make_shared<const YoloDetector>(model, cuda);
    detectors[key] = detector;
    return detector;
}

std::shared_ptr<const YoloDetector> yoloDetectorIfLoaded(const fs::path& model, bool cuda) {
    std::lock_guard guard(registryLock());
    auto& detectors = registry();
    auto found = detectors.find(registryKey(model, cuda));
    return found != detectors.end() ? found->second : nullptr;
}

void yoloCudaLibraryDirectory(const fs::path& directory) {
#ifdef _WIN32
    static std::mutex lock; static std::wstring current;
    std::lock_guard guard(lock);
    const std::wstring wanted = directory.empty() ? std::wstring{} : fs::absolute(directory).wstring();
    if (wanted == current) return;
    current = wanted;
    SetDllDirectoryW(wanted.empty() ? nullptr : wanted.c_str());
    if (!wanted.empty()) {   // cuDNN loads its own pieces by name later, so the folder goes on PATH as well
        std::wstring path(32768, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"PATH", path.data(), DWORD(path.size()));
        path.resize(length < path.size() ? length : 0);
        if (path.find(wanted) == std::wstring::npos) SetEnvironmentVariableW(L"PATH", (wanted + L";" + path).c_str());
    }
#else
    (void)directory;
#endif
}
