// Checks of the C++ labeler engine, one group per module. Each group mirrors the Python test of the same
// name, so a behaviour the two engines must share has the same check on both sides.
#include "labeler_catalog.hpp"
#include "labeler_assist.hpp"
#include "labeler_util.hpp"
#include <opencv2/imgproc.hpp>
#include <zstd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>

using namespace boccia::labeler;
namespace fs = std::filesystem;
namespace {
void check(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
template <class Call> bool fails(Call&& call, const std::string& expected = {}) {
    try { call(); return false; }
    catch (const std::exception& error) { if (!expected.empty() && std::string(error.what()).find(expected) == std::string::npos) throw std::runtime_error(std::string("wrong failure: ") + error.what()); return true; }
}
void writeFile(const fs::path& path, const std::string& bytes) { fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary); out << bytes; }
std::string readFile(const fs::path& path) { std::ifstream in(path, std::ios::binary); return std::string((std::istreambuf_iterator<char>(in)), {}); }
struct TempDir {
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / ("boccia-engine-" + randomHex(6))) { fs::create_directories(path); }
    ~TempDir() { std::error_code ignored; fs::remove_all(path, ignored); }
};
// The synthetic recordings of tests/test_ball_labeler.py: three sessions, two mono cameras, three 8x6 frames each,
// stored with a one-byte delta and zstd.
struct Fixture {
    TempDir temp;
    fs::path source, project, exports;
    std::vector<uint8_t> original;
    std::vector<std::string> ids;
    Fixture() : source(temp.path / "recordings"), project(temp.path / "project.json"), exports(temp.path / "exports"), original(48) {
        std::iota(original.begin(), original.end(), uint8_t(0));
        std::vector<uint8_t> predicted = original;
        for (size_t i = predicted.size() - 1; i > 0; --i) predicted[i] = uint8_t(predicted[i] - predicted[i - 1]);
        std::string compressed(ZSTD_compressBound(predicted.size()), '\0');
        compressed.resize(ZSTD_compress(compressed.data(), compressed.size(), predicted.data(), predicted.size(), 3));
        for (const char* session : {"capture-a", "capture-b", "capture-c"}) {
            const fs::path folder = source / session;
            writeFile(folder / "session.json", R"({"complete": true, "cameras": [{"name": "Mono A"}, {"name": "Mono B"}]})");
            for (const char* camera : {"camera-0", "camera-1"}) {
                std::string index;
                for (int frame = 0; frame < 3; ++frame) {
                    Json info = {{"width", 8}, {"height", 6}, {"padding_x", 0}, {"padding_y", 0}, {"pixel_format", 0x01080001}, {"size", 48},
                                 {"stored_size", compressed.size()}, {"offset", frame * compressed.size()}, {"codec", 1}, {"filter_stride", 1}, {"status", 0},
                                 {"host_ns", 1000 + frame * 100 + (std::string(camera) == "camera-1" ? 5 : 0)}};
                    index += info.dump() + (frame < 2 ? "\n" : "");
                }
                writeFile(folder / camera / "frames.raw", compressed + compressed + compressed);
                writeFile(folder / camera / "frames.jsonl", index);
            }
        }
        for (const char* session : {"capture-a", "capture-b", "capture-c"}) ids.push_back(sha256Hex(session).substr(0, 16));
    }
};

