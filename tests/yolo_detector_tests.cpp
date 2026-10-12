// The learned ball detector through the calibration app's detection entry point: the model finds the balls of an image and
// the ROI and the confidence setting apply. Usage: yolo_detector_tests <model.onnx> <image> [expected balls]
#include "ball_detection.hpp"
#include "yolo_detector.hpp"
#include <onnxruntime_c_api.h>
#include <opencv2/imgcodecs.hpp>
#include <cstdlib>
#include <chrono>
#include <opencv2/imgproc.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: yolo_detector_tests <model.onnx> <image> [expected balls]\n"; return 2; }
    try {
        const cv::Mat image = cv::imread(argv[2], cv::IMREAD_COLOR);
        if (image.empty()) throw std::runtime_error("cannot read the image");
        if (const char* dlls = std::getenv("BOCCIA_CUDA_DLLS")) yoloCudaLibraryDirectory(dlls);   // where this machine keeps the CUDA runtime DLLs
        const auto detector = yoloDetectorFor(argv[1], false);
        const auto gpu = yoloDetectorFor(argv[1], true);   // CUDA when the GPU build and its DLLs are present, else the CPU with the reason
        std::cout << "cuda session: " << gpu->provider() << " · boxes at 0.25: " << gpu->detect(image, .25f).size() << "\n";
        if (gpu->detect(image, .25f).size() != detector->detect(image, .25f).size()) throw std::runtime_error("The CUDA session and the CPU session disagree on the balls.");
        for (const auto& [name, session] : {std::pair{"cpu", detector}, std::pair{"cuda", gpu}}) {
            const auto started = std::chrono::steady_clock::now();
            for (int i = 0; i < 5; ++i) session->detect(image, .25f);
            std::cout << name << " session (" << session->provider() << "): " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / 5 << " ms per image\n";
        }
        {   // The preprocessing the detector does on the CPU before every run, timed on its own: letterbox, BGR to RGB, bytes to planar floats.
            const auto target = detector->inputShape();
            const double scale = std::min(double(target.width) / image.cols, double(target.height) / image.rows);
            const int width = int(std::lround(image.cols * scale)), height = int(std::lround(image.rows * scale));
            auto ms = [](auto f, int repeats = 5) { const auto t = std::chrono::steady_clock::now(); for (int i = 0; i < repeats; ++i) f(); return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count() / repeats; };
            cv::Mat resized, canvas(target, CV_8UC3, cv::Scalar(114, 114, 114)), rgb;
            const double letterbox = ms([&] { cv::resize(image, resized, {width, height}, 0, 0, cv::INTER_AREA); resized.copyTo(canvas(cv::Rect((target.width - width) / 2, (target.height - height) / 2, width, height))); cv::cvtColor(canvas, rgb, cv::COLOR_BGR2RGB); });
            std::vector<float> input(size_t(3) * target.width * target.height);
            const double scalar = ms([&] { for (int c = 0; c < 3; ++c) for (int y = 0; y < target.height; ++y) { const uchar* row = rgb.ptr<uchar>(y); float* out = input.data() + (size_t(c) * target.height + y) * target.width; for (int x = 0; x < target.width; ++x) out[x] = row[x * 3 + c] / 255.f; } });
            cv::Mat asFloat; std::vector<cv::Mat> planes;
            const double vectorised = ms([&] { rgb.convertTo(asFloat, CV_32FC3, 1 / 255.); for (int c = 0; c < 3; ++c) planes.push_back(cv::Mat(target, CV_32F, input.data() + size_t(c) * target.width * target.height)); cv::split(asFloat, planes); planes.clear(); });
            std::cout << "preprocessing: letterbox+colour " << letterbox << " ms, bytes to floats scalar loop " << scalar << " ms, same with convertTo+split " << vectorised << " ms\n";
        }
        std::cout << "onnxruntime " << OrtGetApiBase()->GetVersionString() << " · model input " << detector->inputShape() << " · raw boxes at 0.25: " << detector->detect(image, .25f).size() << std::endl;
        BallOptions options; options.model = argv[1]; options.modelConfidence = .25; options.color = true;
        const BallDetection detection = detectBalls(image, options);
        std::cout << detection.balls.size() << " balls in " << detection.milliseconds << " ms\n";
        for (const auto& ball : detection.balls) std::cout << "  centre " << ball.center.x << "," << ball.center.y << " r " << ball.radius << " conf " << ball.coverage << " colour " << ball.color << "\n";
        // A region of interest that excludes everything must leave no ball; the same with a confidence nothing reaches.
        BallOptions corner = options; corner.roiEnabled = true; corner.roi = {{0, 0}, {.01f, 0}, {.01f, .01f}, {0, .01f}};
        if (!detectBalls(image, corner).balls.empty()) throw std::runtime_error("the ROI did not exclude the balls");
        BallOptions strict = options; strict.modelConfidence = .99;
        if (detectBalls(image, strict).balls.size() > detection.balls.size()) throw std::runtime_error("a stricter confidence found more balls");
        if (argc > 3 && int(detection.balls.size()) != std::atoi(argv[3])) throw std::runtime_error("expected " + std::string(argv[3]) + " balls");
        std::cout << "PASS: the model finds the balls through detectBalls, with the ROI and the confidence applied\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << "\n"; return 1; }
}
