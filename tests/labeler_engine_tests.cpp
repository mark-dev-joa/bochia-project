// Checks of the C++ labeler engine, one group per module. Each group mirrors the Python test of the same
// name, so a behaviour the two engines must share has the same check on both sides.
#include "labeler_catalog.hpp"
#include "labeler_assist.hpp"
#include "labeler_auto.hpp"
#include "labeler_store.hpp"
#include "labeler_export.hpp"
#include "labeler_native.hpp"
#include "labeler_selection.hpp"
#include "labeler_util.hpp"
#include "native_labeler_view.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <zstd.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <chrono>
#include <random>
#include <numeric>
#include <set>
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
    // Rewrites one camera as an uncompressed BGR8 colour stream showing the given 8x6 image on all three frames.
    void colourCamera(const char* session, const char* camera, const cv::Mat& bgr) const {
        const std::string pixels(reinterpret_cast<const char*>(bgr.data), bgr.total() * 3);
        const int size = int(bgr.total() * 3);
        std::string index;
        for (int frame = 0; frame < 3; ++frame) {
            Json info = {{"width", bgr.cols}, {"height", bgr.rows}, {"padding_x", 0}, {"padding_y", 0}, {"pixel_format", 0x02180015}, {"size", size}, {"stored_size", size},
                         {"offset", frame * size}, {"codec", 0}, {"filter_stride", 0}, {"status", 0}, {"host_ns", 1000 + frame * 100 + (std::string(camera) == "camera-1" ? 5 : 0)}};
            index += info.dump() + (frame < 2 ? "\n" : "");
        }
        writeFile(source / session / camera / "frames.raw", pixels + pixels + pixels);
        writeFile(source / session / camera / "frames.jsonl", index);
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
    // refineCircle: a bright disc on a dark floor is measured to a fraction of a pixel; nothing is measured on a flat image
    cv::Mat floor(240, 320, CV_8UC3, cv::Scalar(40, 45, 50)); cv::circle(floor, {160, 120}, 20, cv::Scalar(230, 230, 230), -1, cv::LINE_8);
    cv::Mat discMask(240, 320, CV_8UC1, cv::Scalar(0)); cv::circle(discMask, {160, 120}, 20, cv::Scalar(1), -1, cv::LINE_8);
    const CircleFit fit = refineCircle(floor, discMask);
    check(fit.ok && std::abs(fit.x - 160.5) < .2 && std::abs(fit.y - 120.5) < .2 && std::abs(fit.r - 20) < .3 && fit.coverage > .9 && fit.residual < .5, "circle fit wrong: " + std::to_string(fit.x) + "," + std::to_string(fit.y) + " r " + std::to_string(fit.r) + " ok " + std::to_string(fit.ok));   // cv::circle paints the pixels whose centres lie within 20
    cv::Mat shrunk(240, 320, CV_8UC1, cv::Scalar(0)); cv::circle(shrunk, {159, 119}, 18, cv::Scalar(1), -1, cv::LINE_8);   // a draft mask a pixel or two inside the ball
    const CircleFit fromShrunk = refineCircle(floor, shrunk);
    check(fromShrunk.ok && std::abs(fromShrunk.r - 20) < .3 && std::abs(fromShrunk.x - 160.5) < .2, "the fit must find the image edge, not the mask edge");
    check(!refineCircle(cv::Mat(240, 320, CV_8UC3, cv::Scalar(40, 45, 50)), discMask).ok, "a flat image must not give a circle");
    Json refinedBox = {{"bbox", {140, 100, 41, 41}}, {"segmentation", encodeMask(discMask)}, {"occluded", false}, {"source", "auto_sam"}};
    Json refinedBox2 = refinedBox; refinedBox2["bbox"] = {141, 101, 39, 39}; refinedBox2["segmentation"] = encodeMask(shrunk); refinedBox2["area"] = cv::countNonZero(shrunk);
    check(refinable(refinedBox2) && refineBallBox(refinedBox2, floor) && !refinable(refinedBox2), "the shrunk draft was not refined");
    check(refinedBox2["circle"].size() == 3 && std::abs(refinedBox2["detection_bbox"][0].get<double>() - 140.5) < .3 && std::abs(refinedBox2["detection_bbox"][2].get<double>() - 40) < .6, "refined box wrong: " + refinedBox2["detection_bbox"].dump());
    const cv::Mat measured = decodeMaskChecked(refinedBox2["segmentation"], 320, 240);
    check(std::abs(refinedBox2["bbox"][0].get<double>() - 140.5) <= .5 && std::abs(refinedBox2["bbox"][2].get<double>() - 40) <= 1 && cv::countNonZero(measured != discMask) <= 16 && refinedBox2["area"] == cv::countNonZero(measured), "the mask must become the measured disc: " + refinedBox2["bbox"].dump());
    check(refineBallBox(refinedBox, floor), "the exact disc was not refined");
    const Json kept = validateBoxes(Json::array({refinedBox}), 320, 240)[0];
    check(kept["circle"] == refinedBox["circle"] && kept.contains("detection_bbox"), "validation must keep the circle");
    Json occludedBox = refinedBox; occludedBox["occluded"] = true; occludedBox.erase("circle");
    Json drawn = refinedBox; drawn["source"] = "manual_polygon"; drawn.erase("circle");
    Json nudged = refinedBox; nudged.erase("circle");   // a detection_bbox without a circle is a correction a person made
    check(!refinable(occludedBox) && !refinable(drawn) && !refinable(nudged), "occluded, hand-drawn and hand-corrected balls must not be refined");
    const Json edited = maskAnnotation(shrunk, kept, "manual_brush");
    check(!edited.contains("circle") && !edited.contains("detection_bbox"), "a new outline must drop the measured circle");
    check(validateBoxes(Json::array({{{"bbox", {1, 1, 3, 3}}, {"circle", {1, 2}}}}), 8, 6)[0].contains("circle") == false, "a malformed circle must be dropped");
    std::cout << "PASS: RLE checks, contours, overlap, polygon proposal, box validation and ball colour\n";
}

// LabelerTests.save and .splits of the Python fixture.
Json saveFrame(Project& project, const Fixture& fixture, int recording = 0, const char* camera = "camera-0", int index = 0, bool reviewed = true, Json boxes = nullptr) {
    if (boxes.is_null()) boxes = Json::array({{{"bbox", {2, 1, 3, 4}}, {"occluded", true}}});
    return project.saveFrame({{"recording", fixture.ids[size_t(recording)]}, {"camera", camera}, {"index", index}, {"reviewed", reviewed}, {"boxes", boxes}, {"expected_revision", project.state["revision"]}});
}
Json assignSplits(Project& project, const Fixture& fixture) {
    return project.saveSplits({{"expected_revision", project.state["revision"]}, {"splits", {{fixture.ids[0], "train"}, {fixture.ids[1], "val"}, {fixture.ids[2], "test"}}}});
}
void storeTests() {
    Fixture fixture;
    Catalog catalog(fixture.source);
    Project project(catalog, fixture.project, fixture.exports);
    check(project.state["revision"] == 0 && project.state["splits"][fixture.ids[1]] == "unassigned" && project.state["source_signature"] == catalog.signature && project.state["frames"].empty(), "new project state wrong");
    check(utf8(project.path) == utf8(fs::weakly_canonical(fixture.project)), "project path must be absolute");
    // test_save_resume_and_conflict_do_not_overwrite
    Json summary = saveFrame(project, fixture);
    check(summary["reviewed"] == 1 && summary["boxes"] == 1 && summary["export_frames"] == 1 && summary["export_boxes"] == 1 && summary["draft_boxes"] == 0 && summary["revision"] == 1, "summary after a save wrong: " + summary.dump());
    const std::string key0 = fixture.ids[0] + "/camera-0/0";
    check(summary["frames"][key0]["review_status"] == "reviewed" && summary["frames"][key0]["selected"] == true && summary["frames"][key0]["selection_reason"] == "unfiltered" && summary["selection"]["enabled"] == false, "frame summary wrong");
    { Project resumed(catalog, fixture.project, fixture.exports); check(resumed.frame(fixture.ids[0], "camera-0", 0)["reviewed"] == true && resumed.state["revision"] == 1, "saved frame did not survive a reopen"); }
    const std::string before = readFile(fixture.project);
    check(fails([&] { project.saveSplits({{"expected_revision", 0}, {"splits", {{fixture.ids[0], "train"}}}}); }, "다른 창에서 변경"), "stale revision accepted");
    try { project.saveSplits({{"expected_revision", 0}, {"splits", {{fixture.ids[0], "train"}}}}); check(false, "no conflict"); } catch (const Conflict&) {}
    check(readFile(fixture.project) == before, "a refused save touched the file");
    SetFileAttributesW(fixture.project.c_str(), FILE_ATTRIBUTE_READONLY);   // "disk full": the replace fails
    check(fails([&] { saveFrame(project, fixture, 0, "camera-0", 1); }, "파일을"), "a failed write must throw");
    SetFileAttributesW(fixture.project.c_str(), FILE_ATTRIBUTE_NORMAL);
    check(project.summary()["reviewed"] == 1 && project.state["revision"] == 1 && !project.state["frames"].contains(fixture.ids[0] + "/camera-0/1"), "a failed write changed the state");
    // test_invalid_coordinates_and_missing_split_rejected (the store part) and the status checks
    check(fails([&] { saveFrame(project, fixture, 0, "camera-0", 0, true, Json::array({{{"bbox", {7, 0, 3, 2}}}})); }, "박스는 이미지 안에"), "invalid box saved");
    check(fails([&] { project.saveFrame({{"recording", fixture.ids[0]}, {"camera", "camera-0"}, {"index", 0}, {"reviewed", 1}, {"boxes", Json::array()}, {"expected_revision", 1}}); }, "검수 상태가 올바르지"), "non-bool reviewed accepted");
    check(fails([&] { project.saveFrame({{"recording", fixture.ids[0]}, {"camera", "camera-0"}, {"index", 0}, {"reviewed", true}, {"review_status", "hold"}, {"boxes", Json::array()}, {"expected_revision", 1}}); }, "일치하지"), "reviewed with hold accepted");
    check(fails([&] { saveFrame(project, fixture, 0, "camera-0", 0, true, Json::array({{{"bbox", {2, 1, 3, 4}}, {"color", "red"}}})); }, "모노 이미지의 색상"), "a colour on a mono camera was accepted");
    check(fails([&] { project.saveSplits({{"expected_revision", 1}, {"splits", {{"nope", "train"}}}}); }, "촬영별 분할") && fails([&] { project.saveSplits({{"expected_revision", 1}, {"splits", {{fixture.ids[0], "later"}}}}); }, "촬영별 분할"), "bad splits accepted");
    // test_mask_persistence_export_and_bbox_agree (the store part)
    cv::Mat mask(6, 8, CV_8UC1, cv::Scalar(0)); mask(cv::Rect(2, 1, 3, 4)).setTo(1); mask.at<uchar>(2, 3) = 0;
    const Json rle = encodeMask(mask);
    assignSplits(project, fixture);
    saveFrame(project, fixture, 0, "camera-0", 0, true, Json::array({{{"bbox", {2, 1, 3, 4}}, {"segmentation", rle}, {"source", "efficientsam"}, {"occluded", true}}}));
    { Project resumed(catalog, fixture.project, fixture.exports); const Json restored = resumed.frame(fixture.ids[0], "camera-0", 0)["boxes"][0];
      check(restored["segmentation"] == rle && !restored["contours"].empty() && restored["source"] == "efficientsam" && restored["occluded"] == true, "mask did not survive the store"); }
    check(!project.state["frames"][key0]["boxes"][0].contains("contours"), "contours must never be stored");
    check(fails([&] { saveFrame(project, fixture, 0, "camera-0", 0, true, Json::array({{{"bbox", {1, 1, 3, 4}}, {"segmentation", rle}}})); }, "분할 윤곽과 박스"), "box away from its mask saved");
    // test_manual_polygon_mask_and_rejected_coordinates (the store part)
    saveFrame(project, fixture, 0, "camera-0", 0, true, Json::array({polygonProposal(Json::parse("[[2,1],[4,1],[4,4],[2,4]]"), 8, 6)}));
    const Json restored = project.frame(fixture.ids[0], "camera-0", 0)["boxes"][0];
    check(restored["source"] == "manual_polygon" && cv::countNonZero(decodeMaskChecked(restored["segmentation"], 8, 6)) == 12, "polygon box lost in the store");
    const Json shown = project.frame(fixture.ids[0], "camera-1", 2);
    check(shown["key"] == fixture.ids[0] + "/camera-1/2" && shown["boxes"].empty() && shown["reviewed"] == false && shown["host_ns"] == "1205" && shown["width"] == 8 && shown["selection"]["reason"] == "unfiltered" && shown["revision"] == project.state["revision"], "unlabelled frame response wrong: " + shown.dump());
    // test_background_writes_do_not_conflict_or_overwrite_human_edits
    const std::string key1 = fixture.ids[0] + "/camera-0/1";
    const Json automatic = {{"recording", fixture.ids[0]}, {"camera", "camera-0"}, {"index", 1}, {"width", 8}, {"height", 6}, {"boxes", Json::array()}, {"reviewed", false}, {"automatic", {{"needs_attention", true}}}, {"human_touched", false}};
    const int revision = project.state["revision"];
    check(project.commitAutomatic({{key1, automatic}}) == 1 && project.state["revision"] == revision && project.state["auto_revision"] == 1, "automatic commit wrong");
    summary = project.summary();
    check(summary["automatic_frames"] == 1 && summary["frames"][key1]["needs_attention"] == true && summary["frames"][key1]["review_status"] == "draft" && summary["frames"][key1]["automatic"] == true, "automatic frame summary wrong");
    saveFrame(project, fixture, 0, "camera-0", 1);
    const Json edited = project.state["frames"][key1];
    check(edited["human_touched"] == true && edited["reviewed"] == true && edited["automatic"]["needs_attention"] == true, "the automatic note must survive a human save");
    check(project.commitAutomatic({{key1, automatic}}) == 0 && project.state["frames"][key1] == edited && project.state["auto_revision"] == 1, "a background write overwrote human work");
    // test_sidecar_human_override_reload_and_invalid_reference, with a hand-written sidecar
    const std::string dup = fixture.ids[1] + "/camera-0/1", rep = fixture.ids[1] + "/camera-0/0";
    Json sidecar = {{"version", 1}, {"source_signature", catalog.signature}, {"created_at", "2026-10-08T00:00:00+00:00"}, {"algorithm", "test"}, {"rules", {{"center_pixels", 8.0}}},
                    {"frames", {{rep, {{"selected", true}, {"representative", rep}, {"reason", "first_frame"}}}, {dup, {{"selected", false}, {"representative", rep}, {"reason", "near_duplicate"}}}}}};
    atomicJson(selectionPath(project.path), sidecar, true);
    summary = project.summary();
    check(summary["frames"].contains(dup) == false && summary["selection"]["enabled"] == true && summary["selection"]["selected"] == 17 && summary["selection"]["excluded"] == 1 && summary["selection"]["total"] == 18 && summary["selection"]["coverage"] == 2, "sidecar view wrong: " + summary["selection"].dump());
    check(project.frame(fixture.ids[1], "camera-0", 1)["selection"]["selected"] == false, "frame must show the sidecar decision");
    saveFrame(project, fixture, 1, "camera-0", 1, false);
    summary = project.summary();
    check(summary["frames"][dup]["selected"] == true && summary["frames"][dup]["selection_reason"] == "human_preserved" && summary["selection"]["selected"] == 18, "human work must override the sidecar");
    Sleep(5);
    Json invalid = sidecar; invalid["source_signature"] = "different"; atomicJson(selectionPath(project.path), invalid, true);
    check(fails([&] { project.summary(); }, "대표 프레임 필터와 촬영 데이터가 다릅니다"), "a sidecar of another source was accepted");
    Sleep(5);
    invalid = sidecar; invalid["frames"][dup]["representative"] = fixture.ids[1] + "/camera-1/0"; atomicJson(selectionPath(project.path), invalid, true);
    check(fails([&] { SelectionFile(catalog, selectionPath(project.path)).read(); }, "중복 프레임은"), "a representative of another camera was accepted");
    Sleep(5);
    invalid = sidecar; invalid["frames"][dup]["selected"] = 1; atomicJson(selectionPath(project.path), invalid, true);
    check(fails([&] { SelectionFile(catalog, selectionPath(project.path)).read(); }, "선택 상태"), "a non-bool selection was accepted");
    fs::remove(selectionPath(project.path));
    check(project.summary()["selection"]["enabled"] == false, "a removed sidecar must disable the filter");
    // selectedReviewed: the export subset follows the selection, except for human work
    Json snapshot = project.state;
    snapshot["frame_selection"] = {{"frames", {{key0, {{"selected", false}}}, {key1, {{"selected", false}}}}}};
    snapshot["frames"][key0]["human_touched"] = false;
    auto chosen = selectedReviewed(snapshot);
    check(chosen.size() == 1 && chosen[0].first == key1, "selected reviewed set wrong");
    // test_source_change_rejected_and_plain_unicode_images_supported
    const fs::path raw = fixture.source / "capture-a/camera-0/frames.raw";
    writeFile(raw, readFile(raw) + "extra");
    Catalog changed(fixture.source);
    check(fails([&] { Project(changed, fixture.project, fixture.exports); }, "기존 라벨 프로젝트와 촬영 데이터가 다릅니다"), "a changed source opened the old project");
    const fs::path photos = fixture.temp.path / L"사진" / L"다른날";
    std::vector<unsigned char> png; cv::imencode(".png", catalog.read(fixture.ids[1], "camera-0", 0), png);
    writeFile(photos / L"공.png", std::string(png.begin(), png.end()));
    Catalog plain(photos.parent_path());
    check(plain.recordings.size() == 1 && plain.recordings[0].name == "다른날" && plain.recordings[0].cameras[0].id == "images" && plain.recordings[0].cameras[0].kind == "이미지" && plain.recordings[0].complete, "image folder catalog wrong");
    const cv::Mat photo = plain.read(plain.recordings[0].id, "images", 0);
    for (int y = 0; y < 6; ++y) for (int x = 0; x < 8; ++x) check(photo.at<cv::Vec3b>(y, x)[0] == fixture.original[size_t(y * 8 + x)], "image folder pixels wrong");
    check(fails([&] { Project(plain, fixture.temp.path / "photos.json", photos / "exports"); }, "이미지 입력 폴더 밖"), "export root inside the image folder accepted");
    Project album(plain, fixture.temp.path / "photos.json", fixture.temp.path / "photo-exports");
    const Json page = album.frame(plain.recordings[0].id, "images", 0);
    check(page["width"] == 8 && page["height"] == 6 && page["host_ns"] == "0", "image frame size must come from the file");
    std::cout << "PASS: project store, summary, conflicts, failed writes, automatic commits, sidecar view and image folders\n";
}

