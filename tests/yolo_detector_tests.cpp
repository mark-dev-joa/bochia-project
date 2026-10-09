// The learned ball detector through the calibration app's detection entry point: the model finds the balls of an image and
// the ROI and the confidence setting apply. Usage: yolo_detector_tests <model.onnx> <image> [expected balls]
#include "ball_detection.hpp"
#include "yolo_detector.hpp"
#include <onnxruntime_c_api.h>
#include <opencv2/imgcodecs.hpp>
#include <cstdlib>
#include <chrono>
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
        std::cout << "onnxruntime " << OrtGetApiBase()->GetVersionString() << " · model input " << detector->inputSize() << " · raw boxes at 0.25: " << detector->detect(image, .25f).size() << std::endl;
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
