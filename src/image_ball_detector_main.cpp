#include "yolo_detector.hpp"
#include "background_task.hpp"
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

namespace {
std::string utf8(const fs::path& path) {
    const auto name = path.u8string();
    return {name.begin(), name.end()};
}
fs::path fromUtf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}
double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
fs::path executableDirectory() {
    wchar_t buffer[32768]{};
    const DWORD size = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    if (!size || size >= std::size(buffer)) throw std::runtime_error("실행 파일 경로를 읽을 수 없습니다.");
    return fs::path(buffer).parent_path();
}
struct Options {
    fs::path model, cudaDlls;
    float confidence = .25f;
    bool cuda = true;
    int threads = 4;
};
Options defaultOptions() {
    Options options;
    const auto home = executableDirectory();
    const auto root = fromUtf8(BOCCIA_DATA_DIR);
    // Read the existing detector profile, but never save or change it from this app.
    const auto settings = fs::exists(home / "portable.txt") ? home / "data/settings.json" : root / "settings.json";
    try {
        std::ifstream file(settings);
        if (file) {
            const auto profile = Json::parse(file).value("ball_detection", Json::object());
            options.model = fromUtf8(profile.value("model", std::string{}));
            if (options.model.is_relative() && !options.model.empty()) options.model = home / options.model;
            options.cudaDlls = fromUtf8(profile.value("cuda_dlls", std::string{}));
            options.confidence = std::clamp(profile.value("model_confidence", .25f), .01f, 1.f);
            options.cuda = profile.value("cuda", true);
            options.threads = std::clamp(profile.value("inference_threads", 4), 1, 32);
        }
    } catch (const std::exception&) {
        // A missing or older calibration profile does not prevent standalone use.
    }
    if (!fs::is_regular_file(options.model)) {
        for (const auto& candidate : {
                 home / "models/boccia-yolo26n-1280x960.onnx",
                 root / "models/boccia/yolo26n-20261012-1280x960-u8.onnx",
                 home / "models/boccia-yolo26n.onnx",
                 root / "models/boccia/yolo26n-20261009-u8.onnx"}) {
            if (fs::is_regular_file(candidate)) { options.model = candidate; break; }
        }
    }
    return options;
}
cv::Mat readImage(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("이미지 파일을 열 수 없습니다: " + utf8(path));
    const auto length = file.tellg();
    if (length <= 0 || length > 256 * 1024 * 1024)
        throw std::runtime_error("빈 파일이거나 이미지 파일이 256 MB를 넘습니다.");
    std::vector<uchar> bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())))
        throw std::runtime_error("이미지 파일을 읽을 수 없습니다.");
    auto image = cv::imdecode(bytes, cv::IMREAD_COLOR);
    if (image.empty()) throw std::runtime_error("지원하는 이미지 파일을 넣어 주세요 (PNG, JPG, BMP, TIFF, WebP).");
    return image;
}
void writePng(const fs::path& path, const cv::Mat& image) {
    std::vector<uchar> bytes;
    if (!cv::imencode(".png", image, bytes)) throw std::runtime_error("PNG 변환에 실패했습니다.");
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!file) throw std::runtime_error("결과 이미지를 저장할 수 없습니다: " + utf8(path));
}
void requireSeparateOutput(const fs::path& input, const fs::path& output) {
    std::error_code error;
    if (fs::equivalent(input, output, error))
        throw std::runtime_error("원본 이미지와 다른 이름으로 저장해 주세요.");
}
struct Result {
    fs::path imagePath, model;
    cv::Mat image;
    std::vector<YoloBox> boxes;
    cv::Size inputShape;
    std::string provider;
    float confidence = .25f;
    double detectionMs = 0, readMs = 0, preparationMs = 0, totalMs = 0;
};
struct Engine {
    std::shared_ptr<const YoloDetector> detector;
    fs::path requestedModel;
    bool requestedCuda = false;

