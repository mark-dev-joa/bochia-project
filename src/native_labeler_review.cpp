// Review screen of the v2 design: scene list, three camera tiles that enlarge in place, and the side panel.
#include "native_labeler_app.hpp"
#include "native_labeler_theme.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace boccia::labeler {
namespace {
std::string padded(int value, int width) { auto text = std::to_string(value); return std::string(size_t(std::max(0, width - int(text.size()))), '0') + text; }
// The ball colour the colour camera saw, as a swatch; 0 when unknown.
ImU32 colorSwatch(const std::string& color) {
    return color == "red" ? theme::rgb(0xe03b3b) : color == "blue" ? theme::rgb(0x3b6fe0) : color == "white" ? theme::rgb(0xf2f2f2) : color == "black" ? theme::rgb(0x1a1a1a) : 0;
}
const char* colorName(const std::string& color) {
    return color == "red" ? "빨강" : color == "blue" ? "파랑" : color == "white" ? "흰색" : color == "black" ? "검정" : "";
}
// A ball's colour as a small dot with a dark rim, so a white or black dot stays visible on any background.
void colorDot(ImDrawList* draw, ImVec2 center, ImU32 swatch, float radius = 4.5f) {
    draw->AddCircleFilled(center, radius, swatch);
    draw->AddCircle(center, radius, theme::line2, 0, 1.f);
}
// The number badge above each ball. On the colour camera it carries the ball's colour as a dot, and its name where
// there is room: "1 ● 빨강" on the editor canvas, "1 ●" on the small tiles.
void ballBadges(ImDrawList* draw, const Json& boxes, const std::vector<int>& order, const std::function<ImVec2(double, double)>& at, bool colourCamera, bool withName, int skip = -1) {
    std::vector<ImVec4> placed;   // badges already drawn, so neighbours stack upward instead of covering each other
    for (int rank = 0; rank < int(order.size()); ++rank) {
        const int ball = order[rank];
        if (ball == skip) continue;
        const auto r = detectionBox(boxes[ball]);
        const std::string number = std::to_string(rank + 1), color = boxes[ball].value("color", "unknown");
        const ImU32 swatch = colourCamera ? colorSwatch(color) : 0;
        const std::string name = swatch && withName ? colorName(color) : "";
        const float numberWidth = theme::extent(number, 11, theme::bold).x, dotWidth = swatch ? 13.f : 0.f, nameWidth = name.empty() ? 0.f : theme::extent(name, 11).x + 3;
        const ImVec2 top = at(r.x + r.width / 2, r.y);
        ImVec2 center{top.x, top.y - 12};
        const float w = std::max(16.f, numberWidth + dotWidth + nameWidth + 8);
        auto overlaps = [&] { for (const auto& p : placed) if (center.x - w / 2 < p.z + 2 && center.x + w / 2 > p.x - 2 && center.y - 8 < p.w + 2 && center.y + 8 > p.y - 2) return true; return false; };
        for (int lift = 0; lift < 8 && overlaps(); ++lift) center.y -= 18;
        placed.push_back({center.x - w / 2, center.y - 8, center.x + w / 2, center.y + 8});
        draw->AddRectFilled({center.x - w / 2, center.y - 8}, {center.x + w / 2, center.y + 8}, theme::rgb(0x0d0f12, 210), 8);
        float tx = center.x - (numberWidth + dotWidth + nameWidth) / 2;
        theme::write(draw, {tx, center.y - 7}, theme::mask, number, 11, theme::bold); tx += numberWidth;
        if (swatch) { colorDot(draw, {tx + 8, center.y}, swatch, 4); tx += dotWidth; }
        if (!name.empty()) theme::write(draw, {tx + 3, center.y - 7}, theme::text, name, 11);
    }
}
const char* sourceTag(const Json& box) {
    const auto source = box.value("source", "manual");
    return source == "auto_sam" || source == "efficientsam" ? "SAM" : source == "auto_tracking" || source == "tracking" ? "추적" : source == "circle" || source == "auto_geometry" ? "원형" : "수동";
}
const char* sourceName(const Json& box) {
    const auto source = box.value("source", "manual");
    return source == "auto_sam" ? "자동 분할 (SAM)" : source == "auto_tracking" ? "자동 추적" : source == "efficientsam" ? "클릭 분할" : source == "manual_brush" ? "브러시" : source == "manual_polygon" ? "폴리곤" : "수동 박스";
}
void dashedRect(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 color) {
    const float dash = 4, gap = 3;
    for (float x = a.x; x < b.x; x += dash + gap) { const float end = std::min(x + dash, b.x); draw->AddLine({x, a.y}, {end, a.y}, color); draw->AddLine({x, b.y}, {end, b.y}, color); }
    for (float y = a.y; y < b.y; y += dash + gap) { const float end = std::min(y + dash, b.y); draw->AddLine({a.x, y}, {a.x, end}, color); draw->AddLine({b.x, y}, {b.x, end}, color); }
}
std::string sizeText(const cv::Rect2d& box) { return std::to_string(int(std::lround(box.width))) + "×" + std::to_string(int(std::lround(box.height))); }
}

std::string Application::cameraLabel(const Json& member) {
    const auto name = member.value("name", ""), kind = member.value("kind", "");
    const auto space = name.rfind(' ');
    return kind + " " + (space == std::string::npos ? name : name.substr(space + 1));
}
std::vector<int> Application::sorted(const FrameView& frame) const {
    const auto& boxes = frame.annotation["boxes"];
    std::vector<int> order(boxes.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return detectionBox(boxes[a]).x < detectionBox(boxes[b]).x; });
    return order;
}

// ---- scene list -----------------------------------------------------------------------------

