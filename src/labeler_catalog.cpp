#include "labeler_catalog.hpp"
#include "labeler_util.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <windows.h>
#include <zstd.h>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace boccia::labeler {
namespace fs = std::filesystem;
namespace {
std::string readBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("파일을 읽지 못했습니다: " + utf8(path));
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
std::string_view withoutBom(std::string_view text) { return text.substr(0, 3) == "\xEF\xBB\xBF" ? text.substr(3) : text; }
std::string lower(std::string text) { for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c))); return text; }
const char* imageExtensions[] = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff"};
// os.stat().st_mtime_ns on Windows: the FILETIME, moved to the Unix epoch, in nanoseconds.
int64_t modifiedNs(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) throw std::runtime_error("파일 정보를 읽지 못했습니다: " + utf8(path));
    const uint64_t ticks = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    return int64_t(ticks - 116444736000000000ull) * 100;
}
}
FrameInfo FrameInfo::parse(const Json& line) {
    FrameInfo info;
    info.width = line.value("width", 0); info.height = line.value("height", 0); info.paddingX = line.value("padding_x", 0);
    info.pixelFormat = line.value("pixel_format", 0u); info.status = line.value("status", 0);
    info.codec = line.value("codec", 0); info.filterStride = line.value("filter_stride", 0);
    info.size = line.value("size", 0ull); info.storedSize = line.value("stored_size", info.size); info.offset = line.value("offset", 0ull);
    if (line.contains("host_ns") && line["host_ns"].is_number()) { info.hostNs = line["host_ns"].get<int64_t>(); info.hasHostNs = true; }
    if (line.contains("id") && line["id"].is_number()) { info.id = line["id"].get<uint64_t>(); info.hasId = true; }
    return info;
}
cv::Mat decodeRaw(const fs::path& raw, const FrameInfo& info) {
    const int width = info.width, height = info.height;
    if (!(0 < width && width <= 20000 && 0 < height && height <= 20000) || info.status != 0) throw std::runtime_error("유효하지 않은 원본 프레임입니다.");
    const int codec = info.codec, predictor = info.filterStride;
    const uint64_t size = info.size, storedSize = info.storedSize;
    if ((codec != 0 && codec != 1) || predictor < 0 || predictor > 8 || size == 0 || storedSize == 0) throw std::runtime_error("지원하지 않는 원본 압축 형식입니다.");
    if (codec == 0 && (predictor || storedSize != size)) throw std::runtime_error("압축하지 않은 원본의 크기가 맞지 않습니다.");
    std::string blob(size_t(storedSize), '\0');
    {
        std::ifstream in(raw, std::ios::binary);
        if (in) { in.seekg(std::streamoff(info.offset)); in.read(blob.data(), std::streamsize(blob.size())); }
        if (!in || size_t(in.gcount()) != blob.size()) throw std::runtime_error("원본 파일이 잘렸습니다.");
    }
    std::vector<uint8_t> pixels;
    if (codec == 1) {
        pixels.resize(size_t(size));
        const auto count = ZSTD_decompress(pixels.data(), pixels.size(), blob.data(), blob.size());
        if (ZSTD_isError(count) || count != size) throw std::runtime_error("복원한 원본의 크기가 맞지 않습니다.");
    } else pixels.assign(blob.begin(), blob.end());
    if (pixels.size() != size) throw std::runtime_error("복원한 원본의 크기가 맞지 않습니다.");
    if (predictor) for (size_t n = size_t(predictor); n < pixels.size(); ++n) pixels[n] = uint8_t(pixels[n] + pixels[n - size_t(predictor)]);
    const uint32_t format = info.pixelFormat;
    const int channels = (format == 0x02180014 || format == 0x02180015) ? 3 : 1;
    const size_t rowSize = size_t(width) * channels + size_t(std::max(0, info.paddingX));
    if (info.paddingX < 0 || pixels.size() < rowSize * height) throw std::runtime_error("원본 영상의 행 크기가 맞지 않습니다.");
    cv::Mat packed(height, int(rowSize), CV_8UC1, pixels.data());
    cv::Mat image;
    if (channels == 3) {
        cv::Mat rows = packed(cv::Rect(0, 0, width * 3, height));
        image = rows.reshape(3, height).clone();
        if (format == 0x02180014) cv::cvtColor(image, image, cv::COLOR_RGB2BGR);   // RGB8 -> BGR
    } else {
        cv::Mat plane = packed(cv::Rect(0, 0, width, height));
        if (format == 0x01080001) cv::cvtColor(plane, image, cv::COLOR_GRAY2BGR);          // Mono8: three identical channels
        else if (format == 0x01080009) cv::cvtColor(plane, image, cv::COLOR_BayerRGGB2BGR);   // physical RGGB mosaic (BayerRG8)
        else throw std::runtime_error("지원하지 않는 픽셀 형식: " + std::to_string(format));
    }
    return image;
}
const CameraStream* Recording::camera(const std::string& cameraId) const {
    for (const auto& stream : cameras) if (stream.id == cameraId) return &stream;
    return nullptr;
}
Catalog::Catalog(const fs::path& folder) {
    std::error_code error;
    source = fs::weakly_canonical(folder, error).make_preferred();
    if (error || !fs::is_directory(source)) throw std::runtime_error("촬영 폴더가 없습니다: " + utf8(source));
    Sha256 digest;
    digest.update(utf8(source));
    std::vector<fs::path> sessions;
    for (const auto& entry : fs::recursive_directory_iterator(source, fs::directory_options::skip_permission_denied))
        if (entry.is_regular_file() && lower(entry.path().filename().string()) == "session.json") sessions.push_back(entry.path());
    std::sort(sessions.begin(), sessions.end(), pathLess);
    for (const auto& sessionPath : sessions) {
        const Json session = Json::parse(withoutBom(readBytes(sessionPath)));
        const fs::path sessionFolder = sessionPath.parent_path();
        std::string relative = sessionFolder.lexically_relative(source).generic_string();
        if (relative.empty() || relative == ".") relative = ".";
        Recording recording;
        recording.id = sha256Hex(relative).substr(0, 16);
        recording.name = utf8(sessionFolder.filename());
        recording.relativePath = relative;
        recording.complete = session.value("complete", false);
        std::vector<fs::path> indexes;
        for (const auto& entry : fs::directory_iterator(sessionFolder)) {
            if (!entry.is_directory()) continue;
            const auto name = entry.path().filename().string();
            if (name.rfind("camera-", 0) == 0 && fs::is_regular_file(entry.path() / "frames.jsonl")) indexes.push_back(entry.path() / "frames.jsonl");
        }
        std::sort(indexes.begin(), indexes.end(), pathLess);
        const Json descriptions = session.value("cameras", Json::array());
        for (const auto& indexPath : indexes) {
            const fs::path rawPath = indexPath.parent_path() / "frames.raw";
            if (!fs::is_regular_file(rawPath)) continue;
            const std::string indexBytes = readBytes(indexPath);
            CameraStream stream;
            std::string_view text = withoutBom(indexBytes);
            for (size_t start = 0; start <= text.size();) {
                size_t end = text.find('\n', start); if (end == std::string_view::npos) end = text.size();
                std::string_view line = text.substr(start, end - start);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
                if (!line.empty()) stream.frames.push_back(FrameInfo::parse(Json::parse(line)));
                start = end + 1;
            }
            if (stream.frames.empty()) continue;
            stream.id = indexPath.parent_path().filename().string();
            const int cameraNumber = std::stoi(stream.id.substr(stream.id.rfind('-') + 1));
            const Json description = cameraNumber >= 0 && size_t(cameraNumber) < descriptions.size() && descriptions[size_t(cameraNumber)].is_object() ? descriptions[size_t(cameraNumber)] : Json::object();
            stream.name = description.value("name", stream.id);
            if (description.contains("serial") && description["serial"].is_string()) { stream.serial = description["serial"]; stream.hasSerial = true; }
            stream.kind = stream.frames.front().pixelFormat == 0x01080001 ? "모노" : "컬러";
            stream.rawPath = rawPath;
            for (size_t i = 0; i < stream.frames.size(); ++i) stream.times.push_back(stream.frames[i].hasHostNs ? stream.frames[i].hostNs : int64_t(i));
            digest.update(relative); digest.update(stream.id); digest.update(indexBytes);
            digest.update(std::to_string(fs::file_size(rawPath)));
            recording.cameras.push_back(std::move(stream));
        }
        if (!recording.cameras.empty()) recordings.push_back(std::move(recording));
    }
    // A plain image folder is also useful for a separate day's evaluation set.
    if (sessions.empty()) {
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(source, fs::directory_options::skip_permission_denied)) files.push_back(entry.path());
        std::sort(files.begin(), files.end(), pathLess);
        std::vector<std::pair<fs::path, std::vector<fs::path>>> byFolder;
        for (const auto& path : files) {
            if (!fs::is_regular_file(path)) continue;
            const auto extension = lower(path.extension().string());
            if (std::find(std::begin(imageExtensions), std::end(imageExtensions), extension) == std::end(imageExtensions)) continue;
            auto folder = std::find_if(byFolder.begin(), byFolder.end(), [&](const auto& item) { return item.first == path.parent_path(); });
            if (folder == byFolder.end()) { byFolder.push_back({path.parent_path(), {}}); folder = byFolder.end() - 1; }
            folder->second.push_back(path);
        }
        for (const auto& [imageFolder, paths] : byFolder) {
            std::string relative = imageFolder.lexically_relative(source).generic_string();
            if (relative.empty() || relative == ".") relative = ".";
            Recording recording;
            recording.id = sha256Hex(relative).substr(0, 16); recording.name = utf8(imageFolder.filename()); recording.relativePath = relative; recording.complete = true;
            CameraStream stream; stream.id = "images"; stream.name = "이미지"; stream.kind = "이미지";
            for (const auto& path : paths) {
                FrameInfo info; info.imagePath = path; stream.frames.push_back(info); stream.times.push_back(int64_t(stream.times.size()));
                digest.update(utf8(path.lexically_relative(source).make_preferred()) + ":" + std::to_string(fs::file_size(path)) + ":" + std::to_string(modifiedNs(path)));
            }
            recording.cameras.push_back(std::move(stream));
            recordings.push_back(std::move(recording));
        }
    }
    if (recordings.empty()) throw std::runtime_error("session.json 촬영 데이터 또는 이미지 파일을 찾지 못했습니다.");
    signature = digest.hex();
}
const Recording* Catalog::find(const std::string& id) const {
    for (const auto& recording : recordings) if (recording.id == id) return &recording;
    return nullptr;
}
const Recording& Catalog::recording(const std::string& id) const {
    if (const auto* found = find(id)) return *found;
    throw std::runtime_error("촬영 또는 카메라를 찾지 못했습니다.");
}
const CameraStream& Catalog::camera(const std::string& recordingId, const std::string& cameraId) const {
    if (const auto* found = find(recordingId)) if (const auto* stream = found->camera(cameraId)) return *stream;
    throw std::runtime_error("촬영 또는 카메라를 찾지 못했습니다.");
}
const FrameInfo& Catalog::frameInfo(const std::string& recordingId, const std::string& cameraId, int index) const {
    const auto& frames = camera(recordingId, cameraId).frames;
    if (index < 0 || size_t(index) >= frames.size()) throw std::runtime_error("프레임 번호가 범위를 벗어났습니다.");
    return frames[size_t(index)];
}
cv::Mat Catalog::read(const std::string& recordingId, const std::string& cameraId, int index) const {
    const auto& stream = camera(recordingId, cameraId);
    const auto& info = frameInfo(recordingId, cameraId, index);
    if (info.isImage()) {
        const std::string bytes = readBytes(info.imagePath);
        cv::Mat image = cv::imdecode(cv::Mat(1, int(bytes.size()), CV_8UC1, const_cast<char*>(bytes.data())), cv::IMREAD_COLOR);
        if (image.empty()) throw std::runtime_error("이미지 파일을 읽지 못했습니다.");
        return image;
    }
    return decodeRaw(stream.rawPath, info);
}
int Catalog::nearest(const std::string& recordingId, const std::string& cameraId, int64_t hostNs) const {
    const auto& times = camera(recordingId, cameraId).times;
    const auto position = std::lower_bound(times.begin(), times.end(), hostNs) - times.begin();
    const size_t right = std::min(size_t(position), times.size() - 1), left = right > 0 ? right - 1 : 0;
    auto distance = [&](size_t at) { const auto delta = times[at] - hostNs; return delta < 0 ? -delta : delta; };
    return int(distance(left) <= distance(right) ? left : right);
}
Json Catalog::metadata() const {
    Json result = Json::array();
    for (const auto& recording : recordings) {
        Json cameras = Json::array();
        for (const auto& stream : recording.cameras) cameras.push_back({{"id", stream.id}, {"name", stream.name}, {"kind", stream.kind}, {"count", stream.frames.size()}});
        result.push_back({{"id", recording.id}, {"name", recording.name}, {"relative_path", recording.relativePath}, {"complete", recording.complete}, {"cameras", cameras}});
    }
    return result;
}
int Catalog::totalFrames() const {
    int total = 0;
    for (const auto& recording : recordings) for (const auto& stream : recording.cameras) total += int(stream.frames.size());
    return total;
}
}
