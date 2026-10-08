#include "labeler_export.hpp"
#include "labeler_assist.hpp"
#include "labeler_util.hpp"
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <stdexcept>

namespace boccia::labeler {
namespace fs = std::filesystem;
namespace {
void writeText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary); out << text; out.flush();
    if (!out) throw std::runtime_error("파일을 쓰지 못했습니다: " + utf8(path));
}
std::string encodePng(const cv::Mat& image) {
    std::vector<unsigned char> bytes;
    if (!cv::imencode(".png", image, bytes)) throw std::runtime_error("이미지를 PNG로 저장하지 못했습니다.");
    return std::string(bytes.begin(), bytes.end());
}
Json emptyCoco() {
    return {{"info", {{"description", "Boccia ball reviewed annotations"}, {"version", "1.0"}}}, {"licenses", Json::array()}, {"images", Json::array()}, {"annotations", Json::array()},
            {"categories", Json::array({{{"id", 1}, {"name", "boccia_ball"}, {"supercategory", "ball"}}})}};
}
}
Json exportDataset(const Catalog& catalog, const Json& snapshot, const fs::path& exportRoot, bool crops, const std::function<void(int)>& progress) {
    // Every export gets a new directory. No stale labels, overwrite, or split fallback.
    const fs::path output = exportRoot / (localStamp() + "-" + randomHex(4));
    fs::create_directories(exportRoot);
    if (!fs::create_directory(output)) throw std::runtime_error("내보내기 폴더가 이미 있습니다: " + utf8(output));
    const fs::path incomplete = output / "INCOMPLETE.txt";
    writeText(incomplete, "내보내기가 끝나지 않았습니다. manifest.json이 있는 완료된 폴더를 사용하세요.\n");
    Json coco = Json::object(), detectionCoco = Json::object();
    for (const char* split : splitNames) {
        coco[split] = emptyCoco(); detectionCoco[split] = emptyCoco();
        fs::create_directories(output / "images" / split); fs::create_directories(output / "labels" / split);
        if (crops) fs::create_directories(output / "crops" / split);
    }
    fs::create_directories(output / "annotations");
    Json records = Json::array(), cropRecords = Json::array();
    auto reviewed = selectedReviewed(snapshot);
    std::sort(reviewed.begin(), reviewed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    int annotationId = 1;
    try {
        int imageId = 0;
        for (const auto& [key, item] : reviewed) {
            ++imageId;
            const std::string recording = item["recording"], camera = item["camera"];
            const int index = item["index"];
            const Json splitValue = snapshot["splits"].value(recording, Json());
            if (!splitValue.is_string() || std::find(std::begin(splitNames), std::end(splitNames), splitValue.get<std::string>()) == std::end(splitNames)) throw std::runtime_error("검수한 촬영의 분할이 지정되지 않았습니다.");
            const std::string split = splitValue;
            const cv::Mat image = catalog.read(recording, camera, index);
            const int width = image.cols, height = image.rows;
            if (width != item["width"].get<int>() || height != item["height"].get<int>()) throw std::runtime_error("라벨링 이후 원본 크기가 변경되었습니다.");
            const Json boxes = validateBoxes(item["boxes"], width, height);
            const std::string stem = std::format("{}_{}_{:06}", recording, camera, index), filename = stem + ".png";
            writeText(output / "images" / split / filename, encodePng(image));
            Json imageEntry = {{"id", imageId}, {"file_name", filename}, {"width", width}, {"height", height}, {"recording_id", recording}, {"camera_id", camera}, {"frame_index", index}};
            coco[split]["images"].push_back(imageEntry); detectionCoco[split]["images"].push_back(imageEntry);
            std::string yolo;
            for (const auto& box : boxes) {
                const Json& bbox = box["bbox"];
                Json annotation = {{"id", annotationId}, {"image_id", imageId}, {"category_id", 1}, {"bbox", bbox},
                                   {"area", bbox[2].get<double>() * bbox[3].get<double>()}, {"iscrowd", 0}, {"occluded", box["occluded"]}};
                if (box.contains("segmentation")) {
                    annotation["segmentation"] = box["segmentation"];
                    long long area = 0; const auto& counts = box["segmentation"]["counts"];
                    for (size_t i = 1; i < counts.size(); i += 2) area += counts[i].get<long long>();
                    annotation["area"] = area;
                }
                coco[split]["annotations"].push_back(annotation);
                const Json& detected = box.contains("detection_bbox") ? box["detection_bbox"] : bbox;
                const double x = detected[0], y = detected[1], w = detected[2], h = detected[3];
                Json detection = Json::object();
                for (const auto& [name, value] : annotation.items()) if (name != "segmentation") detection[name] = value;
                detection["bbox"] = detected; detection["area"] = w * h;
                if (box.contains("color")) detection["color"] = box["color"];
                detectionCoco[split]["annotations"].push_back(detection);
                yolo += std::format("0 {:.8f} {:.8f} {:.8f} {:.8f}\n", (x + w / 2) / width, (y + h / 2) / height, w / width, h / height);
                if (crops) {
                    const int left = int(std::floor(x)), top = int(std::floor(y)), right = std::min(width, int(std::ceil(x + w))), bottom = std::min(height, int(std::ceil(y + h)));
                    const std::string cropName = std::format("{}_{:06}.png", stem, annotationId), cropPath = "crops/" + split + "/" + cropName;
                    writeText(output / "crops" / split / cropName, encodePng(image(cv::Rect(left, top, right - left, bottom - top))));
                    cropRecords.push_back({{"file_name", cropPath}, {"source_image", "images/" + split + "/" + filename}, {"source_key", key}, {"bbox", detected},
                                           {"crop_bounds", {left, top, right, bottom}}, {"occluded", box["occluded"]}});
                }
                ++annotationId;
            }
            writeText(output / "labels" / split / (stem + ".txt"), yolo);
            const auto& info = catalog.frameInfo(recording, camera, index);
            records.push_back({{"key", key}, {"split", split}, {"file_name", "images/" + split + "/" + filename}, {"recording_path", catalog.recording(recording).relativePath},
                               {"camera", camera}, {"frame", index}, {"host_ns", info.hasHostNs ? std::to_string(info.hostNs) : std::to_string(index)}, {"boxes", boxes}});
            if (progress) progress(imageId);
        }
        Json counts = Json::object();
        for (const char* split : splitNames) {
            atomicJson(output / "annotations" / (std::string("instances_") + split + ".json"), coco[split], true);
            atomicJson(output / "annotations" / (std::string("instances_detection_") + split + ".json"), detectionCoco[split], true);
            counts[split] = {{"images", coco[split]["images"].size()}, {"boxes", coco[split]["annotations"].size()}};
        }
        writeText(output / "data.yaml", "path: " + Json(utf8(fs::path(output).generic_wstring())).dump() + "\ntrain: images/train\nval: images/val\ntest: images/test\nnames:\n  0: boccia_ball\n");
        Json selection = Json::object();
        const Json frameSelection = snapshot.value("frame_selection", Json{{"enabled", false}});
        for (const auto& [name, value] : frameSelection.items()) if (name != "frames") selection[name] = value;
        atomicJson(output / "manifest.json", {{"version", 1}, {"complete", true}, {"created_at", utcIso()}, {"source", utf8(catalog.source)},
                                              {"source_signature", catalog.signature}, {"project_revision", snapshot["revision"]}, {"frame_selection", selection},
                                              {"splits", snapshot["splits"]}, {"native_split", snapshot.value("native_split", Json())},
                                              {"counts", counts}, {"frames", records}, {"crops", cropRecords}}, true);
        fs::remove(incomplete);
        return {{"path", utf8(output)}, {"counts", counts}, {"images", records.size()}, {"boxes", annotationId - 1}};
    } catch (const std::exception& error) {
        writeText(incomplete, std::string("내보내기 실패: ") + error.what() + "\n이 폴더를 학습에 사용하지 마세요.\n");
        throw;
    }
}
Json startExport(Project& project, bool crops) {
    std::lock_guard guard(project.lock);
    if (project.job.value("status", "") == "running") throw std::runtime_error("이미 내보내는 중입니다.");
    Json snapshot = project.state;
    snapshot["frame_selection"] = project.selection.view(project.state["frames"]);
    const auto reviewed = selectedReviewed(snapshot);
    if (reviewed.empty()) throw std::runtime_error("검수 완료한 프레임이 없습니다. 공을 모두 표시한 뒤 검수 완료를 누르세요.");
    for (const auto& [key, item] : reviewed) {
        const Json split = snapshot["splits"].value(item["recording"].get<std::string>(), Json());
        if (!split.is_string() || std::find(std::begin(splitNames), std::end(splitNames), split.get<std::string>()) == std::end(splitNames))
            throw std::runtime_error("검수한 촬영의 학습 / 검증 / 시험 분할을 먼저 지정하세요.");
    }
    project.job = {{"status", "running"}, {"done", 0}, {"total", reviewed.size()}, {"revision", snapshot["revision"]}};
    if (project.exportWorker.joinable()) project.exportWorker.join();
    project.exportWorker = std::thread([&project, snapshot = std::move(snapshot), crops] {
        try {
            const Json result = exportDataset(project.catalog, snapshot, project.exportRoot, crops, [&project](int done) { std::lock_guard guard(project.lock); project.job["done"] = done; });
            std::lock_guard guard(project.lock); project.job["status"] = "complete"; project.job["result"] = result;
        } catch (const std::exception& error) {
            std::lock_guard guard(project.lock); project.job["status"] = "error"; project.job["error"] = error.what();
        }
    });
    return project.job;
}
}