    Result run(const fs::path& path, const Options& options) {
        const auto started = Clock::now();
        Result result;
        result.imagePath = path;
        auto stage = Clock::now();
        result.image = readImage(path);
        result.readMs = elapsedMs(stage);
        if (!detector || requestedModel != options.model || requestedCuda != options.cuda) {
            stage = Clock::now();
            auto next = std::make_shared<const YoloDetector>(options.model, options.cuda);
            // Warm up once per model/device. The reported detection time is the subsequent real run.
            next->detect(result.image, options.confidence);
            detector = std::move(next);
            requestedModel = options.model;
            requestedCuda = options.cuda;
            result.preparationMs = elapsedMs(stage);
        }
        stage = Clock::now();
        result.boxes = detector->detect(result.image, options.confidence);
        result.detectionMs = elapsedMs(stage); // Synchronous ORT Run, including preprocessing and box decode.
        result.totalMs = elapsedMs(started);
        result.model = detector->path();
        result.provider = detector->provider();
        result.inputShape = detector->inputShape();
        result.confidence = options.confidence;
        return result;
    }
};
Json resultJson(const Result& result) {
    Json boxes = Json::array();
    for (size_t i = 0; i < result.boxes.size(); ++i) {
        const auto& box = result.boxes[i];
        boxes.push_back({{"id", i + 1}, {"confidence", box.confidence}, {"class_id", box.label},
                         {"center_px", {(box.x1 + box.x2) * .5f, (box.y1 + box.y2) * .5f}},
                         {"box_xyxy_px", {box.x1, box.y1, box.x2, box.y2}}});
    }
    return {{"image", utf8(result.imagePath)}, {"image_size", {result.image.cols, result.image.rows}},
            {"model", utf8(result.model)}, {"model_input_size", {result.inputShape.width, result.inputShape.height}},
            {"provider", result.provider}, {"confidence_threshold", result.confidence},
            {"ball_count", result.boxes.size()}, {"detection_ms", result.detectionMs},
            {"image_read_ms", result.readMs}, {"model_preparation_ms", result.preparationMs},
            {"total_ms", result.totalMs}, {"warmup_runs", result.preparationMs > 0 ? 1 : 0}, {"balls", boxes}};
}
cv::Mat annotatedImage(const Result& result) {
    cv::Mat annotated = result.image.clone();
    for (size_t i = 0; i < result.boxes.size(); ++i) {
        const auto& box = result.boxes[i];
        const cv::Point start(cvRound(box.x1), cvRound(box.y1)), end(cvRound(box.x2), cvRound(box.y2));
        const cv::Point center(cvRound((box.x1 + box.x2) * .5f), cvRound((box.y1 + box.y2) * .5f));
        const cv::Scalar color(120, 230, 70);
        cv::rectangle(annotated, start, end, color, 2, cv::LINE_AA);
        cv::drawMarker(annotated, center, color, cv::MARKER_CROSS, 12, 2, cv::LINE_AA);
        char label[48];
        std::snprintf(label, sizeof label, "%zu  %.0f%%", i + 1, box.confidence * 100);
        cv::putText(annotated, label, {std::max(0, start.x), std::max(16, start.y - 5)},
                    cv::FONT_HERSHEY_SIMPLEX, .5, color, 1, cv::LINE_AA);
    }
    return annotated;
}
fs::path chooseFile(GLFWwindow* window, bool save, const fs::path& initial = {}) {
    wchar_t buffer[32768]{};
    if (!initial.empty()) wcsncpy_s(buffer, initial.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = glfwGetWin32Window(window);
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = DWORD(std::size(buffer));
    dialog.lpstrTitle = save ? L"검출 결과 이미지 저장" : L"이미지 선택";
    dialog.lpstrFilter = save ? L"PNG 이미지\0*.png\0" : L"이미지\0*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.webp\0모든 파일\0*.*\0";
    dialog.lpstrDefExt = save ? L"png" : nullptr;
    dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))) return {};
    return fs::path(buffer);
}
struct Texture {
    GLuint id = 0;
    ~Texture() { if (id) glDeleteTextures(1, &id); }
    void update(const cv::Mat& source) {
        cv::Mat display;
        // Resize only the preview; inference and exported coordinates use original pixels.
        const double scale = std::min(1.0, 2400.0 / std::max(source.cols, source.rows));
        cv::resize(source, display, {}, scale, scale, cv::INTER_AREA);
        if (!id) glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, display.cols, display.rows, 0, GL_BGR, GL_UNSIGNED_BYTE, display.data);
    }
};
struct Application {
    Options options;
    GLFWwindow* window;
    std::shared_ptr<Engine> engine = std::make_shared<Engine>();
    BackgroundTask<Result> task;
    std::optional<fs::path> queued;
    std::optional<Result> result;
    Texture texture;
    std::string error, notice;
    bool overlay = true;