void utilTests() {
    check(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of abc wrong");
    Sha256 pieces; pieces.update("ab"); pieces.update("c");
    check(pieces.hex() == sha256Hex("abc"), "incremental sha256 differs from one-shot");
    check(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of empty input wrong");
    const auto name = randomHex(16);
    check(name.size() == 32 && name != randomHex(16) && name.find_first_not_of("0123456789abcdef") == std::string::npos, "random hex name malformed");
    const auto stamp = utcIso();
    check(stamp.size() == 32 && stamp[10] == 'T' && stamp.substr(26) == "+00:00" && stamp[19] == '.', "utc timestamp format wrong: " + stamp);
    check(localStamp().size() == 15 && localStamp()[8] == '-', "local stamp format wrong");
    TempDir temp;
    const auto file = temp.path / "nested" / "value.json";
    atomicJson(file, Json{{"b", 1}, {"a", Json::array({1, 2})}}, true);
    std::string text = readFile(file);   // the stream is closed again before the next replace
    check(text == "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": 1\n}\n", "indented json file wrong: " + text);
    atomicJson(file, Json{{"b", 1}}, false);
    text = readFile(file);
    check(text == "{\"b\":1}\n", "compact json file wrong: " + text);
    int entries = 0; for (const auto& entry : fs::directory_iterator(file.parent_path())) { (void)entry; ++entries; }
    check(entries == 1, "temporary file left beside the json");
    check(pathLess("a/b", "a-b/c") && !pathLess("a-b/c", "a/b"), "path order must compare pieces, not characters");
    check(pathLess("D:/x/Capture-2/session.json", "d:/x/capture-10/session.json") == (std::wstring(L"capture-2") < std::wstring(L"capture-10")), "path order must ignore case");
    check(!pathLess("a/b", "a/b"), "equal paths are not less");
    std::cout << "PASS: sha256, random names, timestamps, atomic json and python path order\n";
}

void catalogTests() {
    Fixture fixture;
    Catalog catalog(fixture.source);
    check(catalog.recordings.size() == 3 && catalog.recordings[0].id == "15d25a1a92958366" && catalog.recordings[0].name == "capture-a", "recording ids or order differ from Python");
    check(catalog.recordings[0].id == fixture.ids[0] && catalog.recordings[2].id == fixture.ids[2], "recording id is not the hash of the relative path");
    // test_lossless_compressed_mono_and_nearest_camera
    cv::Mat image = catalog.read(fixture.ids[0], "camera-0", 2);
    check(image.rows == 6 && image.cols == 8 && image.channels() == 3, "decoded mono frame has the wrong shape");
    for (int y = 0; y < 6; ++y) for (int x = 0; x < 8; ++x) for (int c = 0; c < 3; ++c) check(image.at<cv::Vec3b>(y, x)[c] == fixture.original[size_t(y * 8 + x)], "mono pixels differ from the original");
    check(catalog.nearest(fixture.ids[0], "camera-1", 1198) == 2 && catalog.nearest(fixture.ids[0], "camera-1", 0) == 0 && catalog.nearest(fixture.ids[0], "camera-1", 9999) == 2, "nearest frame wrong");
    const auto& stream = catalog.camera(fixture.ids[0], "camera-1");
    check(stream.kind == "모노" && stream.name == "Mono B" && stream.times == std::vector<int64_t>{1005, 1105, 1205} && stream.frames.size() == 3, "camera stream fields wrong");
    const auto metadata = catalog.metadata();
    check(metadata.size() == 3 && metadata[0]["cameras"].size() == 2 && metadata[0]["cameras"][0]["count"] == 3 && metadata[0]["cameras"][1]["name"] == "Mono B" && metadata[0]["complete"] == true && metadata[0]["relative_path"] == "capture-a", "metadata wrong");
    check(catalog.totalFrames() == 18, "total frame count wrong");
    check(fails([&] { catalog.frameInfo(fixture.ids[0], "camera-0", 3); }, "프레임 번호") && fails([&] { catalog.camera(fixture.ids[0], "camera-9"); }, "카메라를 찾지"), "range errors wrong");
    // test_bayer_channel_order_padding_and_truncation
    cv::Mat mosaic(12, 14, CV_8UC1, cv::Scalar(0));
    mosaic.colRange(12, 14).setTo(99);
    for (int y = 0; y < 12; ++y) for (int x = 0; x < 12; ++x) mosaic.at<uchar>(y, x) = uchar(y % 2 == 0 ? (x % 2 == 0 ? 240 : 120) : (x % 2 == 0 ? 120 : 15));
    const fs::path bayer = fixture.temp.path / "bayer.raw";
    writeFile(bayer, std::string(reinterpret_cast<const char*>(mosaic.data), size_t(mosaic.total())));
    FrameInfo info; info.width = 12; info.height = 12; info.paddingX = 2; info.pixelFormat = 0x01080009; info.size = info.storedSize = 168;
    cv::Mat colour = decodeRaw(bayer, info);
    check(colour.at<cv::Vec3b>(6, 6) == cv::Vec3b(15, 120, 240), "bayer channel order wrong");
    writeFile(bayer, "short");
    check(fails([&] { decodeRaw(bayer, info); }, "잘렸습니다"), "truncated raw must fail");
    FrameInfo odd = info; odd.pixelFormat = 0x01080002;
    writeFile(bayer, std::string(168, 'a'));
    check(fails([&] { decodeRaw(bayer, odd); }, "지원하지 않는 픽셀 형식"), "unknown pixel format must fail");
    // test_source_change_rejected: the signature covers the raw sizes and the index bytes
    const fs::path raw = fixture.source / "capture-a/camera-0/frames.raw";
    writeFile(raw, readFile(raw) + "extra");
    Catalog changed(fixture.source);
    check(changed.signature != catalog.signature && changed.recordings[0].id == catalog.recordings[0].id, "signature must follow the raw files, ids must not");
    TempDir empty;
    check(fails([&] { Catalog(empty.path); }, "찾지 못했습니다"), "an empty folder must fail");
    check(fails([&] { Catalog(empty.path / "missing"); }, "촬영 폴더가 없습니다"), "a missing folder must fail");
    std::cout << "PASS: catalog ids, signature, mono and bayer decoding, nearest frame and errors\n";
}

void assistTests() {
    // test_mask_persistence_export_and_bbox_agree: the RLE checks
    cv::Mat mask(6, 8, CV_8UC1, cv::Scalar(0));
    mask(cv::Rect(2, 1, 3, 4)).setTo(1); mask.at<uchar>(2, 3) = 0;   // a hole must survive RLE, unlike a filled polygon
    const Json rle = encodeMask(mask);
    check(cv::countNonZero(decodeMaskChecked(rle, 8, 6) != mask) == 0, "RLE round trip lost the hole");
    for (const Json& bad : {Json{{"size", {8, 6}}, {"counts", rle["counts"]}}, Json{{"size", {6, 8}}, {"counts", {47}}}, Json{{"size", {6, 8}}, {"counts", {48}}}, Json{{"size", {6, 8}}, {"counts", {true, 47}}}})
        check(fails([&] { decodeMaskChecked(bad, 8, 6); }, "분할 마스크"), "invalid RLE accepted: " + bad.dump());
    cv::Mat filled(6, 8, CV_8UC1, cv::Scalar(1));
    check(encodeMask(filled)["counts"] == Json::array({0, 48}) && cv::countNonZero(decodeMaskChecked(encodeMask(filled), 8, 6) != filled) == 0, "a full mask must start with a zero run");
    check(maskContours(mask).size() == 2 && maskContours(mask)[0].size() >= 4, "contours of the block and its hole wrong");   // the one-pixel hole has its own contour, as in Python
    check(std::abs(overlap({0, 0, 2, 2}, {1, 1, 2, 2}) - 1.0 / 7) < 1e-9 && overlap({0, 0, 2, 2}, {5, 5, 1, 1}) == 0, "overlap wrong");
    // test_manual_polygon_mask_and_rejected_coordinates
    const Json proposal = polygonProposal(Json::parse("[[2,1],[4,1],[4,4],[2,4]]"), 8, 6);
    check(proposal["bbox"] == Json::array({2, 1, 3, 4}) && proposal["source"] == "manual_polygon" && proposal["occluded"] == false, "polygon proposal wrong");
    check(cv::countNonZero(decodeMaskChecked(proposal["segmentation"], 8, 6)) == 12 && !proposal["contours"].empty(), "polygon mask wrong");
    for (const char* points : {"[[0,0],[1,1]]", "[[0,0],[1,1],[2,2]]", "[[-1,0],[4,1],[4,4]]"})
        check(fails([&] { polygonProposal(Json::parse(points), 8, 6); }), std::string("polygon accepted: ") + points);
    // test_invalid_coordinates_and_missing_split_rejected: the coordinate part
    for (const Json& bbox : {Json{-1, 0, 2, 2}, Json{7, 0, 3, 2}, Json{0, 0, 0, 2}, Json{std::nan(""), 0, 2, 2}, Json{true, 0, 2, 2}})
        check(fails([&] { validateBoxes(Json::array({{{"bbox", bbox}}}), 8, 6); }, "박스"), "invalid box accepted: " + bbox.dump());
    check(fails([&] { validateBoxes(Json::array({{{"bbox", {2, 1, 3, 4}}, {"occluded", 1}}}), 8, 6); }, "가림") && fails([&] { validateBoxes(Json::array({{{"bbox", {2, 1, 3, 4}}, {"color", "green"}}}), 8, 6); }, "색상"), "box attribute checks wrong");
    Json boxes = validateBoxes(Json::parse(R"([{"bbox":[2.123456,1,3,4],"occluded":true,"color":"red","source":"efficientsam","detection_bbox":[1,0,5,5],"extra":1},{"bbox":[0,0,2,2],"color":"unknown","source":"web"}])"), 8, 6);
    check(boxes.size() == 2 && boxes[0]["bbox"][0] == 2.1235 && boxes[0]["occluded"] == true && boxes[0]["color"] == "red" && boxes[0]["source"] == "efficientsam" && boxes[0]["detection_bbox"] == Json::array({1.0, 0.0, 5.0, 5.0}) && !boxes[0].contains("extra"), "validated box wrong: " + boxes[0].dump());
    check(!boxes[1].contains("color") && !boxes[1].contains("source") && boxes[1]["bbox"][2].is_number_float(), "unknown colour and foreign source must be dropped");
    Json masked = Json::array({{{"bbox", {2, 1, 3, 4}}, {"segmentation", rle}}});
    check(validateBoxes(masked, 8, 6)[0]["segmentation"] == rle, "a matching mask must be kept");
    masked[0]["bbox"] = {1, 1, 3, 4};
    check(fails([&] { validateBoxes(masked, 8, 6); }, "분할 윤곽과 박스 좌표가 다릅니다"), "a box that left its mask was accepted");
    check(fails([&] { validateBoxes(Json(std::vector<Json>(501, Json{{"bbox", {0, 0, 1, 1}}})), 8, 6); }, "목록"), "501 boxes accepted");
    // test_ball_color_from_the_colour_camera
    cv::Mat disc(20, 20, CV_8UC1, cv::Scalar(0)); cv::circle(disc, {10, 10}, 8, cv::Scalar(1), -1);
    auto patchOf = [&](cv::Vec3b bgr) { cv::Mat image(20, 20, CV_8UC3, cv::Scalar(30, 40, 50)); image.setTo(cv::Scalar(bgr[0], bgr[1], bgr[2]), disc); return image; };
    check(ballColor(patchOf({8, 12, 32}), disc) == "red" && ballColor(patchOf({42, 58, 68}), disc) == "white" && ballColor(patchOf({10, 11, 10}), disc) == "black" && ballColor(patchOf({60, 22, 10}), disc) == "blue", "ball colours wrong");
    check(ballColor(cv::Mat(20, 20, CV_8UC1, cv::Scalar(0)), disc) == "unknown", "a mono image must have no colour");
    check(boundedNumber(3, 0, 10, "x") == 3 && fails([&] { boundedNumber(Json(true), 0, 10, "값"); }, "값: 0~10 사이") && fails([&] { boundedNumber(11, 0, 10, "값"); }), "bounded number wrong");
    std::cout << "PASS: RLE checks, contours, overlap, polygon proposal, box validation and ball colour\n";
}
}

int main(int argc, char** argv) try {
    if (argc >= 3 && std::string(argv[1]) == "--catalog") {
        // Parity with the Python engine on a real source: the same lines as scratch parity_catalog.py prints.
        Catalog catalog(fromUtf8(argv[2]));
        std::cout << "signature " << catalog.signature << "\n";
        for (const auto& recording : catalog.recordings) {
            std::cout << "recording " << recording.id << " " << recording.name;
            for (const auto& stream : recording.cameras) std::cout << " " << stream.id << ":" << stream.frames.size() << ":" << stream.kind;
            std::cout << "\n";
        }
        for (int i = 3; i < argc; ++i) {
            const std::string spec = argv[i]; const auto first = spec.find('/'), second = spec.find('/', first + 1);
            cv::Mat image = catalog.read(spec.substr(0, first), spec.substr(first + 1, second - first - 1), std::stoi(spec.substr(second + 1)));
            check(image.isContinuous(), "decoded image not continuous");
            std::cout << "decode " << spec << " (" << image.rows << ", " << image.cols << ", " << image.channels() << ") " << sha256Hex(std::string_view(reinterpret_cast<const char*>(image.data), image.total() * image.elemSize())) << "\n";
        }
        return 0;
    }
    utilTests();
    catalogTests();
    assistTests();
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << "\n"; return 1; }