// NativeWorkflowTests.review_all and .confirm of the Python fixture.
void reviewAll(Project& project) {
    for (const auto& scene : sceneIndex(project.catalog)) {
        Json keys = Json::array();
        for (const auto& member : scene["members"]) keys.push_back(member["key"]);
        nativeAction(project, "review", {{"keys", keys}, {"status", "reviewed"}, {"expected_revision", project.state["revision"]}});
    }
}
Json confirmSplit(Project& project) {
    Json plan = nativeAction(project, "split-preview", Json::object());
    plan["expected_revision"] = project.state["revision"];
    return nativeAction(project, "split-confirm", plan);
}
Json sceneKeysOf(const Json& scene) { Json keys = Json::array(); for (const auto& member : scene["members"]) keys.push_back(member["key"]); return keys; }
void nativeTests() {
    Fixture fixture;
    Catalog catalog(fixture.source);
    Project project(catalog, fixture.project, fixture.exports);
    // test_scene_index_covers_each_frame_once_and_retains_unmatched
    Json scenes = sceneIndex(catalog);
    std::set<std::string> keys; size_t total = 0; bool pairs = true;
    for (const auto& scene : scenes) { pairs = pairs && scene["members"].size() == 2; for (const auto& member : scene["members"]) { keys.insert(member["key"].get<std::string>()); ++total; } }
    check(total == 18 && keys.size() == 18 && pairs && scenes.size() == 9, "scene index must cover each frame once");
    check(scenes[0]["id"] == fixture.ids[0] + "/0" && scenes[0]["recording_number"] == 1 && scenes[0]["index"] == 0 && scenes[0]["host_ns"] == "1000"
          && scenes[0]["members"][1]["delta_ms"] == 5e-06 && scenes[0]["members"][1]["camera"] == "camera-1" && scenes[0]["members"][0]["kind"] == "모노" && scenes[0]["members"][0]["name"] == "Mono A", "scene fields wrong: " + scenes[0].dump());
    check(scenes[8]["id"] == fixture.ids[2] + "/8" && scenes[8]["recording_number"] == 3, "scene ids must count across recordings");
    Catalog shifted(fixture.source);
    for (auto& time : shifted.recordings[0].cameras[1].times) time += 1'000'000'000;
    scenes = sceneIndex(shifted); keys.clear(); bool single = false;
    for (const auto& scene : scenes) { single = single || scene["members"].size() == 1; for (const auto& member : scene["members"]) keys.insert(member["key"].get<std::string>()); }
    check(keys.size() == 18 && single && scenes.size() == 12, "unmatched frames must stay visible as sparse scenes");
    // test_review_all_then_confirm_and_label_edit_invalidates_confirmation
    check(fails([&] { confirmSplit(project); }, "미완료"), "confirm before review accepted");
    reviewAll(project);
    Json native = nativeCatalog(project);
    check(native["target_images"] == 18 && native["reviewed_target_images"] == 18 && native["api_version"] == 5 && native["split_confirmed"] == false && native["fingerprint"].is_null() && native["groups"].empty()
          && native["scenes"][0]["members"][0]["status"] == "reviewed" && native["scenes"][0]["members"][0]["count"] == 0 && native["recordings"].size() == 3 && native["summary"]["reviewed"] == 18, "native catalog after review wrong");
    const Json confirmed = confirmSplit(project);
    check(splitValid(project), "confirmation must be valid right after confirming");
    const std::string version = project.state["native_split"]["fingerprint"];
    check(confirmed["fingerprint"] == version && confirmed["split_confirmed"] == true && nativeCatalog(project)["fingerprint"] == version && version.size() == 64 && nativeCatalog(project)["split_confirmed"] == true, "fingerprint not reported");
    check(project.state["native_groups"].size() == 3 && project.state["native_split"]["splits"] == project.state["splits"], "confirmation state wrong");
    { Project restored(catalog, fixture.project, fixture.exports); check(splitValid(restored), "confirmation lost on reopen"); }
    saveFrame(project, fixture, 0, "camera-0", 0, false);
    check(!splitValid(project) && nativeCatalog(project)["fingerprint"].is_null() && !project.state.contains("native_split") && nativeCatalog(project)["split_confirmed"] == false, "a label edit must invalidate the confirmation");
    check(fails([&] { nativeAction(project, "export", {{"crops", false}}); }, "분할을 확정"), "export without a valid confirmation accepted");
    // test_data_version_covers_the_assignment_and_the_groups
    reviewAll(project);
    const Json first = confirmSplit(project);
    Json groups = Json::object(); for (const auto& id : fixture.ids) groups[id] = id;
    Json splits = project.state["splits"];
    std::string train, test;
    for (const auto& [recording, split] : splits.items()) { if (split == "train") train = recording; if (split == "test") test = recording; }
    splits[train] = "test"; splits[test] = "train";
    auto request = [&] { return Json{{"groups", groups}, {"splits", splits}, {"expected_revision", project.state["revision"]}}; };
    const Json swapped = nativeAction(project, "split-confirm", request());
    check(swapped["fingerprint"] != first["fingerprint"] && splitValid(project), "the same labels with another test set must be another dataset");
    check(nativeAction(project, "split-confirm", request())["fingerprint"] == swapped["fingerprint"], "confirming the same plan twice must give the same version");
    Json renamed = Json::object(); for (const auto& id : fixture.ids) renamed[id] = "batch-" + id;
    const Json paired = {{fixture.ids[0], "pair"}, {fixture.ids[1], "pair"}, {fixture.ids[2], "single"}};
    check(fingerprint(project, nullptr, &renamed) == fingerprint(project, nullptr, &groups) && fingerprint(project, nullptr, &paired) != fingerprint(project, nullptr, &groups), "group names must not matter, membership must");
    // test_batch_groups_survive_a_label_fix_and_a_restart
    Json plan = nativeAction(project, "split-preview", {{"groups", renamed}}); plan["expected_revision"] = project.state["revision"];
    nativeAction(project, "split-confirm", plan);
    saveFrame(project, fixture, 0, "camera-0", 0, false);
    check(!splitValid(project) && nativeCatalog(project)["groups"] == renamed, "groups lost after a label fix");
    { Project restored(catalog, fixture.project, fixture.exports); check(nativeCatalog(restored)["groups"] == renamed, "groups lost after a restart"); }
    // test_review_refuses_labels_the_screen_has_not_shown
    const std::string key = fixture.ids[0] + "/camera-0/0";
    Json sceneKeys;
    for (const auto& scene : sceneIndex(catalog)) for (const auto& member : scene["members"]) if (member["key"] == key) sceneKeys = sceneKeysOf(scene);
    saveFrame(project, fixture, 0, "camera-0", 0, false);   // one box arrives after a screen loaded the scene with none
    auto review = [&](const Json& seen) { return nativeAction(project, "review", {{"keys", sceneKeys}, {"seen", seen}, {"expected_revision", project.state["revision"]}}); };
    Json seen = Json::object(); for (const auto& item : sceneKeys) seen[item.get<std::string>()] = 0;
    bool conflicted = false;
    try { review(seen); } catch (const Conflict&) { conflicted = true; }
    check(conflicted && project.state["frames"][key]["reviewed"] == false, "unseen boxes were confirmed");
    seen[key] = 1; review(seen);
    check(project.state["frames"][key]["reviewed"] == true && project.state["frames"][key]["human_touched"] == true && !project.state["frames"][key]["boxes"][0].contains("contours"), "review with the right counts failed");
    // test_atomic_scene_review_hold_and_mono_color_guard
    const Json scene0 = sceneKeysOf(sceneIndex(catalog)[0]);
    nativeAction(project, "review", {{"keys", scene0}, {"status", "hold"}, {"expected_revision", project.state["revision"]}});
    for (const auto& item : scene0) check(project.state["frames"][item.get<std::string>()]["review_status"] == "hold" && project.state["frames"][item.get<std::string>()]["reviewed"] == false, "hold not stored");
    const Json prior = project.state;
    check(fails([&] { nativeAction(project, "review", {{"keys", {scene0[0], fixture.ids[0] + "/camera-1/2"}}, {"expected_revision", prior["revision"]}}); }, "서로 다른 시점"), "frames of different moments reviewed together");
    check(project.state == prior, "a refused review changed the state");
    check(fails([&] { nativeAction(project, "review", {{"keys", {scene0[0], fixture.ids[1] + "/camera-0/0"}}, {"expected_revision", prior["revision"]}}); }, "한 장면의 촬영만"), "two recordings reviewed together");
    check(fails([&] { nativeAction(project, "review", {{"keys", Json::array()}, {"expected_revision", 0}}); }, "검수 상태 요청") && fails([&] { nativeAction(project, "review", {{"keys", scene0}, {"status", "later"}, {"expected_revision", 0}}); }, "검수 상태 요청"), "bad review requests accepted");
    check(fails([&] { nativeAction(project, "review", {{"keys", scene0}, {"expected_revision", 0}}); }, "다른 창에서"), "stale review revision accepted");
    check(fails([&] { nativeAction(project, "nope", Json::object()); }, "지원하지 않는"), "unknown action accepted");
    // test_groups_never_cross_splits_and_insufficient_groups_rejected
    reviewAll(project);
    plan = nativeAction(project, "split-preview", Json::object());
    std::set<std::string> values; for (const auto& [recording, split] : plan["splits"].items()) values.insert(split.get<std::string>());
    check(values == std::set<std::string>{"train", "val", "test"} && plan["splits"].size() == 3 && plan["counts"][fixture.ids[0]] == 6 && plan["groups"][fixture.ids[1]] == fixture.ids[1], "split plan wrong: " + plan.dump());
    Json one = Json::object(); for (const auto& id : fixture.ids) one[id] = "one-scene";
    check(fails([&] { nativeAction(project, "split-preview", {{"groups", one}}); }, "최소 3개"), "one group planned");
    check(fails([&] { nativeAction(project, "split-preview", {{"groups", {{fixture.ids[0], "a"}}}}); }, "모든 촬영에"), "partial groups planned");
    const Json before = project.state;
    const Json same = {{fixture.ids[0], "same"}, {fixture.ids[1], "same"}, {fixture.ids[2], "other"}};
    check(fails([&] { nativeAction(project, "split-confirm", {{"groups", same}, {"splits", plan["splits"]}, {"expected_revision", before["revision"]}}); }, "최소 3개"), "a group across splits confirmed");   // three recordings in two groups fail the group count first, as in Python
    check(project.state == before, "a refused confirmation changed the state");
    Json partial = plan["splits"]; partial[fixture.ids[0]] = "unassigned";
    check(fails([&] { nativeAction(project, "split-confirm", {{"groups", plan["groups"]}, {"splits", partial}, {"expected_revision", before["revision"]}}); }, "모든 촬영을"), "an unassigned recording confirmed");
    // test_grow_masks_widens_draft_masks_and_leaves_human_work
    {
        Fixture second; Catalog catalog2(second.source); Project project2(catalog2, second.project, second.exports);
        cv::Mat mask(6, 8, CV_8UC1, cv::Scalar(0)); mask(cv::Rect(3, 2, 2, 2)).setTo(1);
        const Json draft = {{"bbox", {3, 2, 2, 2}}, {"segmentation", encodeMask(mask)}, {"area", 4}, {"occluded", false}, {"source", "auto_sam"}};
        const std::string key2 = second.ids[0] + "/camera-0/0";
        project2.state["frames"][key2] = {{"recording", second.ids[0]}, {"camera", "camera-0"}, {"index", 0}, {"width", 8}, {"height", 6}, {"boxes", {draft}}, {"reviewed", false}, {"human_touched", false}};
        saveFrame(project2, second, 1, "camera-0", 0, true, Json::array({draft}));   // saved by a person: left alone
        check(nativeCatalog(project2)["growable"] == Json({{"frames", 1}, {"balls", 1}}), "growable count wrong");
        const Json result = nativeAction(project2, "grow-masks", {{"pixels", 1}, {"expected_revision", project2.state["revision"]}});
        const Json grown = project2.state["frames"][key2]["boxes"][0];
        check(grown["bbox"] == Json::array({2, 1, 4, 4}) && grown["area"] == 12, "grown box wrong: " + grown.dump());
        cv::Mat expected(6, 8, CV_8UC1, cv::Scalar(0)); expected(cv::Rect(2, 2, 4, 2)).setTo(1); expected(cv::Rect(3, 1, 2, 4)).setTo(1);
        check(cv::countNonZero(decodeMaskChecked(grown["segmentation"], 8, 6) != expected) == 0, "grown mask is not the one-pixel ellipse dilation");
        check(project2.state["frames"][second.ids[1] + "/camera-0/0"]["boxes"][0]["bbox"] == Json::array({3.0, 2.0, 2.0, 2.0}), "human work was grown");
        check(result["frames"] == 1 && result["balls"] == 1 && result["mask_growth"]["pixels"] == 1 && nativeCatalog(project2)["mask_growth"]["pixels"] == 1 && nativeCatalog(project2)["growable"] == Json({{"frames", 1}, {"balls", 1}}), "grow result wrong: " + result.dump());
        check(fails([&] { nativeAction(project2, "grow-masks", {{"pixels", 9}, {"expected_revision", project2.state["revision"]}}); }, "1에서 3") && fails([&] { nativeAction(project2, "grow-masks", {{"pixels", true}, {"expected_revision", project2.state["revision"]}}); }, "1에서 3"), "bad pixel counts accepted");
        nativeAction(project2, "grow-masks", {{"pixels", 2}, {"expected_revision", project2.state["revision"]}});
        check(project2.state["mask_growth"]["pixels"] == 3 && project2.state["mask_growth"]["at"].get<std::string>().size() == 32, "growth passes must add up");
    }
    // test_colour_fill_runs_in_the_background_and_keeps_given_colours, with a real colour camera instead of a patched read()
    {
        Fixture third;
        cv::Mat mask(6, 8, CV_8UC1, cv::Scalar(0)); mask(cv::Rect(1, 1, 4, 4)).setTo(1);
        cv::Mat image(6, 8, CV_8UC3, cv::Scalar(30, 40, 50)); image.setTo(cv::Scalar(8, 12, 32), mask);
        third.colourCamera("capture-a", "camera-0", image);
        Catalog catalog3(third.source);
        check(catalog3.camera(third.ids[0], "camera-0").kind == "컬러" && catalog3.read(third.ids[0], "camera-0", 1).at<cv::Vec3b>(2, 2) == cv::Vec3b(8, 12, 32), "colour fixture wrong");
        Project project3(catalog3, third.project, third.exports);
        const Json red = {{"bbox", {1, 1, 4, 4}}, {"segmentation", encodeMask(mask)}, {"occluded", false}, {"source", "auto_sam"}};
        Json white = red; white["color"] = "white";
        const std::string key3 = third.ids[0] + "/camera-0/0";
        project3.state["frames"][key3] = {{"recording", third.ids[0]}, {"camera", "camera-0"}, {"index", 0}, {"width", 8}, {"height", 6}, {"boxes", {red, white}}, {"reviewed", false}, {"human_touched", false}};
        check(nativeCatalog(project3)["colorable"] == 1 && colorFillStatus(project3)["status"] == "idle", "colorable count wrong");
        const Json started = nativeAction(project3, "fill-colors", Json::object());
        check(started["status"] == "running" && started["total"] == 1, "colour fill did not start: " + started.dump());
        for (int i = 0; i < 500 && colorFillStatus(project3)["status"] == "running"; ++i) Sleep(10);
        check(colorFillStatus(project3)["status"] == "complete" && colorFillStatus(project3)["filled"] == 1 && colorFillStatus(project3)["done"] == 1, "colour fill did not complete: " + colorFillStatus(project3).dump());
        const Json boxes = project3.state["frames"][key3]["boxes"];
        check(boxes[0]["color"] == "red" && boxes[1]["color"] == "white", "colours wrong: " + boxes.dump());
        check(nativeCatalog(project3)["colorable"] == 0 && nativeCatalog(project3)["color_fill"]["balls"] == 1 && project3.state["revision"] == 0 && project3.state["auto_revision"] == 1 && !project3.state.contains("native_split"), "colour fill must record on the automatic revision: " + project3.state.dump().substr(0, 200));
        check(fails([&] { saveFrame(project3, third, 0, "camera-1", 0, true, Json::array({{{"bbox", {2, 1, 3, 4}}, {"color", "red"}}})); }, "모노"), "mono guard lost");
        saveFrame(project3, third, 0, "camera-0", 1, true, Json::array({{{"bbox", {1, 1, 4, 4}}, {"color", "blue"}}}));   // a colour camera takes colours
        // A shown colour-camera ball carries its colour before anything is stored, and keeps it once the frame is confirmed.
        const std::string key4 = third.ids[0] + "/camera-0/2", mono4 = third.ids[0] + "/camera-1/2";
        for (const auto& k : {key4, mono4}) project3.state["frames"][k] = {{"recording", third.ids[0]}, {"camera", k == key4 ? "camera-0" : "camera-1"}, {"index", 2}, {"width", 8}, {"height", 6}, {"boxes", {red}}, {"reviewed", false}, {"human_touched", false}};
        check(project3.frame(third.ids[0], "camera-0", 2)["boxes"][0]["color"] == "red" && !project3.state["frames"][key4]["boxes"][0].contains("color"), "a shown colour-camera ball must carry its colour without being stored yet");
        check(!project3.frame(third.ids[0], "camera-1", 2)["boxes"][0].contains("color"), "a mono ball must stay without colour");
        nativeAction(project3, "review", {{"keys", {key4, mono4}}, {"status", "reviewed"}, {"expected_revision", project3.state["revision"]}});
        check(project3.state["frames"][key4]["boxes"][0]["color"] == "red" && !project3.state["frames"][mono4]["boxes"][0].contains("color"), "confirming must store the shown colour");
        BallAssist noModel(fs::path("missing-model"));
        const Json polygonPayload = {{"recording", third.ids[0]}, {"camera", "camera-0"}, {"index", 1}, {"points", Json::parse("[[1,1],[4,1],[4,4],[1,4]]")}};
        check(assistance(project3, noModel, "polygon", polygonPayload)["proposals"][0]["color"] == "red", "a polygon on the colour camera must propose the colour");
        Json monoPayload = polygonPayload; monoPayload["camera"] = "camera-1";
        check(!assistance(project3, noModel, "polygon", monoPayload)["proposals"][0].contains("color"), "a polygon on a mono camera must not propose a colour");
    }
    // refine-circles: the boxes of drafted balls come from the image's edge; hand-drawn and occluded balls are left alone
    {
        Fixture fifth;
        cv::Mat floor(100, 120, CV_8UC3, cv::Scalar(40, 45, 50)); cv::circle(floor, {60, 50}, 20, cv::Scalar(230, 230, 230), -1, cv::LINE_8);
        fifth.colourCamera("capture-a", "camera-0", floor);
        Catalog catalog5(fifth.source); Project project5(catalog5, fifth.project, fifth.exports);
        cv::Mat small(100, 120, CV_8UC1, cv::Scalar(0)); cv::circle(small, {59, 49}, 18, cv::Scalar(1), -1, cv::LINE_8);   // the draft stops short of the edge
        const auto bounds = cv::boundingRect(small);
        const Json draft = {{"bbox", {bounds.x, bounds.y, bounds.width, bounds.height}}, {"segmentation", encodeMask(small)}, {"occluded", false}, {"source", "auto_sam"}};
        Json drawn = draft; drawn["source"] = "manual_polygon";
        Json hidden = draft; hidden["occluded"] = true;
        const std::string key5 = fifth.ids[0] + "/camera-0/0";
        project5.state["frames"][key5] = {{"recording", fifth.ids[0]}, {"camera", "camera-0"}, {"index", 0}, {"width", 120}, {"height", 100}, {"boxes", {draft, drawn, hidden}}, {"reviewed", false}, {"human_touched", false}};
        const std::string key6 = fifth.ids[0] + "/camera-0/1";   // the same ball on an image a person confirmed
        project5.state["frames"][key6] = {{"recording", fifth.ids[0]}, {"camera", "camera-0"}, {"index", 1}, {"width", 120}, {"height", 100}, {"boxes", {draft}}, {"reviewed", true}, {"review_status", "reviewed"}, {"human_touched", true}};
        check(nativeCatalog(project5)["circleable"] == 1 && nativeCatalog(project5)["circleable_all"] == 2 && circleRefineStatus(project5)["status"] == "idle", "circleable counts wrong");
        const Json started = nativeAction(project5, "refine-circles", Json::object());
        check(started["status"] == "running" && started["total"] == 1, "circle refinement did not start: " + started.dump());
        for (int i = 0; i < 500 && circleRefineStatus(project5)["status"] == "running"; ++i) Sleep(10);
        const Json status5 = circleRefineStatus(project5);
        check(status5["status"] == "complete" && status5["balls"] == 1 && status5["skipped"] == 0, "circle refinement did not complete: " + status5.dump());
        const Json& boxes5 = project5.state["frames"][key5]["boxes"];
        check(boxes5[0].contains("circle") && std::abs(boxes5[0]["circle"][2].get<double>() - 20) < .3 && std::abs(boxes5[0]["detection_bbox"][0].get<double>() - 40.5) < .3 && std::abs(boxes5[0]["bbox"][0].get<double>() - 40.5) <= .5 && std::abs(boxes5[0]["bbox"][2].get<double>() - 40) <= 1 && boxes5[0]["segmentation"] != draft["segmentation"] && boxes5[0]["source"] == "auto_sam", "the drafted ball must get the measured disc and box: " + boxes5[0].dump().substr(0, 160));
        check(!boxes5[1].contains("circle") && !boxes5[2].contains("circle"), "hand-drawn and occluded balls were refined");
        check(project5.state["revision"] == 0 && project5.state["auto_revision"] == 1 && project5.state["circle_refine"]["balls"] == 1 && nativeCatalog(project5)["circleable"] == 0 && nativeCatalog(project5)["circleable_all"] == 1, "circle refinement record wrong");
        check(!project5.state["frames"][key6]["boxes"][0].contains("circle"), "a confirmed image must be left alone by default");
        nativeAction(project5, "refine-circles", {{"include_reviewed", true}});
        for (int i = 0; i < 500 && circleRefineStatus(project5)["status"] == "running"; ++i) Sleep(10);
        check(circleRefineStatus(project5)["balls"] == 1 && project5.state["frames"][key6]["boxes"][0].contains("circle") && project5.state["frames"][key6]["reviewed"] == true && project5.state["circle_refine"]["balls"] == 2 && nativeCatalog(project5)["circleable_all"] == 0, "the confirmed image must be refined on request and stay confirmed");
        check(project5.frame(fifth.ids[0], "camera-0", 0)["boxes"][0]["color"] == "white", "the shown frame must still colour the ball");
    }
    // test_thumbnail_is_a_small_cached_jpeg
    const cv::Mat thumb = thumbnail(project, fixture.ids[0], "camera-0", 0, 32);
    check(thumb.rows == 24 && thumb.cols == 32 && thumb.data == thumbnail(project, fixture.ids[0], "camera-0", 0, 32).data, "thumbnail size or cache wrong");
    check(fails([&] { thumbnail(project, fixture.ids[0], "camera-0", 0, 4000); }, "썸네일 폭"), "oversized thumbnail accepted");
    // test_catalog_reports_attention_and_recording_statistics
    {
        Fixture fourth; Catalog catalog4(fourth.source); Project project4(catalog4, fourth.project, fourth.exports);
        saveFrame(project4, fourth, 0, "camera-0", 0, true, Json::array({{{"bbox", {2, 1, 3, 4}}}, {{"bbox", {0, 0, 2, 2}}}}));
        const Json native4 = nativeCatalog(project4);
        const Json& stats = native4["stats"][fourth.ids[0]];
        check(stats["number"] == 1 && stats["images"] == 6 && stats["balls"] == 2 && stats["empty"] == 5 && stats["small"] == 2 && stats["reviewed"] == 1 && stats["day"].is_null() && stats["started"].is_null() && stats["seconds"].is_null() && stats["frames"] == Json::array({3, 3}) && stats["name"] == "capture-a", "recording stats wrong: " + stats.dump());
        bool attention = true;
        for (const auto& scene : native4["scenes"]) for (const auto& member : scene["members"]) attention = attention && member.contains("attention") && member.contains("selected") && member.contains("count");
        check(attention, "scene members must carry attention, selected and count");
        check(native4["stats"][fourth.ids[2]]["images"] == 6 && native4["stats"][fourth.ids[2]]["number"] == 3, "stats of the other recordings wrong");
    }
    std::cout << "PASS: scene index, review, confirmation, data version, groups, unseen guard, hold, split rules, grow masks, colour fill, thumbnails and stats\n";
}