    Application(Options initial, GLFWwindow* target) : options(std::move(initial)), window(target) {}
    void request(const fs::path& path) {
        queued = path;
        error.clear();
        notice.clear();
    }
    static void dropped(GLFWwindow* target, int count, const char** paths) {
        auto* app = static_cast<Application*>(glfwGetWindowUserPointer(target));
        if (!app || count < 1) return;
        try {
            app->request(fromUtf8(paths[0]));
            if (count > 1) app->notice = "첫 번째 이미지를 열었습니다. 이미지 한 장씩 넣어 주세요.";
        } catch (const std::exception& problem) { app->error = problem.what(); }
    }
    void tick() {
        try {
            if (auto completed = task.poll(); completed && !queued) {
                result = std::move(*completed);
                texture.update(result->image);
            }
        } catch (const std::exception& problem) { error = problem.what(); }
        if (queued && !task.pending()) {
            const auto path = *queued;
            const auto config = options;
            const auto worker = engine;
            queued.reset();
            task.start([worker, path, config](std::stop_token) { return worker->run(path, config); });
        }
    }
    bool busy() const { return queued.has_value() || task.pending(); }
    void drawImage() {
        const auto room = ImGui::GetContentRegionAvail();
        if (!result) {
            ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), room.y * .38f));
            ImGui::TextWrapped("이미지 파일을 이 창에 드래그 앤 드롭하세요.");
            ImGui::TextDisabled("PNG · JPG · BMP · TIFF · WebP / 컬러·흑백 이미지");
            return;
        }
        const auto& source = result->image;
        const float scale = std::min(room.x / source.cols, room.y / source.rows);
        if (scale <= 0) return;
        const ImVec2 size(source.cols * scale, source.rows * scale);
        const ImVec2 origin(ImGui::GetCursorScreenPos().x + (room.x - size.x) * .5f,
                            ImGui::GetCursorScreenPos().y + (room.y - size.y) * .5f);
        ImGui::SetCursorScreenPos(origin);
        ImGui::Image(ImTextureRef(ImTextureID(texture.id)), size);
        if (!overlay) return;
        auto* draw = ImGui::GetWindowDrawList();
        const ImU32 color = IM_COL32(100, 235, 145, 255);
        draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
        for (size_t i = 0; i < result->boxes.size(); ++i) {
            const auto& box = result->boxes[i];
            const ImVec2 start(origin.x + box.x1 * scale, origin.y + box.y1 * scale);
            const ImVec2 end(origin.x + box.x2 * scale, origin.y + box.y2 * scale);
            const ImVec2 center((start.x + end.x) * .5f, (start.y + end.y) * .5f);
            draw->AddRect(start, end, color, 0, 0, 2);
            draw->AddLine({center.x - 5, center.y}, {center.x + 5, center.y}, color, 2);
            draw->AddLine({center.x, center.y - 5}, {center.x, center.y + 5}, color, 2);
            char label[48];
            std::snprintf(label, sizeof label, "%zu  %.0f%%", i + 1, box.confidence * 100);
            const ImVec2 textSize = ImGui::CalcTextSize(label);
            const ImVec2 anchor(std::clamp(start.x, origin.x, std::max(origin.x, origin.x + size.x - textSize.x - 8)),
                                std::max(origin.y, start.y - textSize.y - 7));
            draw->AddRectFilled(anchor, {anchor.x + textSize.x + 8, anchor.y + textSize.y + 4}, IM_COL32(15, 32, 25, 235), 3);
            draw->AddText({anchor.x + 4, anchor.y + 2}, color, label);
        }
        draw->PopClipRect();
    }
    void draw() {
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("이미지 공 검출", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("이미지 공 검출");
        ImGui::SameLine();
        ImGui::TextDisabled("학습한 보치아 YOLO 모델");
        if (ImGui::Button("이미지 열기")) {
            if (const auto path = chooseFile(window, false); !path.empty()) request(path);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!result || busy());
        if (ImGui::Button("다시 검출")) request(result->imagePath);
        ImGui::SameLine();
        if (ImGui::Button("결과 이미지 저장")) {
            try {
                auto initial = result->imagePath.parent_path() / (result->imagePath.stem().native() + fs::path("_balls.png").native());
                if (const auto path = chooseFile(window, true, initial); !path.empty()) {
                    requireSeparateOutput(result->imagePath, path);
                    writePng(path, annotatedImage(*result));
                    notice = "결과 이미지 저장: " + utf8(path);
                }
            } catch (const std::exception& problem) { error = problem.what(); }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("검출 표시", &overlay);
        if (busy()) ImGui::TextColored({.45f, .8f, 1, 1}, "이미지를 처리하고 있습니다...");
        else if (result) ImGui::TextWrapped("%s  /  %d × %d px", utf8(result->imagePath.filename()).c_str(), result->image.cols, result->image.rows);
        else ImGui::TextDisabled("파일을 드롭하면 자동으로 검출합니다.");
        if (!error.empty()) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored({1, .5f, .45f, 1}, "%s", error.c_str());
            ImGui::PopTextWrapPos();
        }
        if (!notice.empty()) ImGui::TextWrapped("%s", notice.c_str());
        ImGui::Separator();
        const float sidebar = 330;
        ImGui::BeginChild("image", {std::max(100.f, ImGui::GetContentRegionAvail().x - sidebar - 12), 0}, ImGuiChildFlags_Borders);
        drawImage();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("results", {0, 0}, ImGuiChildFlags_Borders);
        if (result) {
            ImGui::Text("공 %zu개", result->boxes.size());
            ImGui::SetWindowFontScale(1.65f);
            ImGui::TextColored({.45f, .9f, .65f, 1}, "%.2f ms", result->detectionMs);
            ImGui::SetWindowFontScale(1);
            ImGui::TextUnformatted("검출 처리 시간");
            ImGui::TextDisabled("전처리 + 추론 + 결과 좌표 변환");
            ImGui::Spacing();
            ImGui::Text("이미지 읽기: %.2f ms", result->readMs);
            if (result->preparationMs > 0) ImGui::Text("최초 모델 준비: %.1f ms", result->preparationMs);
            ImGui::Text("전체 처리: %.2f ms", result->totalMs);
            ImGui::TextDisabled("모델 준비·워밍업은 검출 시간에서 제외");
            ImGui::Separator();
            ImGui::TextWrapped("모델: %s", utf8(result->model.filename()).c_str());
            ImGui::Text("모델 입력: %d × %d", result->inputShape.width, result->inputShape.height);
            ImGui::TextWrapped("실행: %s", result->provider.c_str());
            ImGui::Text("검출 신뢰도 기준: %.2f", result->confidence);
        } else {
            ImGui::TextWrapped("모델: %s", utf8(options.model.filename()).c_str());
            ImGui::TextUnformatted("검출 후 공 개수와 처리 시간이 표시됩니다.");
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(busy());
        ImGui::SliderFloat("신뢰도", &options.confidence, .01f, 1.f, "%.2f");
        ImGui::Checkbox("GPU 사용", &options.cuda);
        ImGui::EndDisabled();
        ImGui::TextDisabled("변경 후 '다시 검출'을 누르세요.");
        ImGui::Separator();
        if (result && result->boxes.empty()) ImGui::TextWrapped("공을 찾지 못했습니다. 신뢰도를 낮춰 다시 검출할 수 있습니다.");
        if (result && ImGui::BeginTable("balls", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY, {0, std::max(100.f, ImGui::GetContentRegionAvail().y)})) {
            ImGui::TableSetupColumn("공", ImGuiTableColumnFlags_WidthFixed, 30);
            ImGui::TableSetupColumn("신뢰도");
            ImGui::TableSetupColumn("X (px)");
            ImGui::TableSetupColumn("Y (px)");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < result->boxes.size(); ++i) {
                const auto& box = result->boxes[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%zu", i + 1);
                ImGui::TableNextColumn(); ImGui::Text("%.1f%%", box.confidence * 100);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", (box.x1 + box.x2) * .5f);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", (box.y1 + box.y2) * .5f);
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::End();
    }
};
} // namespace

int main() {
    try {
        Options options = defaultOptions();
        fs::path image, output, screenshot;
        bool headless = false, hidden = false;
        int count = 0;
        auto* raw = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!raw) throw std::runtime_error("실행 옵션을 읽을 수 없습니다.");
        std::vector<std::wstring> args(raw, raw + count);
        LocalFree(raw);
        for (size_t i = 1; i < args.size(); ++i) {
            auto value = [&]() -> const std::wstring& {
                if (++i >= args.size()) throw std::runtime_error("옵션 값이 빠졌습니다.");
                return args[i];
            };
            if (args[i] == L"--image") image = value();
            else if (args[i] == L"--model") options.model = value();
            else if (args[i] == L"--cpu") options.cuda = false;
            else if (args[i] == L"--cuda") options.cuda = true;
            else if (args[i] == L"--confidence") options.confidence = std::stof(value());
            else if (args[i] == L"--headless") headless = true;
            else if (args[i] == L"--output") output = value();
            else if (args[i] == L"--smoke-hidden") hidden = true;
            else if (args[i] == L"--capture") screenshot = value();
            else if (args[i].starts_with(L"--")) throw std::runtime_error("알 수 없는 실행 옵션입니다.");
            else image = args[i]; // Also accepts dropping an image onto the executable itself.
        }
        if (!std::isfinite(options.confidence) || options.confidence < 0 || options.confidence > 1)
            throw std::runtime_error("신뢰도는 0~1 사이로 지정해 주세요.");
        yoloCpuThreadCount(options.threads);
        yoloCudaLibraryDirectory(options.cudaDlls);
        if (headless) {
            if (image.empty() || output.empty()) throw std::runtime_error("--headless에는 --image와 --output이 필요합니다.");
            const auto annotatedPath = output.parent_path() / (output.stem().native() + fs::path("_balls.png").native());
            requireSeparateOutput(image, output);
            requireSeparateOutput(image, annotatedPath);
            Engine engine;
            const auto result = engine.run(image, options);
            if (!output.parent_path().empty()) fs::create_directories(output.parent_path());
            std::ofstream file(output);
            file << resultJson(result).dump(2) << '\n';
            if (!file) throw std::runtime_error("검출 결과를 저장할 수 없습니다.");
            writePng(annotatedPath, annotatedImage(result));
            std::cout << "balls=" << result.boxes.size() << " detection_ms=" << result.detectionMs << '\n';
            return 0;
        }
        if (!glfwInit()) throw std::runtime_error("화면을 초기화할 수 없습니다.");
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        if (hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        auto* window = glfwCreateWindow(1320, 840, "Boccia - Image Ball Detector", nullptr, nullptr);
        if (!window) { glfwTerminate(); throw std::runtime_error("검출 창을 열 수 없습니다."); }
        glfwSetWindowSizeLimits(window, 960, 620, GLFW_DONT_CARE, GLFW_DONT_CARE);
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);
        if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) throw std::runtime_error("OpenGL을 초기화할 수 없습니다.");
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        const auto bundledFont = executableDirectory() / "fonts/NotoSansKR.ttf";
        const auto sourceFont = fromUtf8(BOCCIA_DATA_DIR).parent_path() / "assets/fonts/NotoSansKR.ttf";
        if (fs::exists(bundledFont)) io.Fonts->AddFontFromFileTTF(utf8(bundledFont).c_str(), 18, nullptr, io.Fonts->GetGlyphRangesKorean());
        else if (fs::exists(sourceFont)) io.Fonts->AddFontFromFileTTF(utf8(sourceFont).c_str(), 18, nullptr, io.Fonts->GetGlyphRangesKorean());
        else if (fs::exists("C:/Windows/Fonts/malgun.ttf")) io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/malgun.ttf", 18, nullptr, io.Fonts->GetGlyphRangesKorean());
        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        style.WindowPadding = {16, 14};
        style.FramePadding = {10, 6};
        style.ItemSpacing = {10, 8};
        style.FrameRounding = 5;
        style.ChildRounding = 6;
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330");
        int status = 0;
        {
            Application app(options, window);
            glfwSetWindowUserPointer(window, &app);
            glfwSetDropCallback(window, Application::dropped);
            if (!image.empty()) {
                const auto name = utf8(image);
                const char* droppedPaths[] = {name.c_str()};
                Application::dropped(window, 1, droppedPaths); // Same route as an actual file drop.
            }
            const auto started = Clock::now();
            int settled = 0;
            while (!glfwWindowShouldClose(window)) {
                glfwPollEvents();
                app.tick();
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                app.draw();
                ImGui::Render();
                int width = 0, height = 0;
                glfwGetFramebufferSize(window, &width, &height);
                glViewport(0, 0, width, height);
                glClearColor(.06f, .07f, .09f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                if (hidden && !app.busy() && ++settled >= 8) {
                    if (!app.error.empty()) { std::cerr << app.error << '\n'; status = 4; }
                    if (!screenshot.empty()) {
                        cv::Mat pixels(height, width, CV_8UC3);
                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                        glReadPixels(0, 0, width, height, GL_BGR, GL_UNSIGNED_BYTE, pixels.data);
                        cv::flip(pixels, pixels, 0);
                        writePng(screenshot, pixels);
                    }
                    if (glGetError() != GL_NO_ERROR) status = 5;
                    if (!output.empty() && app.result) {
                        if (!output.parent_path().empty()) fs::create_directories(output.parent_path());
                        std::ofstream file(output);
                        file << resultJson(*app.result).dump(2) << '\n';
                        if (!file) status = 4;
                    }
                    glfwSetWindowShouldClose(window, true);
                }
                if (hidden && elapsedMs(started) > 60000) { status = 6; glfwSetWindowShouldClose(window, true); }
                glfwSwapBuffers(window);
            }
            app.task.wait();
            glfwSetWindowUserPointer(window, nullptr);
        } // Destroy the image texture while its OpenGL context is still current.
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return status;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        // Command-line checks report a nonzero exit; ordinary launches explain startup failures in a dialog.
        if (std::wstring(GetCommandLineW()).find(L"--") == std::wstring::npos)
            MessageBoxW(nullptr, fromUtf8(error.what()).c_str(), L"이미지 공 검출", MB_OK | MB_ICONERROR);
        return 4;
    }
}
