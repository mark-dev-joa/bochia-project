#pragma once
// The recordings under one source folder, read the way ball_labeler.py's Catalog reads them: the same
// recording ids, the same source signature and the same decoded pixels, without the camera SDK.
#include "native_labeler_model.hpp"
#include <cstdint>
#include <vector>

namespace boccia::labeler {
struct FrameInfo {
    int width = 0, height = 0, paddingX = 0, codec = 0, filterStride = 0, status = 0;
    uint32_t pixelFormat = 0;
    uint64_t size = 0, storedSize = 0, offset = 0;
    int64_t hostNs = 0;
    uint64_t id = 0;
    bool hasHostNs = false, hasId = false;
    std::filesystem::path imagePath;           // set for a plain image file instead of a raw frame
    bool isImage() const { return !imagePath.empty(); }
    static FrameInfo parse(const Json& line);   // one frames.jsonl entry; absent fields keep Python's defaults
};
struct CameraStream {
    std::string id, name, serial, kind;        // kind: "모노", "컬러" or "이미지"
    bool hasSerial = false;
    std::vector<FrameInfo> frames;
    std::vector<int64_t> times;                // host_ns, or the index when a frame has none
    std::filesystem::path rawPath;             // empty for an image folder
};
struct Recording {
    std::string id, name, relativePath;
    bool complete = false;
    std::vector<CameraStream> cameras;         // camera-0, camera-1, … in path order
    const CameraStream* camera(const std::string& id) const;
};
class Catalog {
public:
    explicit Catalog(const std::filesystem::path& source);
    std::filesystem::path source;              // the absolute, canonical folder
    std::string signature;
    std::vector<Recording> recordings;         // in Python's dict order: sorted session.json paths
    const Recording* find(const std::string& recording) const;
    const Recording& recording(const std::string& id) const;
    const CameraStream& camera(const std::string& recording, const std::string& camera) const;
    const FrameInfo& frameInfo(const std::string& recording, const std::string& camera, int index) const;
    cv::Mat read(const std::string& recording, const std::string& camera, int index) const;   // BGR, three channels
    int nearest(const std::string& recording, const std::string& camera, int64_t hostNs) const;
    Json metadata() const;
    int totalFrames() const;
};
cv::Mat decodeRaw(const std::filesystem::path& raw, const FrameInfo& info);
}