std::vector<fs::path> listFiles(const fs::path& folder, const std::string& extension) {
    std::vector<fs::path> files;
    if (fs::is_directory(folder)) for (const auto& entry : fs::directory_iterator(folder)) if (entry.path().extension() == extension) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    return files;
}
Json readJson(const fs::path& path) { return Json::parse(readFile(path)); }
cv::Mat readPng(const fs::path& path) { const std::string bytes = readFile(path); return cv::imdecode(cv::Mat(1, int(bytes.size()), CV_8UC1, const_cast<char*>(bytes.data())), cv::IMREAD_COLOR); }
void exportTests() {
    Fixture fixture;
    Catalog catalog(fixture.source);
    Project project(catalog, fixture.project, fixture.exports);
    // test_detection_override_keeps_mask_and_both_coco_coordinate_contracts
    cv::Mat mask(6, 8, CV_8UC1, cv::Scalar(0)); mask(cv::Rect(2, 1, 3, 4)).setTo(1); mask.at<uchar>(2, 3) = 0;
    const Json rle = encodeMask(mask);
    const Json box = {{"bbox", {2, 1, 3, 4}}, {"segmentation", rle}, {"detection_bbox", {1, 0, 5, 6}}, {"source", "manual_brush"}};
    saveFrame(project, fixture, 0, "camera-0", 0, true, Json::array({box}));
    check(cv::countNonZero(decodeMaskChecked(project.frame(fixture.ids[0], "camera-0", 0)["boxes"][0]["segmentation"], 8, 6) != mask) == 0, "mask lost before export");
    assignSplits(project, fixture);
    Json result = exportDataset(catalog, project.state, fixture.temp.path / "export-native");
    fs::path output = fromUtf8(result["path"]);
    const Json segmentation = readJson(output / "annotations/instances_train.json")["annotations"][0], detection = readJson(output / "annotations/instances_detection_train.json")["annotations"][0];
    check(segmentation["bbox"] == Json::array({2.0, 1.0, 3.0, 4.0}) && segmentation["area"] == 11 && segmentation["segmentation"] == rle && segmentation["occluded"] == false && segmentation["category_id"] == 1 && segmentation["image_id"] == 1, "segmentation annotation wrong: " + segmentation.dump());
    check(detection["bbox"] == Json::array({1.0, 0.0, 5.0, 6.0}) && !detection.contains("segmentation") && detection["area"] == 30.0, "detection annotation wrong: " + detection.dump());
    auto yoloTokens = [&](const fs::path& folder) { const auto files = listFiles(folder, ".txt"); check(files.size() == 1, "one yolo file expected in " + utf8(folder)); std::vector<double> values; std::string text = readFile(files[0]); size_t start = 0; while (start < text.size()) { size_t end = text.find_first_of(" \n", start); if (end == std::string::npos) end = text.size(); if (end > start) values.push_back(std::stod(text.substr(start, end - start))); start = end + 1; } return values; };
    std::vector<double> yolo = yoloTokens(output / "labels/train");
    check(yolo.size() == 5 && std::abs(yolo[3] - 5.0 / 8) < 1e-7 && std::abs(yolo[4] - 1) < 1e-7 && std::abs(yolo[1] - 3.5 / 8) < 1e-7, "yolo line must use the detection box");
    check(fails([&] { Json bad = box; bad["detection_bbox"] = {-1, 0, 5, 6}; validateBoxes(Json::array({bad}), 8, 6); }, "박스"), "detection box outside the image accepted");
    check(!fs::exists(output / "INCOMPLETE.txt") && fs::exists(output / "manifest.json") && fs::exists(output / "data.yaml") && listFiles(output / "images/train", ".png").size() == 1 && listFiles(output / "crops/train", ".png").size() == 1, "export folder layout wrong");
    const Json manifest = readJson(output / "manifest.json");
    check(manifest["version"] == 1 && manifest["complete"] == true && manifest["source_signature"] == catalog.signature && manifest["project_revision"] == project.state["revision"] && manifest["frame_selection"] == Json({{"enabled", false}}) && manifest["native_split"].is_null() && manifest["counts"]["train"]["images"] == 1 && manifest["counts"]["val"]["boxes"] == 0 && manifest["frames"].size() == 1 && manifest["frames"][0]["host_ns"] == "1000" && manifest["frames"][0]["recording_path"] == "capture-a" && manifest["crops"][0]["bbox"] == Json::array({1.0, 0.0, 5.0, 6.0}), "manifest wrong: " + manifest.dump());
    check(readFile(output / "data.yaml").find("names:\n  0: boccia_ball\n") != std::string::npos && readFile(output / "data.yaml").rfind("path: \"", 0) == 0, "data.yaml wrong");
    // test_invalid_coordinates_and_missing_split_rejected (the export part)
    { Fixture other; Catalog catalog2(other.source); Project project2(catalog2, other.project, other.exports); saveFrame(project2, other);
      check(fails([&] { startExport(project2, true); }, "분할을 먼저 지정"), "export without splits started");
      Project empty(catalog2, other.temp.path / "empty.json", other.exports);
      check(fails([&] { startExport(empty, true); }, "검수 완료한 프레임이"), "export without reviewed frames started"); }
    // test_export_group_splits_review_filter_and_coordinate_agreement
    {
        Fixture other; Catalog catalog2(other.source); Project project2(catalog2, other.project, other.exports);
        assignSplits(project2, other);
        saveFrame(project2, other);                                              // train
        saveFrame(project2, other, 0, "camera-1", 2);                            // same recording, same split
        saveFrame(project2, other, 1, "camera-0", 0, true, Json::array());       // explicitly verified negative, val
        saveFrame(project2, other, 2, "camera-0", 0, false);                     // draft, excluded
        result = exportDataset(catalog2, project2.state, other.exports);
        output = fromUtf8(result["path"]);
        check(result["images"] == 3 && result["boxes"] == 2, "export counts wrong: " + result.dump());
        const Json train = readJson(output / "annotations/instances_train.json"), val = readJson(output / "annotations/instances_val.json"), test = readJson(output / "annotations/instances_test.json");
        check(train["images"].size() == 2 && train["images"][0]["recording_id"] == other.ids[0] && train["images"][1]["recording_id"] == other.ids[0] && val["images"].size() == 1 && val["annotations"].empty() && test["images"].empty(), "split contents wrong");
        const std::string filename = train["images"][0]["file_name"];
        const cv::Mat image = readPng(output / "images/train" / filename);
        for (int y = 0; y < 6; ++y) for (int x = 0; x < 8; ++x) check(image.at<cv::Vec3b>(y, x)[0] == other.original[size_t(y * 8 + x)], "exported image pixels wrong");
        const auto yoloFiles = listFiles(output / "labels/train", ".txt");
        check(yoloFiles.size() == 2, "two train label files expected");
        const std::vector<double> values = [&] { std::vector<double> v; std::string text = readFile(output / "labels/train" / (fs::path(filename).stem().string() + ".txt")); size_t start = 0; while (start < text.size()) { size_t end = text.find_first_of(" \n", start); if (end == std::string::npos) end = text.size(); if (end > start) v.push_back(std::stod(text.substr(start, end - start))); start = end + 1; } return v; }();
        const double expected[] = {0, 3.5 / 8, 3.0 / 6, 3.0 / 8, 4.0 / 6};
        for (int i = 0; i < 5; ++i) check(std::abs(values[size_t(i)] - expected[i]) < 1e-7, "yolo coordinates wrong");
        const Json manifest2 = readJson(output / "manifest.json");
        check(manifest2["complete"] == true && !fs::exists(output / "INCOMPLETE.txt"), "manifest must say complete");
        const Json crop = manifest2["crops"][0];
        const cv::Mat cropImage = readPng(output / fromUtf8(crop["file_name"]));
        check(cropImage.rows == 4 && cropImage.cols == 3, "crop size wrong");
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 3; ++x) check(cropImage.at<cv::Vec3b>(y, x)[0] == other.original[size_t((y + 1) * 8 + x + 2)], "crop pixels wrong");
        check(crop["bbox"] == train["annotations"][0]["bbox"] && crop["occluded"] == true && crop["crop_bounds"] == Json::array({2, 1, 5, 5}), "crop record wrong: " + crop.dump());
        check(listFiles(output / "labels/val", ".txt").size() == 1 && readFile(listFiles(output / "labels/val", ".txt")[0]).empty(), "the verified negative must have an empty label file");
    }
    // test_failed_export_is_marked_incomplete: the raw file goes missing between the save and the export
    {
        Fixture other; Catalog catalog2(other.source); Project project2(catalog2, other.project, other.exports);
        assignSplits(project2, other); saveFrame(project2, other);
        writeFile(other.source / "capture-a/camera-0/frames.raw", "short");
        check(fails([&] { exportDataset(catalog2, project2.state, other.exports); }, "잘렸습니다"), "export of an unreadable frame succeeded");
        const auto folders = [&] { std::vector<fs::path> f; for (const auto& entry : fs::directory_iterator(other.exports)) f.push_back(entry.path()); return f; }();
        check(folders.size() == 1 && fs::exists(folders[0] / "INCOMPLETE.txt") && !fs::exists(folders[0] / "manifest.json") && readFile(folders[0] / "INCOMPLETE.txt").find("내보내기 실패") == 0, "failed export not marked");
        check(exportHistory(project2).empty(), "a failed export must not appear in the history");
    }
    // test_confirmed_native_export_contains_all_reviewed_images_and_fixed_groups, test_export_history_lists_only_completed_exports
    {
        Fixture other; Catalog catalog2(other.source); Project project2(catalog2, other.project, other.exports);
        check(exportHistory(project2).empty(), "history of a new project must be empty");
        saveFrame(project2, other, 0, "camera-0", 0, false);
        reviewAll(project2);
        confirmSplit(project2);
        const Json started = nativeAction(project2, "export", {{"crops", false}});
        check(started["status"] == "running" && started["total"] == 18, "export job did not start: " + started.dump());
        check(fails([&] { nativeAction(project2, "export", {{"crops", false}}); }, "이미 내보내는 중") || project2.job["status"] != "running", "a second export started while one runs");
        for (int i = 0; i < 500 && project2.job["status"] == "running"; ++i) Sleep(10);
        check(project2.job["status"] == "complete", "export job failed: " + project2.job.dump());
        const Json done = project2.job["result"];
        check(done["images"] == 18 && done["boxes"] == 1 && project2.job["done"] == 18, "export result wrong: " + done.dump());
        output = fromUtf8(done["path"]);
        const Json manifest3 = readJson(output / "manifest.json");
        check(manifest3["splits"] == project2.state["splits"] && manifest3["native_split"] == project2.state["native_split"] && manifest3["frame_selection"]["enabled"] == false, "manifest must record the confirmed split");
        for (const auto& id : other.ids) {
            std::set<std::string> used;
            for (const auto& frame : manifest3["frames"]) if (frame["recording_path"] == catalog2.recording(id).relativePath) used.insert(frame["split"].get<std::string>());
            check(used == std::set<std::string>{project2.state["splits"][id].get<std::string>()}, "a recording was split across sets");
        }
        for (const char* split : datasetSplits) check(fs::exists(output / "annotations" / (std::string("instances_detection_") + split + ".json")), "detection coco missing");
        check(!fs::exists(output / "crops"), "crops were written although not requested");
        const fs::path broken = other.exports / "20990101-000000-broken";
        writeFile(broken / "manifest.json", "{}"); writeFile(broken / "INCOMPLETE.txt", "x");
        const fs::path noManifest = other.exports / "20990102-000000-empty"; fs::create_directories(noManifest);
        const Json history = exportHistory(project2);
        check(history.size() == 1 && history[0]["fingerprint"] == project2.state["native_split"]["fingerprint"] && history[0]["name"] == utf8(output.filename()) && history[0]["path"] == utf8(output), "history wrong: " + history.dump());
        int images = 0; for (const auto& [split, counts] : history[0]["counts"].items()) images += counts["images"].get<int>();
        check(images == 18 && nativeCatalog(project2)["split_confirmed"] == true, "history counts wrong");
    }
    // test_export_subset_masks_and_explicit_human_inclusion, with the selection written by hand (Task 8 builds it)
    {
        Fixture other; Catalog catalog2(other.source); Project project2(catalog2, other.project, other.exports);
        Json snapshot = project2.state;
        const Json ball = {{"bbox", {2, 1, 3, 4}}, {"segmentation", rle}, {"occluded", false}};
        Json decisions = Json::object();
        for (const auto& scene : sceneIndex(catalog2)) for (const auto& member : scene["members"]) {
            const std::string key = member["key"];
            const std::string representative = key.substr(0, key.rfind('/')) + "/0";
            snapshot["frames"][key] = {{"recording", key.substr(0, key.find('/'))}, {"camera", member["camera"]}, {"index", member["index"]}, {"width", 8}, {"height", 6}, {"boxes", {ball}}, {"reviewed", true}, {"human_touched", false}};
            decisions[key] = {{"selected", member["index"] == 0}, {"representative", representative}, {"reason", member["index"] == 0 ? "first_frame" : "near_duplicate"}};
        }
        snapshot["frame_selection"] = {{"enabled", true}, {"frames", decisions}, {"algorithm", "test"}};
        for (const auto& id : other.ids) snapshot["splits"][id] = "train";
        const std::string included = other.ids[0] + "/camera-0/2";
        snapshot["frames"][included]["human_touched"] = true;
        result = exportDataset(catalog2, snapshot, other.exports, false);
        check(result["images"] == 7, "the export must take the representatives plus human work: " + result.dump());
        const Json manifest4 = readJson(fromUtf8(result["path"]) / "manifest.json");
        bool found = false; for (const auto& frame : manifest4["frames"]) found = found || frame["key"] == included;
        check(found && manifest4["frame_selection"]["algorithm"] == "test" && !manifest4["frame_selection"].contains("frames"), "human work missing from the export or the selection summary wrong");
        const Json coco = readJson(fromUtf8(result["path"]) / "annotations/instances_train.json");
        bool masks = true; for (const auto& entry : coco["annotations"]) masks = masks && entry["segmentation"] == rle;
        check(masks && coco["images"].size() == 7, "masks must be exported as stored");
    }
    std::cout << "PASS: export of coco, detection coco, yolo, crops, manifest, failures, history and selection subsets\n";
}