// Rows are precomputed so the list does not search the catalog JSON for every scene on every frame.
void Application::rebuildRows() {
    rows_.clear(); rowsDirty_ = false;
    if (!catalog_.is_object()) return;
    int recording = -1, headerAt = -1; long long first = 0;
    const auto& scenes = catalog_["scenes"];
    for (int i = 0; i < int(scenes.size()); ++i) {
        const auto& scene = scenes[i];
        SceneRow row; row.scene = i; row.recording = scene.value("recording_number", 0); row.index = scene.value("index", 0);
        const long long time = std::stoll(scene.value("host_ns", "0"));
        if (row.recording != recording) { recording = row.recording; first = time; }
        row.seconds = float((time - first) / 1e9);
        bool target = false, todo = false, held = false, done = true, attention = false;
        for (const auto& member : scene["members"]) {
            if (row.cameras >= 3) break;
            const auto state = memberState(member);
            row.state[row.cameras] = state; row.count[row.cameras] = member.value("count", 0); ++row.cameras;
            if (state == MemberState::Duplicate) continue;
            target = true; todo |= state == MemberState::Todo || state == MemberState::Editing; held |= state == MemberState::Hold;
            done &= state == MemberState::Done; attention |= member.value("attention", false);
        }
        if (!row.cameras || (representatives_ && !target)) continue;
        if ((filter_ == 1 && !todo) || (filter_ == 2 && !held) || (filter_ == 3 && !(target && done))) continue;
        if (headerAt < 0 || rows_[headerAt].recording != row.recording) { SceneRow header; header.header = true; header.recording = row.recording; headerAt = int(rows_.size()); rows_.push_back(header); }
        ++rows_[headerAt].scenes;
        row.warn = attention || countsDiffer(scene);
        const auto& lead = scene["members"][0];
        row.thumb = lead.value("key", ""); row.color = lead.value("kind", "") == "컬러";
        rows_.push_back(std::move(row));
    }
}
// Previous or next scene in the list as filtered, so hidden repeats are skipped.
void Application::stepRow(int step) {
    if (rowsDirty_) rebuildRows();
    int at = -1;
    for (int i = 0; i < int(rows_.size()); ++i) if (!rows_[i].header && rows_[i].scene == scene_) { at = i; break; }
    if (at < 0) {
        for (int i = 0; i < int(rows_.size()); ++i) if (!rows_[i].header && (step > 0 ? rows_[i].scene > scene_ : rows_[i].scene < scene_)) { at = i; if (step > 0) break; }
        if (at >= 0) navigate(rows_[at].scene);
        return;
    }
    for (int i = at + step; i >= 0 && i < int(rows_.size()); i += step) if (!rows_[i].header) { navigate(rows_[i].scene); return; }
}
GLuint Application::thumb(const std::string& key, bool color) {
    const auto [found, added] = thumbs_.emplace(key, 0);   // 0 until the image arrives; each one is asked for once
    if (added) thumbWanted_.emplace_back(key, color);
    return found->second;
}
// Thumbnails load on their own future so they never block scene navigation.
void Application::pumpThumbs() {
    if (thumbTask_.valid() && thumbTask_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            for (auto& [key, image] : thumbTask_.get()) {
                GLuint id = 0;
                if (!image.empty()) uploadTexture(id, image);
                thumbs_[key] = id;
            }
        } catch (...) {}
    }
    if (thumbTask_.valid() || thumbWanted_.empty() || !catalog_.is_object()) return;
    const size_t count = std::min<size_t>(12, thumbWanted_.size());
    std::vector<std::pair<std::string, bool>> batch(thumbWanted_.end() - count, thumbWanted_.end());
    thumbWanted_.resize(thumbWanted_.size() - count);
    const auto engine = engine_; const float gain = brightness_;
    thumbTask_ = std::async(std::launch::async, [engine, batch, gain] {
        Thumbs result;
        for (const auto& [key, color] : batch) {
            try { cv::Mat image = engine->thumbnail(key), shown; image.convertTo(shown, CV_8UC3, color ? gain : 1.12); result.emplace_back(key, shown); }
            catch (...) { result.emplace_back(key, cv::Mat()); }
        }
        return result;
    });
}
void Application::queue() {
    if (rowsDirty_) rebuildRows();
    auto draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(avail);
    const float width = avail.x;
    draw->AddLine({origin.x + width - .5f, origin.y}, {origin.x + width - .5f, origin.y + avail.y}, theme::line);
    theme::write(draw, {origin.x + 12, origin.y + 10}, theme::text, "장면 목록", theme::base, theme::bold);
    int total = 0; for (const auto& row : rows_) total += !row.header;
    const std::string count = "장면 " + grouped(total) + "개";
    theme::write(draw, {origin.x + width - 40 - theme::extent(count, theme::caption).x, origin.y + 12}, theme::text3, count, theme::caption);
    ImGui::SetCursorScreenPos({origin.x + width - 34, origin.y + 8});
    if (ImGui::InvisibleButton("##fold", {22, 26})) listOpen_ = false;
    if (ImGui::IsItemHovered()) { draw->AddRectFilled({origin.x + width - 34, origin.y + 8}, {origin.x + width - 12, origin.y + 34}, theme::bg3, 4); ImGui::SetTooltip("목록 접기"); }
    theme::icon(draw, theme::Icon::Left, {origin.x + width - 23, origin.y + 21}, 11, theme::text2);
    static const char* filters[] = {"전체", "미검수", "보류", "확인됨"};
    ImGui::SetCursorScreenPos({origin.x + 12, origin.y + 38});
    if (theme::segmented("##filter", filters, 4, &filter_, nullptr, 24, width - 25)) { rowsDirty_ = true; scrollToCurrent_ = true; }
    ImGui::SetCursorScreenPos({origin.x + 12, origin.y + 74});
    if (theme::checkbox("반복 장면 숨김", &representatives_)) { rowsDirty_ = true; scrollToCurrent_ = true; }
    draw->AddLine({origin.x, origin.y + 103.5f}, {origin.x + width - 1, origin.y + 103.5f}, theme::line);

    ImGui::SetCursorScreenPos({origin.x, origin.y + 104});
    ImGui::BeginChild("SceneRows", {width - 1, avail.y - 104 - 45});
    auto list = ImGui::GetWindowDrawList();
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const float top = ImGui::GetScrollY(), view = ImGui::GetWindowHeight(), rowWidth = ImGui::GetContentRegionAvail().x;
    float y = 0;
    for (int i = 0; i < int(rows_.size()); ++i) {
        const auto& row = rows_[i];
        const float height = row.header ? 30.f : 54.f;
        if (!row.header && row.scene == scene_ && scrollToCurrent_) { ImGui::SetScrollY(std::max(0.f, y - view / 2 + height / 2)); scrollToCurrent_ = false; }
        if (y + height >= top && y <= top + view) {
            const ImVec2 p{base.x, base.y + y};
            if (row.header) {
                list->AddRectFilled(p, {p.x + rowWidth, p.y + height}, theme::bg2);
                theme::icon(list, theme::Icon::Down, {p.x + 18, p.y + 15}, 9, theme::text3);
                const std::string name = "촬영 " + padded(row.recording, 2);
                theme::write(list, {p.x + 32, p.y + 7}, theme::text, name, theme::sub, theme::bold);
                theme::write(list, {p.x + 40 + theme::extent(name, theme::sub, theme::bold).x, p.y + 8}, theme::text3, std::to_string(row.scenes) + "장면", theme::caption);
                static const char* letters[] = {"C", "A", "B"};
                for (int camera = 0; camera < 3; ++camera) theme::write(list, {p.x + rowWidth - 12 - 5 - (2 - camera) * 14 - 3, p.y + 9}, theme::text3, letters[camera], 10, theme::mono);
            } else {
                ImGui::SetCursorScreenPos(p); ImGui::PushID(i);
                const bool clicked = ImGui::InvisibleButton("##row", {rowWidth, height}), hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                const bool here = row.scene == scene_;
                if (here) { list->AddRectFilled(p, {p.x + rowWidth, p.y + height}, theme::rgb(0x5b9dff, 64)); list->AddRect({p.x + .5f, p.y + .5f}, {p.x + rowWidth - .5f, p.y + height - .5f}, theme::rgb(0x5b9dff, 150)); list->AddRectFilled(p, {p.x + 4, p.y + height}, theme::accent); }
                else if (hovered) list->AddRectFilled(p, {p.x + rowWidth, p.y + height}, theme::bg2);
                const ImVec2 a{p.x + 12, p.y + 4.5f}, b{a.x + 60, a.y + 45};
                if (const GLuint id = thumb(row.thumb, row.color)) list->AddImageRounded(ImTextureRef(ImTextureID(id)), a, b, {0, 0}, {1, 1}, IM_COL32_WHITE, 3);
                else list->AddRectFilled(a, b, theme::bg3, 3);
                const std::string frame = "프레임 " + padded(row.index, 4);
                theme::write(list, {p.x + 82, p.y + 9}, theme::text, frame, theme::sub, here ? theme::bold : nullptr);
                char seconds[24]; std::snprintf(seconds, sizeof seconds, "%.2f초", row.seconds);
                if (rowWidth > 260) theme::write(list, {p.x + 88 + theme::extent(frame, theme::sub).x, p.y + 11}, theme::text3, seconds, 11);
                float x = p.x + 82;
                if (row.warn) { theme::warning(list, {x + 6, p.y + 37}, 11, theme::warn); x += 17; }
                std::string balls = "공 ";
                for (int camera = 0; camera < row.cameras; ++camera) balls += (camera ? " · " : "") + std::to_string(row.count[camera]);
                theme::write(list, {x, p.y + 29}, theme::text2, balls, theme::caption);
                for (int camera = 0; camera < row.cameras; ++camera) theme::dot(list, {p.x + rowWidth - 12 - 5 - (2 - camera) * 14, p.y + 27}, row.state[camera]);
                list->AddLine({p.x, p.y + height - .5f}, {p.x + rowWidth, p.y + height - .5f}, theme::line);
                if (clicked) { const int scene = row.scene; later_ = [this, scene] { navigate(scene); }; }
            }
        }
        y += height;
    }
    ImGui::SetCursorPos({0, y}); ImGui::Dummy({1, 1});
    ImGui::EndChild();

    const float bottom = origin.y + avail.y - 45;
    draw->AddLine({origin.x, bottom + .5f}, {origin.x + width - 1, bottom + .5f}, theme::line);
    ImGui::PushFont(nullptr, theme::px(theme::caption));
    const float each = (width - 21 - 12) / 3.5f;
    ImGui::SetCursorScreenPos({origin.x + 10, bottom + 9});
    const bool keys = width >= 260;   // the narrow list has no room for the key letters; the hint bar still shows them
    if (ImGui::Button(keys ? "이전  A" : "이전", {each, 28})) later_ = [this] { stepRow(-1); };
    ImGui::SameLine(0, 6); if (ImGui::Button(keys ? "다음  D" : "다음", {each, 28})) later_ = [this] { stepRow(1); };
    ImGui::SameLine(0, 6); if (ImGui::Button(keys ? "다음 미검수  N" : "다음 미검수", {each * 1.5f, 28})) later_ = [this] { if (guard()) nextUnreviewed(); };
    ImGui::PopFont();
}

// ---- image painting -------------------------------------------------------------------------

// Contours and boxes of one frame. `at` maps original image coordinates to the screen.
void Application::paintMarks(FrameView& frame, const std::function<ImVec2(double, double)>& at, Paint paint) {
    auto draw = ImGui::GetWindowDrawList();
    const auto& boxes = frame.annotation["boxes"];
    for (int i = 0; i < int(boxes.size()); ++i) {
        if (i == paint.skip) continue;
        const bool active = i == paint.selected;
        const ImU32 color = active ? theme::selected : theme::mask;
        if (paint.outlines && i < int(frame.contours.size())) for (const auto& contour : frame.contours[i]) {
            if (contour.size() < 3) continue;
            std::vector<ImVec2> points; points.reserve(contour.size());
            for (const auto& p : contour) points.push_back(at(p.x + .5, p.y + .5));
            draw->AddPolyline(points.data(), int(points.size()), color, ImDrawFlags_Closed, active ? 2.f : 1.5f);
        }
        if (paint.circles && i < int(frame.circles.size()) && frame.circles[i].r > 0) {   // the ball as a circle, fitted to the mask
            const auto& c = frame.circles[i];
            const float scale = at(1, 0).x - at(0, 0).x;
            draw->AddCircle(at(c.x + .5, c.y + .5), float(c.r) * scale, theme::circle, 0, 1.5f);
            draw->AddCircleFilled(at(c.x + .5, c.y + .5), 2, theme::circle);
        }
        const auto r = detectionBox(boxes[i]);
        const bool corrected = boxes[i].contains("detection_bbox"), boxOnly = !boxes[i].contains("segmentation");
        if (active && corrected) {
            const auto& tight = boxes[i]["bbox"];
            const double x = tight[0].get<double>(), y = tight[1].get<double>();
            dashedRect(draw, at(x, y), at(x + tight[2].get<double>(), y + tight[3].get<double>()), theme::mask);
        }
        if (paint.boxes || active || boxOnly) {   // a measured or corrected box follows the toggle like any other
            draw->AddRect(at(r.x, r.y), at(r.x + r.width, r.y + r.height), IM_COL32(0, 0, 0, 150), 0, 0, 3);
            draw->AddRect(at(r.x, r.y), at(r.x + r.width, r.y + r.height), boxOnly && !active ? theme::mask : IM_COL32_WHITE, 0, 0, 1);
        }
    }
}
// Draws the part `region` of the frame into the screen rectangle [a, b]. Crops reuse the full texture.
void Application::paintImage(FrameView& frame, ImVec2 a, ImVec2 b, cv::Rect2d region, Paint paint) {
    if (frame.image.empty()) return;
    if (frame.repaint) repaint(frame);
    auto draw = ImGui::GetWindowDrawList();
    const ImVec2 uv0{float(region.x / frame.image.cols), float(region.y / frame.image.rows)};
    const ImVec2 uv1{float((region.x + region.width) / frame.image.cols), float((region.y + region.height) / frame.image.rows)};
    const double scale = (b.x - a.x) / region.width;
    draw->PushClipRect(a, b, true);
    draw->AddImage(ImTextureRef(ImTextureID(frame.texture)), a, b, uv0, uv1);
    if (paint.fill && masks_ && outlines_) draw->AddImage(ImTextureRef(ImTextureID(frame.overlay)), a, b, uv0, uv1);
    paintMarks(frame, [&](double x, double y) { return ImVec2{a.x + float((x - region.x) * scale), a.y + float((y - region.y) * scale)}; }, paint);
    draw->PopClipRect();
}

// ---- tiles ----------------------------------------------------------------------------------

void Application::reviewPane() {
    const ImVec2 origin = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(avail);
    const float x = origin.x + 12, width = avail.x - 24;
    sceneBar({x, origin.y + 10}, width);
    const float top = origin.y + 58, height = avail.y - 58 - 10;
    const auto spans = tileSpans(editor_ ? camera_ : -1, width);
    for (int slot = 0; slot < 3; ++slot) {
        const ImVec2 pos{x + spans[slot].x, top}, size{spans[slot].width, height};
        ImGui::PushID(slot);
        if (slot >= int(frames_.size())) emptyTile(pos, size);
        else if (!editor_) overviewTile(slot, pos, size);
        else if (slot == camera_) focusTile(slot, pos, size);
        else smallTile(slot, pos, size);
        ImGui::PopID();
    }
}
void Application::sceneBar(ImVec2 pos, float width) {
    auto draw = ImGui::GetWindowDrawList();
    const auto& scene = catalog_["scenes"][scene_];
    const std::string recording = "촬영 " + padded(scene.value("recording_number", 0), 2), frame = "프레임 " + padded(scene.value("index", 0), 4);
    float x = pos.x + 2;
    ImGui::SetCursorScreenPos({x, pos.y + 7});
    if (ImGui::InvisibleButton("##back", {26, 26}) && !history_.empty()) later_ = [this] { back(); };
    if (ImGui::IsItemHovered() && !history_.empty()) { draw->AddRectFilled({x, pos.y + 7}, {x + 26, pos.y + 33}, theme::bg3, 5); ImGui::SetTooltip("이전에 보던 장면으로 (Backspace)"); }
    theme::icon(draw, theme::Icon::Left, {x + 13, pos.y + 20}, 12, history_.empty() ? theme::line2 : theme::text2);
    x += 34;
    theme::write(draw, {x, pos.y + 10}, theme::text3, recording, theme::title); x += theme::extent(recording, theme::title).x + 8;
    theme::icon(draw, theme::Icon::Right, {x + 5, pos.y + 21}, 11, theme::text3); x += 18;
    theme::write(draw, {x, pos.y + 10}, theme::text, frame, theme::title, theme::bold); x += theme::extent(frame, theme::title, theme::bold).x + 12;
    int balls = 0; double delta = 0;
    for (const auto& view : frames_) { balls += int(view.annotation["boxes"].size()); delta = std::max(delta, std::abs(view.member.value("delta_ms", 0.))); }
    char meta[128]; std::snprintf(meta, sizeof meta, "카메라 %d대 · 공 %d개 · 수신 시각 차 최대 %.1fms", int(frames_.size()), balls, delta);
    float right = pos.x + width;
    ImGui::PushFont(nullptr, theme::px(theme::caption));
    ImGui::SetCursorScreenPos({right - 92, pos.y + 8}); ImGui::SetNextItemWidth(92);
    ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);   // Tab cycles the balls; ImGui would otherwise turn this slider into a text field
    if (ImGui::SliderFloat("##brightness", &brightness_, .5f, 3.f, "")) for (auto& view : frames_) view.repaint = true;
    ImGui::PopItemFlag();
    ImGui::PopFont();
    right -= 100 + theme::extent("표시 밝기", theme::caption).x;
    theme::write(draw, {right, pos.y + 12}, theme::text3, "표시 밝기", theme::caption);
    auto option = [&](const char* label, bool& value) {
        const float w = 21 + theme::extent(label, theme::caption).x;
        right -= w + 16;
        ImGui::SetCursorScreenPos({right, pos.y + 11});
        theme::checkbox(label, &value);
    };
    right -= 6;
    option("원", circles_);
    option("박스", boxes_);
    option("마스크 채움", masks_);
    option("윤곽", outlines_);
    // How the frames are shown: the original, or an intermediate of the image processing (I cycles).
    right -= 122;
    ImGui::SetCursorScreenPos({right, pos.y + 8}); ImGui::SetNextItemWidth(108);
    ImGui::PushFont(nullptr, theme::px(theme::caption));
    ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);
    if (ImGui::Combo("##view", &view_, viewNames, viewCount)) for (auto& view : frames_) view.repaint = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("보기 모드 (I)");
    ImGui::PopItemFlag();
    ImGui::PopFont();
    draw->PushClipRect({x, pos.y}, {right - 10, pos.y + 40}, true);   // narrow windows cut the summary instead of running under the toggles
    theme::write(draw, {x, pos.y + 13}, theme::text3, meta, theme::caption);
    draw->PopClipRect();
}
void Application::emptyTile(ImVec2 pos, ImVec2 size) {
    auto draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, theme::bg1, 8);
    draw->AddRect(pos, {pos.x + size.x, pos.y + size.y}, theme::line, 8);
    theme::write(draw, {pos.x + 12, pos.y + 10}, theme::text2, "이 시각의 영상 없음");
    theme::write(draw, {pos.x + 12, pos.y + 32}, theme::text3, "수신 시각 20ms 안에 맞는 프레임이 없습니다.", theme::caption);
}
void Application::tileHeader(int slot, ImVec2 pos, float width, bool compact, const std::string& meta) {
    auto draw = ImGui::GetWindowDrawList();
    const auto& frame = frames_[slot];
    float x = pos.x + (compact ? 8 : 10);
    x += theme::keycapAt(draw, {x, pos.y + 8}, std::to_string(slot + 1).c_str()) + (compact ? 6 : 8);
    const auto name = cameraLabel(frame.member);
    theme::write(draw, {x, pos.y + 8}, theme::text, name, theme::base, theme::bold);
    x += theme::extent(name, theme::base, theme::bold).x + 8;
    const auto state = memberState(frame.member);
    if (compact) theme::dot(draw, {pos.x + width - 14, pos.y + 17}, state);
    else {
        const float chip = theme::chipWidth(state);
        draw->PushClipRect({x, pos.y}, {pos.x + width - chip - 18, pos.y + 34}, true);
        theme::write(draw, {x, pos.y + 10}, theme::text3, meta, theme::caption);
        draw->PopClipRect();
        ImGui::SetCursorScreenPos({pos.x + width - 10 - chip, pos.y + 6});
        if (theme::chip("##chip", state) && state != MemberState::Done && state != MemberState::Duplicate) later_ = [this, slot] { review(Scope::Camera, false, slot); };
    }
    draw->AddLine({pos.x, pos.y + 33.5f}, {pos.x + width, pos.y + 33.5f}, theme::line);
}
// Whole image of one camera. A click on a ball selects it, a click elsewhere enlarges the camera.
void Application::viewImage(int slot, ImVec2 a, ImVec2 b, bool compact) {
    auto& frame = frames_[slot];
    auto draw = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(a);
    const bool clicked = ImGui::InvisibleButton("##view", {b.x - a.x, b.y - a.y}), hovered = ImGui::IsItemHovered();
    const bool here = editor_ && slot == camera_;
    paintImage(frame, a, b, {0, 0, double(frame.image.cols), double(frame.image.rows)}, {.fill = true, .boxes = boxes_, .circles = circles_, .outlines = outlines_, .selected = here ? selected_ : -1});
    const float scale = (b.x - a.x) / frame.image.cols;
    if (!compact) ballBadges(draw, frame.annotation["boxes"], sorted(frame), [&](double x, double y) { return ImVec2{a.x + float(x) * scale, a.y + float(y) * scale}; }, frame.member.value("kind", "") == "컬러", false);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const cv::Point2d point{(mouse.x - a.x) / scale, (mouse.y - a.y) / scale};
    if (clicked) { const int ball = hit(frame, point); later_ = [this, slot, ball] { if (ball >= 0) select(slot, ball); else focus(slot); }; }
    else if (hovered && !compact) {
        // Loupe: 44 original pixels shown at 4x next to the cursor, to find balls the draft missed.
        const double side = 44;
        const cv::Rect2d region{std::clamp(point.x - side / 2, 0., frame.image.cols - side), std::clamp(point.y - side / 2, 0., frame.image.rows - side), side, side};
        auto top = ImGui::GetForegroundDrawList();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImVec2 p{mouse.x + 18, mouse.y + 18};
        if (p.x + 176 > display.x) p.x = mouse.x - 194;
        if (p.y + 176 > display.y) p.y = mouse.y - 194;
        const ImVec2 q{p.x + 176, p.y + 176};
        top->AddImage(ImTextureRef(ImTextureID(frame.texture)), p, q, {float(region.x / frame.image.cols), float(region.y / frame.image.rows)},
                      {float((region.x + side) / frame.image.cols), float((region.y + side) / frame.image.rows)});
        top->AddRect(p, q, theme::rgb(0x0d0f12, 200), 0, 0, 4); top->AddRect(p, q, theme::line2, 0, 0, 1);
    }
}
// Zoomed crops of every ball, two per row. Contours are checked here; the full image is for missing balls.
void Application::cropGrid(int slot, ImVec2 pos, ImVec2 size, bool compact) {
    auto& frame = frames_[slot];
    auto draw = ImGui::GetWindowDrawList();
    const auto order = sorted(frame);
    const auto& boxes = frame.annotation["boxes"];
    const float pad = compact ? 6.f : 10.f, gap = compact ? 6.f : 10.f, caption = compact ? 18.f : 22.f;
    const int cells = int(order.size()) + (compact ? 0 : 1);
    if (!cells || size.y < 70) return;
    const int rows = (cells + 1) / 2;
    const float cell = std::max(36.f, std::min(std::floor((size.x - 2 * pad - gap) / 2), (size.y - 2 * pad - (rows - 1) * gap) / rows - caption));
    const bool here = editor_ && slot == camera_;
    for (int i = 0; i < cells; ++i) {
        const ImVec2 a{pos.x + pad + (i % 2) * (cell + gap), pos.y + pad + (i / 2) * (cell + caption + gap)}, b{a.x + cell, a.y + cell};
        if (b.y + caption > pos.y + size.y + 1) break;
        ImGui::SetCursorScreenPos(a); ImGui::PushID(i);
        ImGui::SetNextItemAllowOverlap();   // the delete button sits in the corner of the crop
        const bool clicked = ImGui::InvisibleButton("##crop", {cell, cell + caption}), hovered = ImGui::IsItemHovered();
        bool removing = false;
        if (i < int(order.size()) && cell > 70) {
            ImGui::SetCursorScreenPos({b.x - 26, a.y + 4});
            const bool asked = ImGui::InvisibleButton("##remove", {22, 22});
            removing = ImGui::IsItemHovered();
            if (asked) pendingRemove_ = {slot, order[i]};
        }
        ImGui::PopID();
        if (i >= int(order.size())) {
            dashedRect(draw, a, b, hovered ? theme::text3 : theme::line2);
            const bool roomy = cell > 120;
            theme::icon(draw, theme::Icon::Plus, {a.x + cell / 2, a.y + cell / 2 - (roomy ? 18 : 0)}, 14, theme::text3);
            if (roomy) theme::write(draw, {a.x + (cell - theme::extent("공 추가", theme::caption).x) / 2, a.y + cell / 2}, theme::text3, "공 추가", theme::caption);
            if (clicked) later_ = [this, slot] { focus(slot); if (editor_ && camera_ == slot) tool_ = Tool::Ai; };
            continue;
        }
        const int ball = order[i];
        const auto r = detectionBox(boxes[ball]);
        const bool active = here && ball == selected_;
        paintImage(frame, a, b, cropRegion(r, frame.image.cols, frame.image.rows), {.fill = false, .boxes = boxes_, .circles = circles_, .outlines = outlines_, .selected = active ? ball : -1});
        draw->AddRect(a, b, active ? theme::selected : hovered ? theme::text3 : theme::line, 4, 0, active ? 2.f : 1.f);
        if (cell > 70) {   // drawn after the image, which would otherwise cover it
            draw->AddRectFilled({b.x - 26, a.y + 4}, {b.x - 4, a.y + 26}, theme::rgb(0x0d0f12, removing ? 235 : 150), 5);
            theme::icon(draw, theme::Icon::Trash, {b.x - 15, a.y + 15}, 12, removing ? theme::warn : hovered ? theme::text : theme::text2);
            if (removing) ImGui::SetTooltip("이 공 삭제");
        }
        // The colour camera's crop names the ball's colour in its corner, the same chip as the badge on the image.
        if (const ImU32 swatch = frame.member.value("kind", "") == "컬러" ? colorSwatch(boxes[ball].value("color", "unknown")) : 0) {
            const std::string name = cell > 70 ? colorName(boxes[ball].value("color", "unknown")) : "";
            const float w = 20 + (name.empty() ? 0.f : theme::extent(name, 11).x + 5);
            draw->AddRectFilled({a.x + 4, a.y + 4}, {a.x + 4 + w, a.y + 22}, theme::rgb(0x0d0f12, 210), 5);
            colorDot(draw, {a.x + 14, a.y + 13}, swatch, 4);
            if (!name.empty()) theme::write(draw, {a.x + 23, a.y + 6}, theme::text, name, 11);
        }
        const std::string number = std::to_string(i + 1);
        const float font = compact ? 11.f : theme::caption;
        theme::write(draw, {a.x + 1, b.y + (compact ? 3 : 4)}, active ? theme::selected : theme::text, number, font, theme::bold);
        float lx = a.x + 7 + theme::extent(number, font, theme::bold).x;
        // The colour camera's crop caption carries the ball's colour too: "1 ● 27×26 · r 13.0".
        if (const ImU32 swatch = frame.member.value("kind", "") == "컬러" ? colorSwatch(boxes[ball].value("color", "unknown")) : 0) { colorDot(draw, {lx + 2, b.y + (compact ? 9.f : 12.f)}, swatch, compact ? 3.5f : 4.5f); lx += 13; }
        std::string label = sizeText(r);
        if (circles_ && ball < int(frame.circles.size()) && frame.circles[ball].r > 0) { char radius[24]; std::snprintf(radius, sizeof radius, " · r %.1f", frame.circles[ball].r); label += radius; }
        if (!compact) theme::write(draw, {lx, b.y + 4}, theme::text2, label, theme::caption, theme::mono);
        if (cell > 70) {
            const char* tag = sourceTag(boxes[ball]);
            const float w = theme::extent(tag, 11).x + 10;
            draw->AddRect({b.x - w, b.y + 3}, {b.x, b.y + 19}, theme::line, 4);
            theme::write(draw, {b.x - w + 5, b.y + 4}, theme::text3, tag, 11);
        }
        if (clicked && !removing) later_ = [this, slot, ball] { select(slot, ball); };
    }
}
void Application::overviewTile(int slot, ImVec2 pos, ImVec2 size) {
    auto draw = ImGui::GetWindowDrawList();
    const auto& frame = frames_[slot];
    draw->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, theme::bg1, 8);
    tileHeader(slot, pos, size.x, false, "공 " + std::to_string(frame.annotation["boxes"].size()) + "개");
    const float image = std::floor(std::min(size.x * .75f, size.y - 34 - 90));
    const ImVec2 a{pos.x + 1, pos.y + 34}, b{pos.x + size.x - 1, pos.y + 34 + image};
    viewImage(slot, a, b, false);
    cropGrid(slot, {pos.x, b.y}, {size.x, pos.y + size.y - b.y}, false);
    draw->AddRect(pos, {pos.x + size.x, pos.y + size.y}, theme::line, 8);
}
void Application::smallTile(int slot, ImVec2 pos, ImVec2 size) {
    auto draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, theme::bg1, 8);
    tileHeader(slot, pos, size.x, true, "");
    const ImVec2 a{pos.x + 1, pos.y + 34}, b{pos.x + size.x - 1, pos.y + 34 + std::floor(size.x * .75f)};
    viewImage(slot, a, b, true);
    cropGrid(slot, {pos.x, b.y}, {size.x, pos.y + size.y - b.y}, true);
    draw->AddRect(pos, {pos.x + size.x, pos.y + size.y}, theme::line, 8);
}
void Application::focusTile(int slot, ImVec2 pos, ImVec2 size) {
    auto draw = ImGui::GetWindowDrawList();
    auto& frame = frames_[slot];
    draw->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, theme::bg1, 8);
    char meta[128]; std::snprintf(meta, sizeof meta, "프레임 %d · 공 %d개 · 수신 시각 %+.2fms", frame.member.value("index", 0), int(frame.annotation["boxes"].size()), frame.member.value("delta_ms", 0.));
    tileHeader(slot, pos, size.x, false, meta);
    canvas(frame, {pos.x + 1, pos.y + 34}, {size.x - 2, size.y - 35});
    draw->AddRect(pos, {pos.x + size.x, pos.y + size.y}, theme::accent, 8);
}