// Stands in for EfficientSAM: a disc of radius 10 at the crop centre, as the Python test's Encoder/Decoder do.
struct FakeSam : SamBackend {
    int calls = 0;
    std::vector<float> encode(const cv::Mat& crop) override {
        ++calls;
        check(crop.at<cv::Vec3b>(0, 0) == cv::Vec3b(10, 40, 200) && crop.rows == 99 && crop.cols == 99, "the crop must be the image region around the first click");
        return std::vector<float>(256 * 64 * 64, 0.f);
    }
    SamOutput decode(const std::vector<float>&, const std::vector<cv::Point2f>&, const std::vector<float>&, cv::Size size) override {
        SamOutput out;
        for (int i = 0; i < 3; ++i) { cv::Mat logits(size, CV_32F, cv::Scalar(-5)); cv::circle(logits, {size.width / 2, size.height / 2}, 10, cv::Scalar(5), -1); out.logits.push_back(logits); }
        out.ious = {.9f, .8f, .7f};
        return out;
    }
};
void samTests() {
    // test_prompt_crop_rgb_conversion_cache_and_negative_click, with a stand-in model
    auto fake = std::make_unique<FakeSam>(); FakeSam* model = fake.get();
    BallAssist assist(fs::path("missing"), std::move(fake));
    check(assist.status()["ready"] == true && assist.status()["model_files"] == false && assist.status()["model"] == "EfficientSAM-Ti", "status with an injected model wrong");
    cv::Mat image(140, 180, CV_8UC3, cv::Scalar(10, 40, 200)); cv::circle(image, {90, 70}, 10, cv::Scalar(230, 230, 230), -1, cv::LINE_8);   // a ball where the stand-in model puts its disc
    const Json positive = {{"x", 90}, {"y", 70}, {"label", 1}};
    const Json first = assist.segment(image, "same-frame", Json::array({positive}), 14);
    const Json second = assist.segment(image, "same-frame", Json::array({positive, {{"x", 112}, {"y", 70}, {"label", 0}}}), 14);
    check(model->calls == 1, "the crop embedding must be cached");
    check(first["bbox"] == second["bbox"] && first["source"] == "efficientsam" && first["quality"] == 0.9 && first["area"] > 280 && first["area"] < 340 && !first["contours"].empty() && first["occluded"] == false, "segment result wrong: " + first.dump());
    check(first["bbox"] == Json::array({80, 60, 21, 21}), "the disc at the crop centre must land on the click: " + first["bbox"].dump());
    check(validateBoxes(Json::array({first}), 180, 140)[0]["segmentation"] == first["segmentation"], "segment mask and box disagree");
    check(fails([&] { assist.segment(image, "same-frame", Json::array({positive, {{"x", 90}, {"y", 70}, {"label", 0}}}), 14); }, "분리하지"), "contradicting clicks accepted");
    check(fails([&] { assist.segment(image, "same-frame", Json::array({{{"x", -1}, {"y", 0}, {"label", 1}}}), 14); }, "클릭 X"), "click outside accepted");
    check(fails([&] { assist.segment(image, "same-frame", Json::array({positive}), std::nan("")); }, "반지름"), "nan radius accepted");
    check(fails([&] { assist.segment(image, "same-frame", Json::array({{{"x", 90}, {"y", 70}, {"label", 0}}}), 14); }, "첫 클릭"), "negative first click accepted");
    check(fails([&] { assist.segment(image, "same-frame", Json::array({positive, {{"x", 170}, {"y", 70}, {"label", 0}}}), 14); }, "첫 공 주변"), "far click accepted");
    check(fails([&] { assist.segment(image, "same-frame", Json::array({positive, {{"x", 91}, {"y", 70}, {"label", true}}}), 14); }, "형식") && fails([&] { assist.segment(image, "same-frame", Json::array(), 14); }, "1~32개"), "bad click lists accepted");
    check(fails([&] { assist.segment(image, "same-frame", Json::array({positive}), 14, Json::array({1, 2, 3})); }, "시작 박스") && fails([&] { assist.segment(image, "same-frame", Json::array({positive}), 14, Json::array({170, 60, 20, 20})); }, "이미지 안에"), "bad seed boxes accepted");
    const Json boxed = assist.segment(image, "same-frame", Json::array({positive}), 14, Json::array({80, 60, 21, 21}));
    check(boxed["bbox"] == first["bbox"] && model->calls == 1, "box prompt must reuse the embedding");
    assist.segment(image, "other-frame", Json::array({positive}), 14);
    check(model->calls == 2, "another frame must encode again");
    // A proposal of the engine's segment tool is measured again from the image: the dragged box fixture of the Python test
    // has no real ball, so this uses the disc above through the dispatcher's own refinement.
    {
        Json proposal = first; refineBallBox(proposal, image);
        check(proposal.contains("circle") && std::abs(proposal["circle"][0].get<double>() - 90.5) < .4 && std::abs(proposal["circle"][2].get<double>() - 10.5) < .7 && proposal.contains("detection_bbox"), "the proposal's circle must follow the image: " + proposal.value("circle", Json()).dump());
    }
    // test_assist_proposals_do_not_mutate_or_cross_cameras, the parts without a model
    BallAssist none(fs::path("missing-model"));
    check(none.status()["ready"] == false && none.status()["runtime"] == true, "missing model reported ready");
    check(fails([&] { none.segment(cv::Mat(80, 80, CV_8UC3, cv::Scalar(0)), "test", Json::array({{{"x", 40}, {"y", 40}, {"label", 1}}}), 14); }, "준비되지"), "segment without a model did not explain");
    // test_circle_candidates_and_moving_box_matching
    cv::Mat scene(180, 240, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::circle(scene, {80, 90}, 14, cv::Scalar(80, 130, 240), -1); cv::circle(scene, {80, 90}, 14, cv::Scalar(240, 240, 240), 2); cv::line(scene, {75, 85}, {83, 95}, cv::Scalar(40, 40, 40), 2);
    const Json boxes = BallAssist::circles(scene, 10, 18);
    bool found = false;   // "near" is a Windows macro
    for (const auto& box : boxes) found = found || (std::abs(box["bbox"][0].get<double>() - 65) < 6 && std::abs(box["bbox"][1].get<double>() - 75) < 6);
    check(found && boxes[0]["source"] == "circle" && boxes[0]["occluded"] == false, "circle candidate missing: " + boxes.dump());
    const cv::Mat shift = (cv::Mat_<float>(2, 3) << 1, 0, 12, 0, 1, -7);
    cv::Mat moved; cv::warpAffine(scene, moved, shift, {240, 180});
    const Json previousBox = {{"bbox", {65, 75, 31, 31}}, {"occluded", true}, {"segmentation", {{"old", "mask"}}}};
    const Json propagated = BallAssist::propagate(scene, moved, Json::array({previousBox}));
    check(propagated.size() == 1 && propagated[0]["bbox"] == Json::array({77, 68, 31, 31}) && !propagated[0].contains("segmentation") && propagated[0]["occluded"] == true && propagated[0]["source"] == "tracking" && propagated[0]["quality"] > .9, "propagated box wrong: " + propagated.dump());
    check(BallAssist::propagate(scene, moved, Json::array({previousBox}), propagated).empty(), "a propagated box must not duplicate an existing one");
    check(BallAssist::propagate(scene, cv::Mat(180, 240, CV_8UC3, cv::Scalar(0, 0, 0)), Json::array({previousBox})).empty(), "a vanished ball was tracked");
    check(fails([&] { BallAssist::propagate(scene, cv::Mat(100, 100, CV_8UC3), Json::array()); }, "크기가 다릅니다"), "mismatched frames accepted");
    check(fails([&] { BallAssist::circles(scene, 1, 18); }, "최소 반지름") && fails([&] { BallAssist::circles(scene, 10, 5); }, "최대 반지름"), "bad radii accepted");
    // assistance() on the fixture: nothing is written, cameras are not crossed
    Fixture fixture; Catalog catalog(fixture.source); Project project(catalog, fixture.project, fixture.exports);
    saveFrame(project, fixture, 0, "camera-1", 0);
    const Json payload = {{"recording", fixture.ids[0]}, {"camera", "camera-0"}, {"index", 1}};
    check(fails([&] { assistance(project, none, "propagate", payload); }, "이전 프레임"), "propagate crossed cameras");
    saveFrame(project, fixture, 0, "camera-0", 0);
    const Json before = project.state;
    const Json tracked = assistance(project, none, "propagate", payload);
    check(tracked["previous_index"] == 0 && tracked["key"] == fixture.ids[0] + "/camera-0/1" && tracked["proposals"].is_array() && project.state == before, "propagate response wrong: " + tracked.dump());   // the 8x6 frames are a flat ramp, so nothing matches
    Json region = payload; region["region"] = {2, 1, 5, 5};
    const Json rounds = assistance(project, none, "circles", region);
    check(rounds["key"] == fixture.ids[0] + "/camera-0/1" && rounds["proposals"].is_array() && project.state == before, "circles response wrong");
    check(fails([&] { Json bad = payload; bad["region"] = {5, 5, 9, 9}; assistance(project, none, "circles", bad); }, "박스는 이미지 안에"), "a region outside the image accepted");
    Json polygon = payload; polygon["points"] = Json::parse("[[2,1],[4,1],[4,4],[2,4]]");
    check(assistance(project, none, "polygon", polygon)["proposals"][0]["bbox"] == Json::array({2, 1, 3, 4}) && project.state == before, "polygon via assistance wrong");
    check(fails([&] { Json request = payload; request["points"] = Json::array({{{"x", 3}, {"y", 2}, {"label", 1}}}); assistance(project, none, "segment", request); }, "준비되지") && fails([&] { assistance(project, none, "magic", payload); }, "지원하지 않는 보조"), "assistance errors wrong");
    // The real model, when its files are in the repository: a bright disc on a dark floor must come back as one mask.
    const fs::path models = fs::path(BOCCIA_TEST_ROOT) / "data/models/efficientsam";
    if (samModelFilesExist(models)) {
        BallAssist real(models);
        cv::Mat floor(240, 320, CV_8UC3, cv::Scalar(40, 45, 50)); cv::circle(floor, {160, 120}, 20, cv::Scalar(230, 230, 230), -1);
        const Json ball = real.segment(floor, "disc", Json::array({{{"x", 160}, {"y", 120}, {"label", 1}}}), 20);
        check(ball["area"] > 900 && ball["area"] < 1700 && ball["bbox"][0] > 130 && ball["bbox"][0] < 150 && ball["quality"] > .5, "EfficientSAM did not find the disc: " + ball.dump());   // pi * 20^2 = 1257
        std::cout << "PASS: EfficientSAM-Ti through ONNX Runtime segments a disc\n";
    } else std::cout << "SKIP: no EfficientSAM model files at " << utf8(models) << "\n";
    std::cout << "PASS: SAM prompts, crop cache, click rules, circles, tracking and the assistance dispatcher\n";
}

// SelectionTests.setUp of tests/test_labeler_selection.py: two image folders of five identical frames with one ball.
struct SelectionFixture {
    TempDir temp;
    fs::path source;
    cv::Mat image;
    Json box;
    std::vector<std::string> ids;
    std::unique_ptr<Catalog> catalog;
    std::unique_ptr<Project> project;
    SelectionFixture() : source(temp.path / "images"), image(100, 120, CV_8UC3, cv::Scalar(80, 80, 80)) {
        cv::circle(image, {50, 50}, 10, cv::Scalar(20, 20, 200), -1);
        std::vector<unsigned char> png; cv::imencode(".png", image, png);
        for (const char* folder : {"a", "b"}) for (int i = 0; i < 5; ++i) writeFile(source / folder / (std::to_string(i) + ".png"), std::string(png.begin(), png.end()));
        catalog = std::make_unique<Catalog>(source);
        for (const auto& recording : catalog->recordings) ids.push_back(recording.id);
        project = std::make_unique<Project>(*catalog, temp.path / "project.json", temp.path / "exports");
        cv::Mat mask(100, 120, CV_8UC1, cv::Scalar(0)); cv::circle(mask, {50, 50}, 10, cv::Scalar(1), -1);
        box = {{"bbox", {40, 40, 21, 21}}, {"segmentation", encodeMask(mask)}, {"occluded", false}};
        for (const auto& id : ids) for (int i = 0; i < 5; ++i)
            project->state["frames"][id + "/images/" + std::to_string(i)] = {{"recording", id}, {"camera", "images"}, {"index", i}, {"width", 120}, {"height", 100}, {"boxes", {box}}, {"reviewed", false}, {"human_touched", false}};
    }
};
void selectionTests() {
    SelectionFixture f;
    check(f.ids.size() == 2 && f.catalog->recordings[0].name == "a", "selection fixture wrong");
    // test_static_selection_representatives_and_camera_isolation
    const Json before = f.project->state;
    const Json result = buildSelection(*f.catalog, before);
    check(result["total"] == 10 && result["selected"] == 4 && result["excluded"] == 6, "static selection counts wrong: " + std::to_string(result["selected"].get<int>()));
    for (const auto& [key, entry] : result["frames"].items()) if (!entry["selected"].get<bool>()) {
        const std::string representative = entry["representative"];
        check(key.substr(0, key.rfind('/')) == representative.substr(0, representative.rfind('/')) && result["frames"][representative]["selected"] == true, "a duplicate must point at a kept frame of its own camera");
    }
    check(f.project->state == before, "selection must not change the project");
    check(result["algorithm"] == "within-camera-geometry-mask-appearance-v2" && result["version"] == 1 && result["source_signature"] == f.catalog->signature && result["cameras"].size() == 2
          && result["cameras"][0]["selected"] == 2 && result["cameras"][0]["camera"] == "images" && result["rules"]["center_pixels"] == 8.0 && result["rules"]["scene_width"] == 720 && result["frames"].size() == 10 && result["project_revision"] == 0, "selection file fields wrong");
    const Json periodic = buildSelection(*f.catalog, before, {}, {{"periodic_frames_without_timestamps", 2}});
    check(periodic["selected"] == 6 && periodic["rules"]["periodic_frames_without_timestamps"] == 2 && periodic["frames"][f.ids[0] + "/images/2"]["reason"] == "periodic_static_sample", "periodic retention wrong");
    for (const auto& id : f.ids) for (int i : {0, 4}) check(result["frames"][id + "/images/" + std::to_string(i)]["selected"] == true, "endpoints must be kept");
    const Json& second = result["frames"][f.ids[0] + "/images/1"];
    check(second["reason"] == "near_duplicate" && second["representative"] == f.ids[0] + "/images/0" && second["metrics"]["scene_mae"] == 0.0 && second["metrics"]["mask_iou"] == 1.0 && second["metrics"]["ball_correlation"] == 1.0
          && result["frames"][f.ids[0] + "/images/0"]["reason"] == "first_frame" && !result["frames"][f.ids[0] + "/images/0"].contains("metrics") && result["frames"][f.ids[0] + "/images/4"]["reason"] == "last_frame", "decision records wrong: " + second.dump());
    // test_movement_occlusion_light_texture_and_unknown_ball_are_preserved
    const FrameFeatures reference = frameFeatures(f.image, Json::array({f.box}));
    const auto pairs = matchBoxes(Json::array({f.box}), Json::array({f.box}));
    check(pairs && pairs->size() == 1 && compareFrames(reference, reference, *pairs).duplicate, "identical frames must be duplicates");
    cv::Mat changed = f.image.clone(); changed(cv::Rect(46, 42, 11, 14)).setTo(cv::Scalar(80, 80, 80));
    check(!compareFrames(reference, frameFeatures(changed, Json::array({f.box})), *pairs).duplicate, "a changed ball region must be kept");
    cv::Mat brighter; f.image.convertTo(brighter, -1, 1, 15);
    check(!compareFrames(reference, frameFeatures(brighter, Json::array({f.box})), *pairs).duplicate, "a lighting change must be kept");
    changed = f.image.clone(); cv::circle(changed, {90, 75}, 8, cv::Scalar(200, 20, 20), -1);
    check(!compareFrames(reference, frameFeatures(changed, Json::array({f.box})), *pairs).duplicate, "a new object must be kept");
    Json occluded = f.box; occluded["occluded"] = true;
    check(!matchBoxes(Json::array({f.box}), Json::array({occluded})), "an occlusion change must not match");
    Json jittered = f.box; jittered["bbox"] = {46, 40, 21, 21};
    check(matchBoxes(Json::array({f.box}), Json::array({jittered})) && matchBoxes(Json::array({f.box}), Json::array({jittered}))->size() == 1, "a jittered box must match");
    Json moved = f.box; moved["bbox"] = {50, 40, 21, 21};
    check(!matchBoxes(Json::array({f.box}), Json::array({moved})) && !matchBoxes(Json::array({f.box}), Json::array()), "a moved or missing ball must not match");
    check(matchBoxes(Json::array({f.box, jittered}), Json::array({jittered, f.box}))->size() == 2, "two boxes must match crosswise");
    // test_box_jitter_on_a_still_ball_is_a_duplicate_but_a_moved_ball_is_not
    auto comparison = compareFrames(reference, frameFeatures(f.image, Json::array({jittered})), *matchBoxes(Json::array({f.box}), Json::array({jittered})));
    check(comparison.duplicate, "jitter on a still ball must be a duplicate: " + comparison.reason);
    cv::Mat movedImage(100, 120, CV_8UC3, cv::Scalar(80, 80, 80)); cv::circle(movedImage, {56, 50}, 10, cv::Scalar(20, 20, 200), -1);
    cv::Mat movedMask(100, 120, CV_8UC1, cv::Scalar(0)); cv::circle(movedMask, {56, 50}, 10, cv::Scalar(1), -1);
    Json movedBall = jittered; movedBall["segmentation"] = encodeMask(movedMask);
    comparison = compareFrames(reference, frameFeatures(movedImage, Json::array({movedBall})), *matchBoxes(Json::array({f.box}), Json::array({movedBall})));
    check(!comparison.duplicate, "a moved ball must be kept: " + comparison.reason);
    // test_mask_shape_change_is_kept_even_with_identical_pixels
    Json square = f.box; cv::Mat squareMask(100, 120, CV_8UC1, cv::Scalar(0)); squareMask(cv::Rect(40, 40, 21, 21)).setTo(1); square["segmentation"] = encodeMask(squareMask);
    comparison = compareFrames(frameFeatures(f.image, Json::array({f.box})), frameFeatures(f.image, Json::array({square})), *matchBoxes(Json::array({f.box}), Json::array({square})));
    check(!comparison.duplicate && comparison.reason == "mask_change" && comparison.metrics["mask_iou"] < .85, "a mask shape change must be kept");
    const Json plain = {{"bbox", {40, 40, 21, 21}}};
    check(compareFrames(reference, frameFeatures(cv::Mat(50, 60, CV_8UC3, cv::Scalar(0, 0, 0)), Json::array({plain})), *pairs).reason == "image_dimensions", "different sizes must be kept");
    check(fails([&] { buildSelection(*f.catalog, Json{{"source_signature", "other"}, {"frames", Json::object()}}); }, "원본 촬영이 다릅니다"), "a foreign snapshot was filtered");
    // test_sidecar_human_override_reload_and_invalid_reference, with the built selection
    atomicJson(selectionPath(f.project->path), result, true);
    std::string duplicate;
    for (const auto& [key, entry] : result["frames"].items()) if (!entry["selected"].get<bool>()) { duplicate = key; break; }
    check(f.project->summary()["frames"][duplicate]["selected"] == false, "built sidecar not applied");
    const auto slash = duplicate.rfind('/');
    f.project->saveFrame({{"recording", duplicate.substr(0, duplicate.find('/'))}, {"camera", "images"}, {"index", std::stoi(duplicate.substr(slash + 1))}, {"boxes", {f.box}}, {"reviewed", false}, {"expected_revision", 0}});
    Json summary = f.project->summary();
    check(summary["frames"][duplicate]["selected"] == true && summary["selection"]["selected"] == 5 && f.project->state["revision"] == 1, "human work must override the built sidecar");
    // test_project_export_uses_filter_and_preserves_existing_review
    SelectionFixture g;
    atomicJson(selectionPath(g.project->path), buildSelection(*g.catalog, g.project->state), true);
    std::string reviewed;
    const Json summary2 = g.project->summary();   // a named value: a range-for over a member of a temporary dangles
    for (const auto& [key, entry] : summary2["frames"].items()) if (!entry["selected"].get<bool>()) { reviewed = key; break; }
    g.project->state["frames"][reviewed]["reviewed"] = true;
    check(g.project->summary()["frames"][reviewed]["selected"] == true && g.project->summary()["export_frames"] == 1, "a reviewed duplicate must stay in the export");
    for (const auto& id : g.ids) g.project->state["splits"][id] = "train";
    Json snapshot = g.project->state; snapshot["frame_selection"] = g.project->selection.view(g.project->state["frames"]);
    const Json exported = exportDataset(*g.catalog, snapshot, g.temp.path / "exports", false);
    check(exported["images"] == 1 && readJson(fromUtf8(exported["path"]) / "manifest.json")["frame_selection"]["selected"] == 5, "the export must follow the filter and record it");
    std::cout << "PASS: representative frames, box matching, scene and ball comparison, mask changes and the built sidecar\n";
}

// The Seeds and Assist stand-ins of test_complete_auto_job_resume_and_mask_tracking.
struct FakeSeeds : SeedSource {
    std::string method() const override { return "test_stereo"; }
    std::pair<std::vector<Seed>, std::string> get(const std::string&, const std::string&, int, const cv::Mat&) override { return {{{3.5, 3.0, 1.5}}, "test_stereo"}; }
};
struct FakeSegmenter : Segmenter {
    std::atomic<int>* calls;
    explicit FakeSegmenter(std::atomic<int>* counter) : calls(counter) {}
    Json segment(const cv::Mat&, const std::string&, const Json&, const Json&, const Json&) override {
        ++*calls;
        cv::Mat mask(6, 8, CV_8UC1, cv::Scalar(0)); mask(cv::Rect(2, 1, 3, 4)).setTo(1);
        return {{"bbox", {2, 1, 3, 4}}, {"occluded", false}, {"segmentation", encodeMask(mask)}, {"area", 12}};
    }
};
void autoTests() {
    // test_tracking_mask_translation_and_scale_matches_bbox
    cv::Mat mask(60, 80, CV_8UC1, cv::Scalar(0)); mask(cv::Rect(30, 20, 11, 11)).setTo(1);
    const Json box = transferMask({{"segmentation", encodeMask(mask)}}, {35, 25, 5}, {45, 28, 5}, {80, 60});
    check(box["bbox"] == Json::array({40, 23, 11, 11}) && box["source"] == "auto_tracking" && box["occluded"] == false, "transferred mask wrong: " + box["bbox"].dump());
    validateBoxes(Json::array({box}), 80, 60);
    const Json scaled = transferMask({{"segmentation", encodeMask(mask)}}, {35, 25, 5}, {35, 25, 10}, {80, 60});
    check(scaled["bbox"][2] == 22 && scaled["bbox"][3] == 22, "a doubled radius must double the mask");
    check(fails([&] { transferMask({{"segmentation", encodeMask(mask)}}, {35, 25, 5}, {500, 500, 5}, {80, 60}); }, "원본 밖"), "a mask moved out of the image accepted");
    check(seedBbox({3.5, 3.0, 1.5}, 8, 6) == Json::array({2, 1, 3, 4}) && seedBbox({0.5, 0.5, 2}, 8, 6) == Json::array({0, 0, 3, 3}), "seed box wrong");
    cv::Mat scene(60, 80, CV_8UC3, cv::Scalar(40, 40, 40)); cv::circle(scene, {35, 25}, 6, cv::Scalar(200, 200, 200), -1);
    const cv::Mat patch = appearance(scene, {35, 25, 6});
    check(patch.rows == 40 && patch.cols == 40 && patch.type() == CV_32FC3 && similar(patch, patch), "appearance patch wrong");
    cv::Mat other(60, 80, CV_8UC3, cv::Scalar(40, 40, 40)); cv::rectangle(other, {28, 18}, {42, 32}, cv::Scalar(200, 20, 20), -1);
    check(!similar(patch, appearance(other, {35, 25, 6})), "different looks must not be similar");
    // test_complete_auto_job_resume_and_mask_tracking
    Fixture fixture; Catalog catalog(fixture.source); Project project(catalog, fixture.project, fixture.exports);
    saveFrame(project, fixture, 0, "camera-0", 0);
    const Json manual = project.state["frames"][fixture.ids[0] + "/camera-0/0"];
    std::atomic<int> segmentations{0};
    AutoLabelJob job(project, fixture.temp.path / "no-root", fixture.temp.path / "no-models");
    check(job.status()["status"] == "idle" && !job.ready(), "a job without models must not be ready");
    check(fails([&] { job.start(); }, "모델을 먼저 설치"), "a job without models started");
    Json started = job.start([] { return std::make_unique<FakeSeeds>(); }, [&] { return std::make_unique<FakeSegmenter>(&segmentations); });
    check(started["status"] == "running" && started["total"] == 18 && started["project_path"] == utf8(project.path) && started["run_id"].get<std::string>().size() == 24, "job did not start: " + started.dump());
    for (int i = 0; i < 1000 && job.status()["status"] == "running"; ++i) Sleep(10);
    Json result = job.status();
    check(result["status"] == "complete", "job did not complete: " + result.dump());
    check(result["done"] == 18 && result["written"] == 17 && result["preserved"] == 1 && result["tracked_masks"] > 0 && result["sam_masks"] > 0 && result["seed_method"] == "test_stereo" && result["workers"] >= 1 && result["boxes"] == 17, "job counts wrong: " + result.dump());
    check(project.state["revision"] == 1 && project.state["auto_revision"] >= 1 && project.state["frames"][fixture.ids[0] + "/camera-0/0"] == manual, "the manual frame must stay and the manual revision must not move");
    int automatic = 0;
    for (const auto& [key, item] : project.state["frames"].items()) if (item.contains("automatic")) {
        ++automatic;
        check(item["reviewed"] == false && item["human_touched"] == false && item["automatic"]["seed_method"] == "test_stereo" && item["automatic"]["seed_count"] == 1 && item["automatic"]["needs_attention"] == false && item["automatic"]["run_id"] == result["run_id"], "automatic frame fields wrong: " + item.dump());
        validateBoxes(item["boxes"], 8, 6);
        check(item["boxes"].size() == 1 && (item["boxes"][0]["source"] == "auto_sam" || item["boxes"][0]["source"] == "auto_tracking"), "automatic box wrong");
    }
    check(automatic == 17 && fs::exists(job.statusPath) && readJson(job.statusPath)["status"] == "complete", "automatic frames or the status file missing");
    { AutoLabelJob reopened(project, fixture.temp.path / "no-root", fixture.temp.path / "no-models"); check(reopened.status()["status"] == "complete" && reopened.status()["written"] == 17, "the status file must come back on restart"); }
    const int before = segmentations;
    job.start([] { return std::make_unique<FakeSeeds>(); }, [&] { return std::make_unique<FakeSegmenter>(&segmentations); });
    for (int i = 0; i < 1000 && job.status()["status"] == "running"; ++i) Sleep(10);
    result = job.status();
    check(result["status"] == "complete" && result["written"] == 0 && result["preserved"] == 18 && segmentations == before, "a second run must preserve everything without reading or segmenting");
    // the colour camera gets colours; a mono camera does not
    {
        Fixture third;
        cv::Mat ballMask(6, 8, CV_8UC1, cv::Scalar(0)); ballMask(cv::Rect(2, 1, 3, 4)).setTo(1);
        cv::Mat image(6, 8, CV_8UC3, cv::Scalar(30, 40, 50)); image.setTo(cv::Scalar(8, 12, 32), ballMask);
        third.colourCamera("capture-a", "camera-0", image);
        Catalog catalog3(third.source); Project project3(catalog3, third.project, third.exports);
        AutoLabelJob job3(project3, third.temp.path / "no-root", third.temp.path / "no-models");
        std::atomic<int> count{0};
        job3.start([] { return std::make_unique<FakeSeeds>(); }, [&] { return std::make_unique<FakeSegmenter>(&count); });
        for (int i = 0; i < 1000 && job3.status()["status"] == "running"; ++i) Sleep(10);
        check(job3.status()["status"] == "complete", "colour job failed: " + job3.status().dump());
        check(project3.state["frames"][third.ids[0] + "/camera-0/0"]["boxes"][0]["color"] == "red" && !project3.state["frames"][third.ids[0] + "/camera-1/0"]["boxes"][0].contains("color"), "colour camera drafts must carry the ball colour");
    }
    // SeedCache: without the cache files every frame falls back to circle candidates
    SeedCache cache(catalog, fixture.temp.path / "no-root");
    check(cache.method() == "circle_candidates" && cache.covered() == 0, "an empty seed cache must fall back");
    cv::Mat floor(240, 320, CV_8UC3, cv::Scalar(40, 45, 50)); cv::circle(floor, {160, 120}, 20, cv::Scalar(230, 230, 230), -1);
    const auto [seeds, method] = cache.get(fixture.ids[0], "camera-0", 0, floor);
    check(method == "circle_candidates" && !seeds.empty() && std::abs(seeds[0].x - 160) < 4 && std::abs(seeds[0].y - 120) < 4 && std::abs(seeds[0].radius - 20) < 4, "circle seeds wrong");
    std::cout << "PASS: mask transfer, appearance, automatic draft run with resume, colours and seed fallback\n";
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
    if (argc >= 4 && std::string(argv[1]) == "--store") {
        // Parity with the Python engine on a real project: the same lines as scratch parity_store.py prints.
        Catalog catalog(fromUtf8(argv[2]));
        const auto projectPath = fromUtf8(argv[3]);
        Project project(catalog, projectPath, projectPath.parent_path() / "parity-exports");
        const Json summary = project.summary();
        std::cout << "summary " << summary["revision"] << " " << summary["reviewed"] << " " << summary["boxes"] << " " << summary["export_frames"] << " " << summary["export_boxes"] << " " << summary["automatic_frames"] << " " << summary["draft_boxes"] << " " << summary["auto_revision"] << "\n";
        std::cout << "selection " << summary["selection"].dump(-1, ' ', true) << "\n";
        std::cout << "frames " << summary["frames"].size() << " " << sha256Hex(summary["frames"].dump(-1, ' ', true)) << "\n";
        for (int i = 4; i < argc; ++i) {
            const std::string spec = argv[i]; const auto first = spec.find('/'), second = spec.find('/', first + 1);
            const Json frame = project.frame(spec.substr(0, first), spec.substr(first + 1, second - first - 1), std::stoi(spec.substr(second + 1)));
            std::cout << "frame " << spec << " " << frame["boxes"].size() << " " << sha256Hex(frame.dump(-1, ' ', true)) << "\n";
        }
        return 0;
    }
    if (argc >= 4 && std::string(argv[1]) == "--native") {
        // Parity with the Python engine on a real project: the same lines as scratch parity_native.py prints.
        Catalog catalog(fromUtf8(argv[2]));
        const auto projectPath = fromUtf8(argv[3]);
        Project project(catalog, projectPath, projectPath.parent_path() / "parity-exports");
        auto digest = [](const Json& value) { return sha256Hex(value.dump(-1, ' ', true)); };
        const Json scenes = sceneIndex(catalog);
        std::cout << "scenes " << scenes.size() << " " << digest(scenes) << "\n";
        const Json native = nativeCatalog(project);
        std::cout << "catalog " << native["target_images"] << " " << native["reviewed_target_images"] << " " << native["colorable"] << " " << native["growable"].dump(-1, ' ', true) << " "
                  << (native["split_confirmed"].get<bool>() ? "True" : "False") << " " << native["groups"].dump(-1, ' ', true) << " " << (native["fingerprint"].is_null() ? "None" : native["fingerprint"].get<std::string>()) << "\n";
        std::cout << "stats " << digest(native["stats"]) << "\n";
        std::cout << "scenes-with-state " << digest(native["scenes"]) << "\n";
        std::cout << "targets " << targets(project).size() << "\n";
        Json groups = Json::object(); for (const auto& recording : catalog.recordings) groups[recording.id] = recording.id;
        std::cout << "fingerprint " << fingerprint(project, nullptr, &groups) << "\n";
        std::cout << "history " << exportHistory(project).size() << "\n";
        return 0;
    }
    if (argc >= 4 && std::string(argv[1]) == "--assist") {
        // Parity with the Python engine on real frames: the same lines as scratch parity_assist.py prints.
        Catalog catalog(fromUtf8(argv[2]));
        const auto projectPath = fromUtf8(argv[3]);
        Project project(catalog, projectPath, projectPath.parent_path() / "parity-exports");
        BallAssist assist(fs::path(BOCCIA_TEST_ROOT) / "data/models/efficientsam");
        for (int i = 4; i < argc; ++i) {
            const std::string spec = argv[i]; const auto first = spec.find('/'), second = spec.find('/', first + 1);
            const std::string recording = spec.substr(0, first), camera = spec.substr(first + 1, second - first - 1); const int index = std::stoi(spec.substr(second + 1));
            const Json frame = project.frame(recording, camera, index);
            const cv::Mat image = project.catalog.read(recording, camera, index);
            int n = 0;
            for (const auto& box : frame["boxes"]) {
                const Json& b = box["bbox"];
                const double cx = b[0].get<double>() + b[2].get<double>() / 2, cy = b[1].get<double>() + b[3].get<double>() / 2, radius = std::max(b[2].get<double>(), b[3].get<double>()) / 2;
                for (int withBox = 0; withBox < 2; ++withBox) {
                    const Json result = assist.segment(image, spec, Json::array({{{"x", cx}, {"y", cy}, {"label", 1}}}), radius, withBox ? b : Json());
                    std::cout << "segment " << spec << " " << n << " " << withBox << " " << result["bbox"].dump() << " " << result["area"] << " " << result["quality"].dump() << " " << sha256Hex(result["segmentation"]["counts"].dump()) << "\n";
                }
                ++n;
            }
            const Json circles = BallAssist::circles(image, 6, 45);
            Json head = Json::array(); for (size_t c = 0; c < circles.size() && c < 3; ++c) head.push_back(circles[c]);
            std::cout << "circles " << spec << " " << circles.size() << " " << head.dump() << "\n";
            if (size_t(index + 1) < project.catalog.camera(recording, camera).frames.size())
                std::cout << "propagate " << spec << " " << BallAssist::propagate(image, project.catalog.read(recording, camera, index + 1), frame["boxes"]).dump() << "\n";
        }
        return 0;
    }
    if (argc >= 5 && std::string(argv[1]) == "--selection") {
        // Parity with the Python filter: builds the selection of a project file and writes it, like scratch parity_selection.py.
        const auto started = std::chrono::steady_clock::now();
        Catalog catalog(fromUtf8(argv[2]));
        const Json snapshot = Json::parse(readFile(fromUtf8(argv[3])));
        Json result = buildSelection(catalog, snapshot, [](int done, int total, const std::string& camera) { if (done % 1000 == 0) std::cout << done << "/" << total << " " << camera << "\n"; });
        result["elapsed_seconds"] = std::round(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() * 100) / 100;
        atomicJson(fromUtf8(argv[4]), result, result["frames"].size() < 128);
        std::cout << "selected " << result["selected"] << " excluded " << result["excluded"] << " seconds " << result["elapsed_seconds"] << "\n";
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "--seeds") {
        // Parity with labeler_auto.SeedCache on the real recordings and the repository's detection caches.
        Catalog catalog(fromUtf8(argv[2]));
        SeedCache cache(catalog, fs::path(BOCCIA_TEST_ROOT));
        std::cout << "seeds covered " << cache.covered() << " keys " << cache.seeded() << " total " << cache.seedCount() << " " << cache.method() << "\n";
        return 0;
    }
    if (argc >= 4 && std::string(argv[1]) == "--refine") {
        // Runs the circle refinement over a whole project copy and prints the counts and the time: --refine <source> <project>
        Catalog catalog(fromUtf8(argv[2]));
        const auto projectPath = fromUtf8(argv[3]);
        Project project(catalog, projectPath, projectPath.parent_path() / "refine-exports");
        const auto started = std::chrono::steady_clock::now();
        std::cout << "circleable " << circleable(project) << "\n";
        nativeAction(project, "refine-circles", Json::object());
        while (circleRefineStatus(project)["status"] == "running") Sleep(200);
        std::cout << circleRefineStatus(project).dump() << " seconds " << std::round(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() * 10) / 10 << " circleable " << circleable(project) << "\n";
        return 0;
    }
    if (argc >= 4 && std::string(argv[1]) == "--auto") {
        // Parity with labeler_auto on a project copy: drafts every unlabelled frame with the real model and prints the counts.
        Catalog catalog(fromUtf8(argv[2]));
        const auto projectPath = fromUtf8(argv[3]);
        Project project(catalog, projectPath, projectPath.parent_path() / "auto-cpp-exports");
        AutoLabelJob job(project, fs::path(BOCCIA_TEST_ROOT), fs::path(BOCCIA_TEST_ROOT) / "data/models/efficientsam");
        const auto started = std::chrono::steady_clock::now();
        job.start();
        while (job.status()["status"] == "running") Sleep(500);
        const Json status = job.status();
        Json summary = Json::object();
        for (const char* name : {"status", "done", "written", "preserved", "boxes", "sam_masks", "tracked_masks", "fallback_boxes", "seed_method", "workers"}) summary[name] = status.value(name, Json());
        std::cout << summary.dump() << " seconds " << std::round(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() * 10) / 10 << "\n";
        return 0;
    }
    if (argc >= 6 && std::string(argv[1]) == "--circle-eval") {
        // Measures the Canny circle refinement on real balls: --circle-eval <source> <project> <count> <out.jsonl> [examples.png] [low high band]
        Catalog catalog(fromUtf8(argv[2]));
        const Json state = Json::parse(readFile(fromUtf8(argv[3])));
        const int wanted = std::stoi(argv[4]);
        const double low = argc > 7 ? std::stod(argv[7]) : 20, high = argc > 8 ? std::stod(argv[8]) : 60; const int band = argc > 9 ? std::stoi(argv[9]) : 3;
        std::vector<std::string> keys; for (const auto& [key, frame] : state["frames"].items()) keys.push_back(key);
        std::shuffle(keys.begin(), keys.end(), std::mt19937(3));
        std::ofstream out(fromUtf8(argv[5]), std::ios::binary);
        std::vector<cv::Mat> examples; int balls = 0;
        for (const auto& key : keys) {
            if (balls >= wanted) break;
            const Json& frame = state["frames"][key];
            if (frame["boxes"].empty()) continue;
            const cv::Mat image = catalog.read(frame["recording"], frame["camera"], frame["index"]);
            const bool colour = catalog.camera(frame["recording"], frame["camera"]).kind == "컬러";
            for (size_t i = 0; i < frame["boxes"].size() && balls < wanted; ++i) {
                const Json& box = frame["boxes"][i];
                if (!box.contains("segmentation") || box.value("occluded", false)) continue;
                const cv::Mat mask = decodeMaskChecked(box["segmentation"], frame["width"], frame["height"]);
                std::vector<std::vector<cv::Point>> contours; cv::findContours(mask.clone(), contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
                std::vector<cv::Point> edge; for (const auto& c : contours) edge.insert(edge.end(), c.begin(), c.end());
                const Circle before = fitCircle(edge);
                const CircleFit after = refineCircle(image, mask, band, low, high);
                out << Json{{"key", key}, {"box", i}, {"kind", colour ? "color" : "mono"}, {"bbox", box["bbox"]},
                            {"mask", {{"x", before.x}, {"y", before.y}, {"r", before.r}}},
                            {"fit", {{"x", after.x}, {"y", after.y}, {"r", after.r}, {"ok", after.ok}, {"coverage", after.coverage}, {"inliers", after.inliers}, {"points", after.points}, {"residual", after.residual}}}}.dump() << "\n";
                ++balls;
                if (argc > 6 && examples.size() < 16 && (examples.size() % 2 == 0) == colour) {
                    const cv::Rect bounds = cv::boundingRect(mask); const int margin = 8;
                    const cv::Rect crop = cv::Rect(bounds.x - margin, bounds.y - margin, bounds.width + 2 * margin, bounds.height + 2 * margin) & cv::Rect(0, 0, image.cols, image.rows);
                    cv::Mat view; cv::resize(image(crop), view, {}, 6, 6, cv::INTER_NEAREST);
                    if (view.channels() == 1) cv::cvtColor(view, view, cv::COLOR_GRAY2BGR);
                    auto at = [&](double x, double y) { return cv::Point(int(std::lround((x - crop.x) * 6)), int(std::lround((y - crop.y) * 6))); };
                    for (const auto& c : contours) { std::vector<cv::Point> poly; for (const auto& p : c) poly.push_back(at(p.x + .5, p.y + .5)); cv::polylines(view, poly, true, cv::Scalar(255, 255, 0), 1, cv::LINE_AA); }
                    cv::circle(view, at(before.x + .5, before.y + .5), int(std::lround(before.r * 6)), cv::Scalar(0, 160, 255), 1, cv::LINE_AA);
                    if (after.ok) cv::circle(view, at(after.x, after.y), int(std::lround(after.r * 6)), cv::Scalar(0, 220, 0), 1, cv::LINE_AA);
                    cv::Mat tile(300, 300, CV_8UC3, cv::Scalar(20, 20, 20)); const cv::Rect place(0, 0, std::min(300, view.cols), std::min(300, view.rows)); view(place).copyTo(tile(place));
                    cv::putText(tile, (colour ? "C " : "M ") + std::string(after.ok ? "ok" : "no") + " r " + std::to_string(int(std::lround(before.r * 10)) / 10.0).substr(0, 4) + ">" + std::to_string(int(std::lround(after.r * 10)) / 10.0).substr(0, 4), {4, 292}, cv::FONT_HERSHEY_SIMPLEX, .45, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
                    examples.push_back(tile);
                }
            }
        }
        if (argc > 6 && !examples.empty()) {
            while (examples.size() % 4) examples.push_back(cv::Mat(300, 300, CV_8UC3, cv::Scalar(20, 20, 20)));
            std::vector<cv::Mat> rows; for (size_t r = 0; r < examples.size(); r += 4) { cv::Mat row; cv::hconcat(std::vector<cv::Mat>(examples.begin() + r, examples.begin() + r + 4), row); rows.push_back(row); }
            cv::Mat sheet; cv::vconcat(rows, sheet); cv::imwrite(utf8(fromUtf8(argv[6])), sheet);
        }
        std::cout << "balls " << balls << "\n";
        return 0;
    }
    utilTests();
    catalogTests();
    assistTests();
    storeTests();
    nativeTests();
    exportTests();
    samTests();
    selectionTests();
    autoTests();
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << "\n"; return 1; }