// ---- focused canvas -------------------------------------------------------------------------

void Application::canvas(FrameView& frame, ImVec2 pos, ImVec2 size) {
    if (frame.image.empty() || size.x < 60 || size.y < 60) return;
    if (frame.repaint) repaint(frame);
    if (frame.scale <= 0) fit(frame, size, true);
    const ImVec2 end{pos.x + size.x, pos.y + size.y};
    ImGui::SetCursorScreenPos(pos); ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
    canvasInput(frame, pos, size, ImGui::IsItemHovered());
    if (frame.repaint) repaint(frame);

    auto draw = ImGui::GetWindowDrawList();
    const float s = frame.scale;
    const ImVec2 a{pos.x + frame.offsetX, pos.y + frame.offsetY};
    auto at = [&](double x, double y) { return ImVec2{a.x + float(x) * s, a.y + float(y) * s}; };
    draw->AddRectFilled(pos, end, IM_COL32(0, 0, 0, 255));
    draw->PushClipRect(pos, end, true);
    draw->AddImage(ImTextureRef(ImTextureID(frame.texture)), a, at(frame.image.cols, frame.image.rows));
    if (masks_ && outlines_) draw->AddImage(ImTextureRef(ImTextureID(frame.overlay)), a, at(frame.image.cols, frame.image.rows));
    if (s >= 5) {   // original pixel grid, so a one-pixel box correction is visible
        const int x0 = std::max(0, int(std::floor((pos.x - a.x) / s))), x1 = std::min(frame.image.cols, int(std::ceil((end.x - a.x) / s)));
        const int y0 = std::max(0, int(std::floor((pos.y - a.y) / s))), y1 = std::min(frame.image.rows, int(std::ceil((end.y - a.y) / s)));
        const ImU32 grid = IM_COL32(255, 255, 255, 22);
        for (int x = x0; x <= x1; ++x) draw->AddLine(at(x, y0), at(x, y1), grid);
        for (int y = y0; y <= y1; ++y) draw->AddLine(at(x0, y), at(x1, y), grid);
    }
    const bool proposing = !proposal_.is_null();
    paintMarks(frame, at, {.fill = false, .boxes = boxes_, .circles = circles_, .outlines = outlines_, .selected = proposing ? -1 : selected_, .skip = proposing ? replacement_ : -1});
    ballBadges(draw, frame.annotation["boxes"], sorted(frame), at, frame.member.value("kind", "") == "컬러", true, proposing ? replacement_ : -1);
    if (proposing) {
        for (const auto& contour : proposalContours_) {
            if (contour.size() < 3) continue;
            std::vector<ImVec2> points; points.reserve(contour.size());
            for (const auto& p : contour) points.push_back(at(p.x + .5, p.y + .5));
            draw->AddPolyline(points.data(), int(points.size()), theme::proposal, ImDrawFlags_Closed, 2);
        }
        if (!proposal_.contains("segmentation") && brushMask_.empty()) { const auto r = detectionBox(proposal_); draw->AddRect(at(r.x, r.y), at(r.x + r.width, r.y + r.height), theme::proposal, 0, 0, 2); }
    }
    for (const auto& point : points_) {
        const ImVec2 c = at(point["x"].get<double>(), point["y"].get<double>());
        const bool include = point["label"] == 1;
        draw->AddCircleFilled(c, 8, include ? theme::okFill : theme::rgb(0xe5484d)); draw->AddCircle(c, 8, IM_COL32_WHITE, 0, 2);
        draw->AddLine({c.x - 4, c.y}, {c.x + 4, c.y}, IM_COL32_WHITE, 2);
        if (include) draw->AddLine({c.x, c.y - 4}, {c.x, c.y + 4}, IM_COL32_WHITE, 2);
    }
    for (size_t i = 0; i < polygon_.size(); ++i) {
        const ImVec2 v = at(polygon_[i].x, polygon_[i].y);
        if (i) draw->AddLine(at(polygon_[i - 1].x, polygon_[i - 1].y), v, theme::proposal, 2);
        draw->AddCircleFilled(v, 3.5f, theme::proposal);
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (dragging_ && (tool_ == Tool::Box || tool_ == Tool::Ai) && std::hypot(mouse.x - dragScreen_.x, mouse.y - dragScreen_.y) > 4)
        draw->AddRect(at(dragStart_.x, dragStart_.y), mouse, theme::proposal, 0, 0, 2);
    const auto& boxes = frame.annotation["boxes"];
    if (!proposing && selected_ >= 0 && selected_ < int(boxes.size())) {
        const auto r = detectionBox(boxes[selected_]);
        const ImVec2 p = at(r.x, r.y), q = at(r.x + r.width, r.y + r.height);
        for (const ImVec2 handle : {ImVec2{(p.x + q.x) / 2, p.y}, ImVec2{q.x, (p.y + q.y) / 2}, ImVec2{(p.x + q.x) / 2, q.y}, ImVec2{p.x, (p.y + q.y) / 2}}) {
            draw->AddRectFilled({handle.x - 4.5f, handle.y - 4.5f}, {handle.x + 4.5f, handle.y + 4.5f}, IM_COL32_WHITE, 2);
            draw->AddRect({handle.x - 4.5f, handle.y - 4.5f}, {handle.x + 4.5f, handle.y + 4.5f}, theme::bg0, 2, 0, 1.5f);
        }
        const std::string tag = std::to_string(int(std::lround(r.width))) + " × " + std::to_string(int(std::lround(r.height))) + " px";
        const float w = theme::extent(tag, theme::caption, theme::mono).x + 14;
        const ImVec2 corner{(p.x + q.x) / 2 - w / 2, p.y - 30};
        draw->AddRectFilled(corner, {corner.x + w, corner.y + 20}, theme::rgb(0x0d0f12, 220), 4);
        theme::write(draw, {corner.x + 7, corner.y + 3}, IM_COL32_WHITE, tag, theme::caption, theme::mono);
    }
    draw->PopClipRect();
    canvasOverlays(frame, pos, size);
}
// Left button adds, right button (or Shift+left) removes. Wheel zooms, a middle drag pans (Space confirms the scene).
void Application::canvasInput(FrameView& frame, ImVec2 pos, ImVec2 size, bool hovered) {
    auto& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const cv::Point2d point{(mouse.x - pos.x - frame.offsetX) / frame.scale, (mouse.y - pos.y - frame.offsetY) / frame.scale};
    const bool inside = point.x >= 0 && point.y >= 0 && point.x < frame.image.cols && point.y < frame.image.rows;
    const bool left = ImGui::IsMouseClicked(ImGuiMouseButton_Left), right = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const bool active = ImGui::IsItemActive();
    if (hovered && io.MouseWheel != 0) {
        const float updated = std::clamp(frame.scale * std::exp(io.MouseWheel * .16f), .1f, 24.f);
        frame.offsetX = mouse.x - pos.x - float(point.x) * updated; frame.offsetY = mouse.y - pos.y - float(point.y) * updated; frame.scale = updated;
    }
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0) || (panning_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)))) { frame.offsetX += io.MouseDelta.x; frame.offsetY += io.MouseDelta.y; }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) panning_ = false;

    if (hovered && !busy() && (left || right)) {
        const bool remove = right || io.KeyShift;
        const int ball = inside ? hit(frame, point) : -1;
        if (left && tool_ == Tool::Select && ball < 0) panning_ = true;
        else if (!inside) {}
        else if (tool_ == Tool::Select) { if (left && !pending()) { selected_ = ball; frame.scale = 0; frame.repaint = true; } }
        else if (tool_ == Tool::Ai) {
            if (!proposal_.is_null() && proposal_.value("source", "") != "efficientsam") message_ = "현재 윤곽을 먼저 반영하거나 취소하세요.";
            else if (remove) segment(point, true);
            else { dragStart_ = point; dragScreen_ = mouse; dragging_ = true; }
        }
        else if (tool_ == Tool::Polygon && proposal_.is_null()) {
            if (remove) { if (!polygon_.empty()) polygon_.pop_back(); }
            else polygon_.push_back(point);
        }
        else if (tool_ == Tool::Box && left && !pending()) { dragStart_ = point; dragScreen_ = mouse; dragging_ = true; replacement_ = -1; }
        else if (tool_ == Tool::Brush) {
            auto& boxes = frame.annotation["boxes"];
            if (selected_ < 0 || selected_ >= int(boxes.size())) message_ = "브러시는 공을 먼저 선택한 뒤 씁니다. 새 공은 AI 분할이나 폴리곤으로 만드세요.";
            else if (!boxes[selected_].contains("segmentation")) message_ = "마스크가 있는 공을 선택하세요.";
            else if (proposal_.is_null() || !brushMask_.empty()) {
                if (proposal_.is_null()) { proposal_ = boxes[selected_]; brushMask_ = decodeMask(boxes[selected_]["segmentation"], frame.image.cols, frame.image.rows); replacement_ = selected_; }
                dragging_ = true; lastBrush_ = point;
                cv::circle(brushMask_, cv::Point(int(point.x), int(point.y)), std::max(1, int(brushSize_ / 2)), cv::Scalar(remove ? 0 : 1), -1);
                frame.repaint = true;
            }
        }
    }
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (dragging_ && tool_ == Tool::Brush && !brushMask_.empty() && inside && down) {
        const bool erase = ImGui::IsMouseDown(ImGuiMouseButton_Right) || io.KeyShift;
        cv::line(brushMask_, cv::Point(int(lastBrush_.x), int(lastBrush_.y)), cv::Point(int(point.x), int(point.y)), cv::Scalar(erase ? 0 : 1), std::max(1, int(brushSize_)), cv::LINE_8);
        lastBrush_ = point; frame.repaint = true;
    }
    if (dragging_ && !down) {
        dragging_ = false;
        const cv::Point2d clamped{std::clamp(point.x, 0., double(frame.image.cols)), std::clamp(point.y, 0., double(frame.image.rows))};
        const cv::Rect2d dragged{std::min(clamped.x, dragStart_.x), std::min(clamped.y, dragStart_.y), std::abs(clamped.x - dragStart_.x), std::abs(clamped.y - dragStart_.y)};
        const bool moved = std::hypot(mouse.x - dragScreen_.x, mouse.y - dragScreen_.y) > 4;
        if (tool_ == Tool::Ai) { if (moved) segmentBox(dragged); else segment(dragStart_, false); }
        else if (tool_ == Tool::Box) { if (dragged.width >= 1 && dragged.height >= 1) proposal_ = {{"bbox", {dragged.x, dragged.y, dragged.width, dragged.height}}, {"occluded", false}, {"source", "manual"}}; }
        else if (!brushMask_.empty()) { try { proposal_ = maskAnnotation(brushMask_, proposal_, "manual_brush"); } catch (const std::exception& error) { error_ = error.what(); } }
        frame.repaint = true;
    }
    (void)size;
}
// Tool rail, tool hint, zoom readout, minimap and the pending-proposal bar float over the canvas.
void Application::canvasOverlays(FrameView& frame, ImVec2 pos, ImVec2 size) {
    auto draw = ImGui::GetWindowDrawList();
    const ImVec2 end{pos.x + size.x, pos.y + size.y};
    const ImU32 glass = theme::rgb(0x15181c, 236);
    struct RailItem { Tool tool; theme::Icon icon; const char* key; };
    static const RailItem tools[] = {{Tool::Select, theme::Icon::Cursor, "V"}, {Tool::Ai, theme::Icon::Spark, "S"}, {Tool::Brush, theme::Icon::Brush, "B"}, {Tool::Polygon, theme::Icon::Polygon, "P"}, {Tool::Box, theme::Icon::Box, "R"}};
    const float railX = pos.x + 10, railY = pos.y + 10, railHeight = 4 + 5 * 38 + 9 + 38 + 9 + 2 * 38 + 2;
    draw->AddRectFilled({railX, railY}, {railX + 44, railY + railHeight}, glass, 8); draw->AddRect({railX, railY}, {railX + 44, railY + railHeight}, theme::line2, 8);
    float y = railY + 4;
    auto button = [&](const char* id, theme::Icon icon, const char* key, bool on) {
        ImGui::SetCursorScreenPos({railX + 4, y});
        const bool clicked = ImGui::InvisibleButton(id, {36, 36}), hovered = ImGui::IsItemHovered();
        if (on) draw->AddRectFilled({railX + 4, y}, {railX + 40, y + 36}, theme::accent, 6); else if (hovered) draw->AddRectFilled({railX + 4, y}, {railX + 40, y + 36}, theme::bg4, 6);
        theme::icon(draw, icon, {railX + 21, y + 17}, 16, on ? theme::ink : theme::text2);
        if (key[0]) theme::write(draw, {railX + 31, y + 24}, on ? theme::ink : theme::text3, key, 9, theme::mono);
        y += 38;
        return clicked;
    };
    auto separator = [&] { draw->AddLine({railX + 10, y + 3}, {railX + 34, y + 3}, theme::line2); y += 9; };
    static const char* const toolNames[] = {"선택 (V)", "AI 분할 (S)", "브러시 (B)", "폴리곤 (P)", "박스 (R)"};
    for (int i = 0; i < 5; ++i) {
        if (button(tools[i].key, tools[i].icon, tools[i].key, tool_ == tools[i].tool) && guard()) tool_ = tools[i].tool;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", toolNames[i]);
    }
    separator();
    if (button("##undo", theme::Icon::Undo, "", false)) undo();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("되돌리기 (Ctrl+Z)");
    separator();
    if (button("##fit", theme::Icon::Fit, "F", false)) { selected_ = -1; frame.scale = 0; frame.repaint = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("화면 맞춤 (F)");
    if (button("##zoom", theme::Icon::Target, "Z", false)) frame.scale = 0;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("선택한 공으로 (Z)");
    if (tool_ == Tool::Brush) {   // the only tool with a setting
        char brush[32]; std::snprintf(brush, sizeof brush, "굵기 %.0fpx", brushSize_);
        const ImVec2 p{pos.x + 64, pos.y + 10};const float w = theme::extent(brush, theme::caption).x + 20;
        draw->AddRectFilled(p, {p.x + w, p.y + 28}, glass, 6); draw->AddRect(p, {p.x + w, p.y + 28}, theme::line2, 6);
        theme::write(draw, {p.x + 10, p.y + 6}, theme::text2, brush, theme::caption);
    }
    char percent[32], ratio[64];
    std::snprintf(percent, sizeof percent, "%.0f%%", frame.scale * 100); std::snprintf(ratio, sizeof ratio, "원본 1픽셀 = 화면 %.1f픽셀", frame.scale);
    const float zoomWidth = 10 + theme::extent(percent, theme::caption, theme::mono).x + 8 + theme::extent(ratio, theme::caption).x + 10;
    const ImVec2 z{end.x - 10 - zoomWidth, pos.y + 10};
    draw->AddRectFilled(z, {z.x + zoomWidth, z.y + 28}, glass, 6); draw->AddRect(z, {z.x + zoomWidth, z.y + 28}, theme::line2, 6);
    theme::write(draw, {z.x + 10, z.y + 6}, theme::text, percent, theme::caption, theme::mono);
    theme::write(draw, {z.x + 18 + theme::extent(percent, theme::caption, theme::mono).x, z.y + 6}, theme::text2, ratio, theme::caption);

    if (size.x > 520 && size.y > 420) {   // minimap with the visible area
        const ImVec2 m0{pos.x + 10, end.y - 10 - 141}, m1{m0.x + 188, m0.y + 141};
        draw->AddRectFilled({m0.x - 3, m0.y - 3}, {m1.x + 3, m1.y + 3}, theme::rgb(0x0d0f12, 160), 8);
        draw->AddImage(ImTextureRef(ImTextureID(frame.texture)), m0, m1);
        draw->AddRect(m0, m1, theme::line2, 0);
        const float vx0 = std::clamp(-frame.offsetX / frame.scale / frame.image.cols, 0.f, 1.f), vy0 = std::clamp(-frame.offsetY / frame.scale / frame.image.rows, 0.f, 1.f);
        const float vx1 = std::clamp((size.x - frame.offsetX) / frame.scale / frame.image.cols, 0.f, 1.f), vy1 = std::clamp((size.y - frame.offsetY) / frame.scale / frame.image.rows, 0.f, 1.f);
        if (vx1 - vx0 < .98f || vy1 - vy0 < .98f) {
            const ImVec2 v0{m0.x + vx0 * 188, m0.y + vy0 * 141}, v1{std::max(m0.x + vx1 * 188, v0.x + 5), std::max(m0.y + vy1 * 141, v0.y + 5)};
            draw->AddRectFilled(v0, v1, theme::rgb(0xffd84a, 40)); draw->AddRect(v0, v1, theme::selected, 0, 0, 1.5f);
        }
    }

    // The pending proposal enlarged at the bottom right, to judge the outline before applying (2026-10-08 user request).
    if (!proposal_.is_null() && size.x > 520 && size.y > 420) {
        cv::Rect2d r;
        if (proposal_.contains("bbox")) r = detectionBox(proposal_);
        else if (!brushMask_.empty()) { const auto b = cv::boundingRect(brushMask_); r = {double(b.x), double(b.y), double(b.width), double(b.height)}; }
        if (r.width >= 1 && r.height >= 1) {
            const float side = 220;
            const ImVec2 p0{end.x - 10 - side, end.y - 10 - 22 - side}, p1{p0.x + side, p0.y + side};
            draw->AddRectFilled({p0.x - 3, p0.y - 3}, {p1.x + 3, p1.y + 22 + 3}, theme::rgb(0x0d0f12, 200), 8);
            const auto region = cropRegion(r, frame.image.cols, frame.image.rows);
            paintImage(frame, p0, p1, region, {.fill = false, .boxes = boxes_, .circles = circles_, .outlines = outlines_, .selected = -1, .skip = replacement_});
            const double scale = side / region.width;
            draw->PushClipRect(p0, p1, true);
            for (const auto& contour : proposalContours_) {
                if (contour.size() < 3) continue;
                std::vector<ImVec2> points; points.reserve(contour.size());
                for (const auto& p : contour) points.push_back({p0.x + float((p.x + .5 - region.x) * scale), p0.y + float((p.y + .5 - region.y) * scale)});
                draw->AddPolyline(points.data(), int(points.size()), theme::proposal, ImDrawFlags_Closed, 2);
            }
            if (proposal_.contains("circle") && circles_) { const auto& c = proposal_["circle"]; draw->AddCircle({p0.x + float((c[0].get<double>() - region.x) * scale), p0.y + float((c[1].get<double>() - region.y) * scale)}, float(c[2].get<double>() * scale), theme::circle, 0, 1.5f); }
            draw->PopClipRect();
            draw->AddRect(p0, p1, theme::proposal, 0, 0, 1.5f);
            std::string caption = "제안 · " + sizeText(r);
            if (proposal_.contains("circle")) { char radius[24]; std::snprintf(radius, sizeof radius, " · r %.1f", proposal_["circle"][2].get<double>()); caption += radius; }
            if (proposal_.contains("color")) { const auto name = colorName(proposal_.value("color", "unknown")); if (*name) caption += std::string(" · ") + name; }
            theme::write(draw, {p0.x, p1.y + 5}, theme::proposal, caption, theme::caption, theme::bold);
        }
    }
    if ((!proposal_.is_null() || polygon_.size() >= 3) && !dragging_) {   // pending proposal: apply or cancel
        ImVec2 anchor{pos.x + size.x / 2, end.y - 70};
        if (!proposal_.is_null()) { const auto r = detectionBox(proposal_); anchor = {pos.x + frame.offsetX + float(r.x + r.width / 2) * frame.scale, pos.y + frame.offsetY + float(r.y + r.height) * frame.scale + 22}; }
        std::string label = "폴리곤 작성 중";
        if (!proposal_.is_null()) {   // say what Space will do: change that ball or add one
            const auto order = sorted(frame);
            const auto found = std::find(order.begin(), order.end(), replacement_);
            label = found == order.end() ? "새 공 추가" : "공 " + std::to_string(found - order.begin() + 1) + " 윤곽 바꾸기";
        }
        const float labelWidth = theme::extent(label, theme::caption, theme::bold).x, barWidth = 12 + labelWidth + 12 + 104 + 6 + 88 + 6;
        const ImVec2 p{std::clamp(anchor.x - barWidth / 2, pos.x + 64, std::max(pos.x + 64, end.x - barWidth - 10)), std::clamp(anchor.y, pos.y + 56, end.y - 44)};
        draw->AddRectFilled(p, {p.x + barWidth, p.y + 34}, theme::rgb(0x15181c, 245), 8); draw->AddRect(p, {p.x + barWidth, p.y + 34}, theme::proposal, 8);
        theme::write(draw, {p.x + 12, p.y + 9}, theme::proposal, label, theme::caption, theme::bold);
        ImGui::PushFont(nullptr, theme::px(theme::caption)); ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6, 4});
        ImGui::SetCursorScreenPos({p.x + 24 + labelWidth, p.y + 5});
        if (theme::primaryButton(proposal_.is_null() ? "닫기  Space" : "반영  Space", {104, 24})) apply();
        ImGui::SetCursorScreenPos({p.x + 24 + labelWidth + 110, p.y + 5});
        if (ImGui::Button("취소  Esc", {88, 24})) cancel();
        ImGui::PopStyleVar(); ImGui::PopFont();
    }
}

// ---- side panel -----------------------------------------------------------------------------

void Application::sidePanel() {
    auto draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(avail);
    draw->AddLine({origin.x + .5f, origin.y}, {origin.x + .5f, origin.y + avail.y}, theme::line);
    const float actions = 112, pad = 14, width = avail.x - 2 * pad;

    ImGui::SetCursorScreenPos({origin.x + 1, origin.y});
    ImGui::BeginChild("SideScroll", {avail.x - 1, avail.y - actions});
    auto list = ImGui::GetWindowDrawList();
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const float x = base.x + pad;
    float y = 12;
    int total = 0; for (const auto& frame : frames_) total += int(frame.annotation["boxes"].size());
    theme::write(list, {x, base.y + y}, theme::text2, "이 장면의 공", theme::caption, theme::bold);
    const std::string count = std::to_string(total) + "개";
    theme::write(list, {x + width - 28 - theme::extent(count, theme::caption).x, base.y + y}, theme::text3, count, theme::caption);
    ImGui::SetCursorScreenPos({x + width - 20, base.y + y - 6});
    if (ImGui::InvisibleButton("##fold", {22, 26})) sideOpen_ = false;
    if (ImGui::IsItemHovered()) { list->AddRectFilled({x + width - 20, base.y + y - 6}, {x + width + 2, base.y + y + 20}, theme::bg3, 4); ImGui::SetTooltip("패널 접기"); }
    theme::icon(list, theme::Icon::Right, {x + width - 9, base.y + y + 7}, 11, theme::text2);
    y += 26;
    for (int slot = 0; slot < int(frames_.size()); ++slot) {
        const auto& frame = frames_[slot];
        theme::dot(list, {x + 5, base.y + y + 10}, memberState(frame.member), 4);
        theme::write(list, {x + 16, base.y + y + 2}, theme::text3, cameraLabel(frame.member), theme::caption);
        y += 24;
        const auto order = sorted(frame);
        const auto& boxes = frame.annotation["boxes"];
        for (int rank = 0; rank < int(order.size()); ++rank) {
            const int ball = order[rank];
            const bool active = editor_ && slot == camera_ && ball == selected_;
            const ImVec2 p{x - 6, base.y + y}, q{x + width + 6, base.y + y + 30};
            ImGui::SetCursorScreenPos(p); ImGui::PushID(slot * 100 + ball);
            ImGui::SetNextItemAllowOverlap();   // the selected row carries a delete button on top
            const bool clicked = ImGui::InvisibleButton("##ball", {q.x - p.x, 30}), hovered = ImGui::IsItemHovered();
            bool removing = false;
            if (active) {
                ImGui::SetCursorScreenPos({q.x - 30, p.y + 4});
                if (ImGui::InvisibleButton("##delete", {24, 22})) pendingRemove_ = {slot, ball};
                removing = ImGui::IsItemHovered();
                if (removing) ImGui::SetTooltip("이 공 삭제 (Delete)");
                theme::icon(list, theme::Icon::Trash, {q.x - 18, p.y + 15}, 12, removing ? theme::warn : theme::text2);
            }
            ImGui::PopID();
            if (active) { list->AddRectFilled(p, q, theme::rgb(0xffd84a, 26), 6); list->AddRect(p, q, theme::rgb(0xffd84a, 115), 6); }
            else if (hovered) list->AddRectFilled(p, q, theme::bg2, 6);
            const ImU32 color = active ? theme::selected : theme::mask;
            const std::string number = std::to_string(rank + 1);
            list->AddCircle({p.x + 17, p.y + 15}, 8.5f, color, 0, 1.5f);
            theme::write(list, {p.x + 17 - theme::extent(number, 11, theme::bold).x / 2, p.y + 8}, color, number, 11, theme::bold);
            const std::string name = "공 " + number;
            theme::write(list, {p.x + 34, p.y + 7}, theme::text, name, theme::sub);
            float tx = p.x + 42 + theme::extent(name, theme::sub).x;
            if (const ImU32 swatch = colorSwatch(boxes[ball].value("color", "unknown"))) { list->AddCircleFilled({tx + 5, p.y + 15}, 5, swatch); list->AddCircle({tx + 5, p.y + 15}, 5, theme::line2); tx += 16; }
            const std::string dimensions = sizeText(detectionBox(boxes[ball]));
            theme::write(list, {tx, p.y + 8}, theme::text3, dimensions, theme::caption, theme::mono); tx += theme::extent(dimensions, theme::caption, theme::mono).x + 8;
            if (boxes[ball].contains("detection_bbox")) { theme::write(list, {tx, p.y + 9}, theme::accent, "보정", 11); tx += theme::extent("보정", 11).x + 6; }
            if (boxes[ball].value("occluded", false)) theme::write(list, {tx, p.y + 9}, theme::hold, "가림", 11);
            const char* tag = sourceTag(boxes[ball]);
            const float tagWidth = theme::extent(tag, 11).x + 10, tagRight = q.x - 8 - (active ? 26 : 0);
            list->AddRect({tagRight - tagWidth, p.y + 7}, {tagRight, p.y + 23}, theme::line, 4);
            theme::write(list, {tagRight + 5 - tagWidth, p.y + 8}, theme::text3, tag, 11);
            if (clicked && !removing) later_ = [this, slot, ball] { select(slot, ball); };
            y += 30;
        }
        y += 4;
    }
    y += 6; list->AddLine({base.x, base.y + y}, {base.x + avail.x, base.y + y}, theme::line); y += 12;

    const bool chosen = editor_ && selected_ >= 0 && selected_ < int(current().annotation["boxes"].size());
    if (chosen) {
        auto& frame = current();
        auto& box = frame.annotation["boxes"][selected_];
        const auto order = sorted(frame);
        const int rank = int(std::find(order.begin(), order.end(), selected_) - order.begin()) + 1;
        const std::string title = cameraLabel(frame.member) + " · 공 " + std::to_string(rank);
        theme::write(list, {x, base.y + y}, theme::text, title, theme::sub, theme::bold);
        theme::write(list, {x + width - theme::extent(sourceName(box), theme::caption).x, base.y + y + 1}, theme::text3, sourceName(box), theme::caption);
        y += 28;
        if (circles_ && selected_ < int(frame.circles.size()) && frame.circles[selected_].r > 0) {
            const auto& c = frame.circles[selected_]; char circle[80]; std::snprintf(circle, sizeof circle, "원  중심 %.1f, %.1f   반지름 %.1f", c.x, c.y, c.r);
            theme::write(list, {x, base.y + y + 2}, theme::text2, circle, theme::caption, theme::mono);
            y += 26;
        }
        theme::write(list, {x, base.y + y + 4}, theme::text2, "일부 가림", theme::caption);
        theme::keycapAt(list, {x + width - 60, base.y + y + 3}, "O");
        bool occluded = box.value("occluded", false);
        ImGui::SetCursorScreenPos({x + width - 30, base.y + y + 4});
        if (theme::toggle("##occluded", &occluded) && !pending() && !busy()) { checkpoint(); box["occluded"] = occluded; changed(); }
        y += 30;
        if (frame.member.value("kind", "") == "컬러") {
            static const char* colors[] = {"미지정", "빨강", "파랑", "흰색", "검정"}; static const char* values[] = {"unknown", "red", "blue", "white", "black"};
            int color = 0; for (int i = 0; i < 5; ++i) if (box.value("color", "unknown") == values[i]) color = i;
            theme::write(list, {x, base.y + y + 5}, theme::text2, "색상 (선택 사항)", theme::caption);
            ImGui::PushFont(nullptr, theme::px(theme::caption)); ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8, 4});
            ImGui::SetCursorScreenPos({x + width - 110, base.y + y}); ImGui::SetNextItemWidth(110);
            if (ImGui::Combo("##color", &color, colors, 5) && !pending() && !busy()) { checkpoint(); box["color"] = values[color]; changed(); }
            ImGui::PopStyleVar(); ImGui::PopFont();
            y += 32;
        }
        y += 6;
        theme::write(list, {x, base.y + y}, theme::text2, "박스 보정", theme::caption, theme::bold);
        theme::write(list, {x + width - theme::extent("원본 1픽셀 단위", theme::caption).x, base.y + y}, theme::text3, "원본 1픽셀 단위", theme::caption);
        y += 24;
        ImGui::SetCursorScreenPos({x, base.y + y});
        nudgePad(frame, box);
        y += 246 + 8;
    } else {
        theme::write(list, {x, base.y + y}, theme::text2, "장면 요약", theme::caption, theme::bold);
        y += 26;
        bool same = true; int first = -1;
        for (const auto& frame : frames_) {
            const auto state = memberState(frame.member);
            const int balls = int(frame.annotation["boxes"].size());
            if (state != MemberState::Duplicate) { if (first < 0) first = balls; else same &= balls == first; }
            theme::dot(list, {x + 5, base.y + y + 10}, state);
            theme::write(list, {x + 18, base.y + y + 1}, theme::text, cameraLabel(frame.member), theme::sub);
            const std::string value = "공 " + std::to_string(balls) + "개 · " + theme::stateLabel(state);
            theme::write(list, {x + width - theme::extent(value, theme::caption).x, base.y + y + 2}, theme::text2, value, theme::caption);
            y += 30;
        }
        if (!same) { theme::warning(list, {x + 6, base.y + y + 14}, 11, theme::warn); theme::write(list, {x + 17, base.y + y + 6}, theme::warn, "카메라마다 공 개수가 다릅니다", theme::caption); y += 26; }
        y += 6;
    }
    ImGui::SetCursorPos({0, y}); ImGui::Dummy({1, 1});
    ImGui::EndChild();

    const float top = origin.y + avail.y - actions;
    draw->AddLine({origin.x, top + .5f}, {origin.x + avail.x, top + .5f}, theme::line);
    ImGui::PushFont(theme::bold, theme::px(theme::base));
    ImGui::SetCursorScreenPos({origin.x + pad, top + 12});
    if (theme::primaryButton("세 장 확인 완료   Space", {width, 44})) later_ = [this] { review(Scope::Scene, false); };
    ImGui::PopFont();
    ImGui::PushFont(nullptr, theme::px(theme::caption));
    ImGui::SetCursorScreenPos({origin.x + pad, top + 66});
    if (ImGui::Button("보류   Q", {(width - 8) / 2, 32})) later_ = [this] { review(editor_ ? Scope::Camera : Scope::Scene, true); };
    ImGui::SameLine(0, 8);
    if (ImGui::Button("다음 미검수   N", {(width - 8) / 2, 32})) later_ = [this] { if (guard()) nextUnreviewed(); };
    ImGui::PopFont();
}
// Edge steppers around a picture of the box. Buttons and arrow keys share Application::nudge().
void Application::nudgePad(FrameView& frame, Json& box) {
    auto draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x - 14;
    draw->AddRectFilled(origin, {origin.x + width, origin.y + 236}, theme::bg2, 8); draw->AddRect(origin, {origin.x + width, origin.y + 236}, theme::line, 8);
    const auto corrected = detectionBox(box);
    const auto& tight = box["bbox"];
    const double tx = tight[0].get<double>(), ty = tight[1].get<double>(), tw = tight[2].get<double>(), th = tight[3].get<double>();
    const int offsets[4] = {int(std::lround(ty - corrected.y)), int(std::lround(corrected.y + corrected.height - ty - th)), int(std::lround(tx - corrected.x)), int(std::lround(corrected.x + corrected.width - tx - tw))};
    const float centerWidth = std::clamp(width - 2 * 82 - 36, 64.f, 104.f), centerHeight = 78;
    const ImVec2 center{origin.x + width / 2, origin.y + 12 + 30 + 6 + centerHeight / 2};
    // Each stepper is [arrow] value [arrow], 82 x 26, centered on `at`. An arrow moves that edge in the direction it
    // points, so the outer arrow of every stepper grows the box and the inner one shrinks it.
    static const char* const edgeNames[4] = {"위", "아래", "왼쪽", "오른쪽"};
    auto stepper = [&](int edge, ImVec2 at) {
        const ImVec2 p{at.x - 41, at.y - 13};
        const bool vertical = edge < 2, firstGrows = edge == 0 || edge == 2;   // top and left steppers point outward on their first arrow
        draw->AddRectFilled(p, {p.x + 82, p.y + 26}, theme::bg1, 6); draw->AddRect(p, {p.x + 82, p.y + 26}, theme::line2, 6);
        draw->AddLine({p.x + 26, p.y}, {p.x + 26, p.y + 26}, theme::line2); draw->AddLine({p.x + 56, p.y}, {p.x + 56, p.y + 26}, theme::line2);
        ImGui::PushID(edge);
        for (int side = 0; side < 2; ++side) {
            const bool grow = (side == 0) == firstGrows;
            const float x = side == 0 ? p.x : p.x + 56;
            ImGui::SetCursorScreenPos({x, p.y});
            if (ImGui::InvisibleButton(side == 0 ? "##first" : "##second", {26, 26})) later_ = [this, edge, grow] { nudge(edge, grow); };
            if (ImGui::IsItemHovered()) {
                draw->AddRectFilled({x, p.y}, {x + 26, p.y + 26}, theme::bg4, 6, side == 0 ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersRight);
                ImGui::SetTooltip("%s 경계를 %s으로 1픽셀", edgeNames[edge], grow ? "바깥" : "안");
            }
            const theme::Icon arrow = vertical ? (side == 0 ? theme::Icon::Up : theme::Icon::Down) : (side == 0 ? theme::Icon::Left : theme::Icon::Right);
            theme::icon(draw, arrow, {x + 13, p.y + 13}, 11, theme::text2);
        }
        ImGui::PopID();
        char value[16]; std::snprintf(value, sizeof value, "%+d", offsets[edge]);
        const std::string shown = offsets[edge] ? value : "0";
        theme::write(draw, {p.x + 41 - theme::extent(shown, theme::caption, theme::mono).x / 2, p.y + 5}, offsets[edge] ? theme::selected : theme::text, shown, theme::caption, theme::mono);
    };
    stepper(0, {center.x, origin.y + 12 + 13});
    stepper(1, {center.x, center.y + centerHeight / 2 + 6 + 13});
    stepper(2, {center.x - centerWidth / 2 - 8 - 41, center.y});
    stepper(3, {center.x + centerWidth / 2 + 8 + 41, center.y});
    const ImVec2 a{center.x - centerWidth / 2, center.y - centerHeight / 2}, b{center.x + centerWidth / 2, center.y + centerHeight / 2};
    draw->AddRectFilled(a, b, theme::rgb(0xffffff, 8), 2); draw->AddRect(a, b, IM_COL32_WHITE, 2, 0, 1.5f);
    draw->AddEllipse(center, {centerWidth / 2 - 14, centerHeight / 2 - 9}, theme::rgb(0x2ee6d0, 150), 0, 0, 1);
    const std::string dimensions = std::to_string(int(std::lround(corrected.width))) + " × " + std::to_string(int(std::lround(corrected.height)));
    theme::write(draw, {center.x - theme::extent(dimensions, theme::base, theme::mono).x / 2, center.y - 9}, theme::text, dimensions, theme::base, theme::mono);

    const float rowY = origin.y + 12 + 30 + 6 + centerHeight + 6 + 30 + 12;
    const char* names[4] = {"X", "Y", "W", "H"};
    const int values[4] = {int(std::lround(corrected.x)), int(std::lround(corrected.y)), int(std::lround(corrected.width)), int(std::lround(corrected.height))};
    const float cell = (width - 20 - 18) / 4;
    for (int i = 0; i < 4; ++i) {
        const ImVec2 p{origin.x + 10 + i * (cell + 6), rowY};
        draw->AddRectFilled(p, {p.x + cell, p.y + 24}, theme::bg1, 5); draw->AddRect(p, {p.x + cell, p.y + 24}, theme::line, 5);
        theme::write(draw, {p.x + 7, p.y + 5}, theme::text3, names[i], theme::caption, theme::mono);
        const std::string value = std::to_string(values[i]);
        theme::write(draw, {p.x + cell - 7 - theme::extent(value, theme::caption, theme::mono).x, p.y + 5}, theme::text, value, theme::caption, theme::mono);
    }
    const bool any = box.contains("detection_bbox");
    theme::write(draw, {origin.x + 10, rowY + 38}, theme::text3, any ? "마스크 외곽에서 보정됨" : "마스크 외곽과 같음", theme::caption);
    ImGui::PushFont(nullptr, theme::px(theme::caption)); ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8, 4});
    ImGui::SetCursorScreenPos({origin.x + width - 10 - 96, rowY + 33});
    ImGui::PushStyleColor(ImGuiCol_Button, theme::bg3);
    if (ImGui::Button("마스크에 맞춤", {96, 24}) && any && !pending() && !busy()) { checkpoint(); box.erase("detection_bbox"); changed(); }
    ImGui::PopStyleColor(); ImGui::PopStyleVar(); ImGui::PopFont();
    (void)frame;
}
}
