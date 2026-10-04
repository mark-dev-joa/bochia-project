// Data preparation, split and export pages of the v2 design. Each has a body column and a summary column.
#include "native_labeler_app.hpp"
#include "native_labeler_theme.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <windows.h>
#include <shellapi.h>

namespace boccia::labeler {
namespace {
const char* const splitKeys[] = {"train", "val", "test"};
const char* const splitNames[] = {"학습", "검증", "평가"};
const ImU32 splitColors[] = {theme::train, theme::val, theme::test};

struct Stage { ImVec2 body, side; float bodyWidth, sideWidth; };
// Reserves the visible area and returns where the two columns start. Everything is placed with absolute positions.
Stage stageFrame() {
    const ImVec2 origin = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(avail);
    const float side = avail.x < 1300 ? 300.f : 372.f;
    return {origin, {origin.x + avail.x - side, origin.y}, avail.x - side - 22, side};
}
// Draw-list content does not grow the scroll area, so reserve it down to the lowest thing drawn.
void reserve(const Stage& stage, float bottom) { ImGui::SetCursorScreenPos({stage.body.x, bottom}); ImGui::Dummy({1, 1}); }
// Returns the y below the heading. The subtitle wraps instead of running under the buttons on a narrow window.
float heading(ImDrawList* draw, ImVec2 pos, float width, const char* title, const char* subtitle) {
    theme::write(draw, pos, theme::text, title, theme::heading, theme::bold);
    return pos.y + 31 + theme::paragraph(draw, {pos.x, pos.y + 31}, theme::text2, subtitle, width, theme::sub) + 15;
}
void panel(ImDrawList* draw, ImVec2 a, ImVec2 b) { draw->AddRectFilled(a, b, theme::bg1, 10); draw->AddRect(a, b, theme::line, 10); }
void card(ImDrawList* draw, ImVec2 pos, float width, const std::string& label, const std::string& value, const std::string& note, ImU32 swatch = 0, ImFont* font = nullptr) {
    panel(draw, pos, {pos.x + width, pos.y + 86});
    float x = pos.x + 14;
    if (swatch) { draw->AddRectFilled({x, pos.y + 15}, {x + 10, pos.y + 25}, swatch, 3); x += 16; }
    theme::write(draw, {x, pos.y + 13}, theme::text3, label, theme::caption);
    draw->PushClipRect(pos, {pos.x + width - 8, pos.y + 86}, true);
    theme::write(draw, {pos.x + 14, pos.y + 31}, theme::text, value, theme::number, font ? font : theme::bold);
    theme::write(draw, {pos.x + 14, pos.y + 62}, theme::text2, note, theme::caption);
    draw->PopClipRect();
}
enum class Mark { Ok, Warn, Info };
// One box of the summary column. Its panel is painted behind the content once the height is known.
struct SideBox {
    ImDrawList* draw; ImVec2 pos; float width, y;
    SideBox(ImDrawList* list, ImVec2 at, float w, const char* title): draw(list), pos(at), width(w), y(at.y + 14) {
        draw->ChannelsSplit(2); draw->ChannelsSetCurrent(1);
        theme::write(draw, {pos.x + 16, y}, theme::text2, title, theme::caption, theme::bold); y += 28;
    }
    float left() const { return pos.x + 16; }
    float inner() const { return width - 32; }
    void text(const std::string& value, ImU32 color = theme::text3) { y += theme::paragraph(draw, {left(), y}, color, value, inner()) + 8; }
    void mark(Mark kind, const std::string& value, const std::string& detail = "") {
        const ImVec2 center{left() + 7, y + 9};
        if (kind == Mark::Ok) theme::check(draw, center, 11, theme::ok); else if (kind == Mark::Warn) theme::warning(draw, center, 13, theme::warn); else theme::icon(draw, theme::Icon::Info, center, 13, theme::text2);
        y += theme::paragraph(draw, {left() + 24, y}, theme::text, value, inner() - 24, theme::sub) + 2;
        if (!detail.empty()) y += theme::paragraph(draw, {left() + 24, y}, theme::text3, detail, inner() - 24) + 2;
        y += 8;
    }
    float close() {   // returns the y where the next box starts
        y += 6; draw->ChannelsSetCurrent(0); panel(draw, pos, {pos.x + width, y}); draw->ChannelsMerge(); return y + 12;
    }
};
// Recordings are shown under the folder they were copied in.
std::string folderOf(const Json& recording) {
    const auto path = recording.value("relative_path", "");
    const auto slash = path.find_first_of("/\\");
    return slash == std::string::npos ? path : path.substr(0, slash);
}
int folderCount(const Json& recordings) {
    int count = 0; std::string last;
    for (const auto& recording : recordings) if (const auto name = folderOf(recording); !count || name != last) { ++count; last = name; }
    return count;
}
// Column x positions between left and right. The minimum widths grow together by up to 35% when there is room
// and shrink together when there is not.
std::vector<float> columnStarts(float left, float right, std::initializer_list<float> minimums) {
    float sum = 0; for (const float width : minimums) sum += width;
    const float factor = std::min(1.35f, (right - left) / sum);
    std::vector<float> starts; float x = left;
    for (const float width : minimums) { starts.push_back(x); x += width * factor; }
    return starts;
}
// The panel and header strip of a recording table. Returns the y of the first row.
float tableHead(ImDrawList* draw, float left, float right, float top, const Json& recordings) {
    panel(draw, {left, top}, {right, top + 32 + 38.f * folderCount(recordings) + 45.f * recordings.size()});
    draw->AddRectFilled({left + 1, top + 1}, {right - 1, top + 32}, theme::bg2, 10, ImDrawFlags_RoundCornersTop);
    return top + 32;
}
void folderRow(ImDrawList* draw, float left, float right, float y, const std::string& name, const std::string& note, ImFont* noteFont, const std::string& tail) {
    draw->AddRectFilled({left + 1, y}, {right - 1, y + 38}, theme::bg2);
    draw->AddLine({left + 1, y}, {right - 1, y}, theme::line);
    theme::icon(draw, theme::Icon::Folder, {left + 24, y + 19}, 13, theme::text2);
    theme::write(draw, {left + 44, y + 11}, theme::text, name, theme::sub, theme::bold);
    const float tailWidth = tail.empty() ? 0 : theme::extent(tail, theme::caption).x + 16;
    draw->PushClipRect({left, y}, {right - 16 - tailWidth, y + 38}, true);
    theme::write(draw, {left + 44 + theme::extent(name, theme::sub, theme::bold).x + 12, y + 12}, theme::text3, note, theme::caption, noteFont);
    draw->PopClipRect();
    if (!tail.empty()) theme::write(draw, {right - tailWidth, y + 12}, theme::text2, tail, theme::caption);
}
void badge(ImDrawList* draw, ImVec2 pos, int number) {
    char label[8]; std::snprintf(label, sizeof label, "%02d", number);
    draw->AddRectFilled(pos, {pos.x + 26, pos.y + 22}, theme::bg3, 5);
    theme::write(draw, {pos.x + 13 - theme::extent(label, theme::caption, theme::mono).x / 2, pos.y + 4}, theme::text2, label, theme::caption, theme::mono);
}
// Start time with the length under it.
void recordingCell(ImDrawList* draw, ImVec2 pos, const Json& recording, const Json& item) {
    theme::write(draw, {pos.x, pos.y + 6}, theme::text, item.contains("started") && item["started"].is_string() ? item["started"].get<std::string>() : recording.value("name", ""), theme::sub);
    if (item.contains("seconds") && item["seconds"].is_number()) { char seconds[32]; std::snprintf(seconds, sizeof seconds, "%.1f초", item["seconds"].get<double>()); theme::write(draw, {pos.x, pos.y + 24}, theme::text3, seconds, theme::caption); }
}
void progressBar(ImDrawList* draw, ImVec2 pos, float width, int done, int total, ImU32 color) {
    draw->AddRectFilled(pos, {pos.x + width, pos.y + 4}, theme::bg4, 2);
    if (total > 0 && done > 0) draw->AddRectFilled(pos, {pos.x + std::max(2.f, width * std::min(done, total) / total), pos.y + 4}, color, 2);
}
}

// ---- data preparation -----------------------------------------------------------------------

void Application::preparePage() {
    auto draw = ImGui::GetWindowDrawList();
    const auto stage = stageFrame();
    const auto& stats = catalog_["stats"]; const auto& recordings = catalog_["recordings"]; const auto& summary = catalog_["summary"];
    float y = heading(draw, stage.body, stage.bodyWidth, "데이터 준비", "현장 PC에서 복사해 온 촬영의 자동 초안과 반복 장면 필터 상태를 확인하고 검수를 시작합니다.");
    int images = 0, cameras = 0;
    for (const auto& recording : recordings) { cameras = std::max(cameras, int(recording["cameras"].size())); for (const auto& camera : recording["cameras"]) images += camera.value("count", 0); }
    if (targetScenes_ < 0) {   // counted once per catalog change, not per frame
        targetScenes_ = emptyDrafts_ = 0;
        for (const auto& scene : catalog_["scenes"]) for (const auto& member : scene["members"]) if (member.value("selected", true)) { ++targetScenes_; break; }
        for (const auto& [key, frame] : summary["frames"].items()) emptyDrafts_ += frame.value("count", 0) == 0;
    }
    const int target = catalog_.value("target_images", 0), reviewedImages = catalog_.value("reviewed_target_images", 0);
    const int candidates = summary.value("draft_boxes", 0) + summary.value("boxes", 0), drafted = summary.value("automatic_frames", 0);
    const auto job = autoStatus_.value("job", Json::object());
    const auto status = job.value("status", "idle");
    const bool running = status == "running";
    const int done = job.value("done", 0), total = job.value("total", 0);

    const float left = stage.body.x, right = left + stage.bodyWidth, gap = 12, cardWidth = (stage.bodyWidth - 3 * gap) / 4;
    card(draw, {left, y}, cardWidth, "추가된 촬영", std::to_string(recordings.size()) + "개", "카메라 " + std::to_string(cameras) + "대");
    card(draw, {left + (cardWidth + gap), y}, cardWidth, "원본 이미지", grouped(images) + "장", "카메라별 " + grouped(images / std::max(1, cameras)) + "장");
    card(draw, {left + 2 * (cardWidth + gap), y}, cardWidth, "자동 초안", running ? grouped(done) + " / " + grouped(total) + "장" : grouped(drafted) + "장 완료",
         "공 후보 " + grouped(candidates) + "개 · 후보 없음 " + grouped(emptyDrafts_) + "장");
    card(draw, {left + 3 * (cardWidth + gap), y}, cardWidth, "검수 대상", grouped(target) + "장", "장면 " + grouped(targetScenes_) + "개 · 반복 " + grouped(images - target) + "장 보관");
    y += 86 + 14;

    // recording table
    const float splitAt = right - 16 - 92, flexibleEnd = splitAt - 16;
    const auto c = columnStarts(left + 62, flexibleEnd, {170, 110, 190, 170, 200});
    static const char* const headers[] = {"촬영", "프레임", "자동 초안 (검수 대상 기준)", "반복 장면 필터", "검수"};
    const float top = y;
    y = tableHead(draw, left, right, top, recordings);
    for (int i = 0; i < 5; ++i) theme::write(draw, {c[i], top + 9}, theme::text3, headers[i], theme::caption);
    theme::write(draw, {splitAt, top + 9}, theme::text3, "분할", theme::caption);
    std::string folder; bool started = false;
    for (const auto& recording : recordings) {
        if (const auto name = folderOf(recording); !started || name != folder) {
            folder = name; started = true;
            const auto count = std::count_if(recordings.begin(), recordings.end(), [&](const Json& other) { return folderOf(other) == folder; });
            folderRow(draw, left, right, y, folder, utf8(config_.source / fromUtf8(folder)), theme::mono, "촬영 " + std::to_string(count) + "개");
            y += 38;
        }
        const auto id = recording.at("id").get<std::string>();
        const auto item = stats.value(id, Json::object());
        draw->AddLine({left + 1, y}, {right - 1, y}, theme::line);
        badge(draw, {left + 16, y + 12}, item.value("number", 0));
        recordingCell(draw, {c[0], y}, recording, item);
        int frames = 0, count = 0; for (const auto& camera : recording["cameras"]) { frames = std::max(frames, camera.value("count", 0)); ++count; }
        theme::write(draw, {c[1], y + 14}, theme::text, std::to_string(frames) + " × " + std::to_string(count) + "대", theme::sub, theme::mono);
        const int selected = item.value("images", 0), empties = item.value("empty", 0), reviewed = item.value("reviewed", 0);
        int all = 0; for (const auto& camera : recording["cameras"]) all += camera.value("count", 0);
        theme::write(draw, {c[2], y + 6}, theme::text, "공 후보 " + grouped(item.value("balls", 0)) + "개", theme::sub);
        if (empties) { theme::warning(draw, {c[2] + 6, y + 31}, 11, theme::warn); theme::write(draw, {c[2] + 17, y + 24}, theme::warn, "후보 없는 이미지 " + grouped(empties) + "장", theme::caption); }
        else theme::write(draw, {c[2], y + 24}, theme::text3, "모든 이미지에 후보 있음", theme::caption);
        theme::write(draw, {c[3], y + 6}, theme::text, "검수 대상 " + grouped(selected) + "장", theme::sub);
        theme::write(draw, {c[3], y + 24}, theme::text3, "반복 장면 " + grouped(all - selected) + "장 보관", theme::caption);
        theme::write(draw, {c[4], y + 8}, theme::text, grouped(reviewed) + " / " + grouped(selected), theme::sub, theme::mono);
        progressBar(draw, {c[4], y + 31}, std::min(190.f, flexibleEnd - c[4] - 8), reviewed, selected, theme::ok);
        // The saved assignment, not a plan that is still open on the split page.
        const auto saved = summary["splits"].value(id, "unassigned");
        int split = -1; for (int i = 0; i < 3; ++i) if (saved == splitKeys[i]) split = i;
        const char* label = split < 0 ? "미지정" : splitNames[split];
        const float chip = theme::extent(label, theme::caption).x + 18;
        draw->AddRect({splitAt, y + 12}, {splitAt + chip, y + 33}, theme::line2, 10);
        theme::write(draw, {splitAt + 9, y + 15}, split < 0 ? theme::text2 : splitColors[split], label, theme::caption);
        y += 45;
    }

    // summary column
    float sy = stage.side.y;
    {
        SideBox box(draw, {stage.side.x, sy}, stage.sideWidth, "다음 단계");
        box.mark(running ? Mark::Info : Mark::Ok, running ? "자동 초안을 만드는 중입니다" : "자동 초안과 반복 장면 필터가 준비됐습니다",
                 "검수 대상 " + grouped(target) + "장 · 확인한 이미지 " + grouped(reviewedImages) + "장");
        ImGui::PushFont(theme::bold, theme::px(theme::base));
        ImGui::SetCursorScreenPos({box.left(), box.y});
        if (theme::primaryButton("검수 시작", {box.inner(), 44})) later_ = [this] { openPage(Page::Review); };
        ImGui::PopFont();
        box.y += 52;
        box.text("자동 초안은 초안입니다. 사람이 확인한 이미지만 학습 데이터로 내보냅니다.");
        sy = box.close();
    }
    {
        SideBox box(draw, {stage.side.x, sy}, stage.sideWidth, "자동 초안");
        const std::string state = running ? "만드는 중 · " + grouped(done) + " / " + grouped(total) + "장"
            : status == "error" ? "오류로 멈췄습니다"
            : status == "stopped" || status == "interrupted" ? "중단됨 · 이어하기를 누르면 남은 프레임부터 만듭니다"
            : status == "complete" || drafted >= images ? "완료 · " + grouped(drafted) + "장"
            : "대기 · 초안 " + grouped(drafted) + " / " + grouped(images) + "장";
        box.y += theme::paragraph(draw, {box.left(), box.y}, theme::text, state, box.inner(), theme::sub) + 6;
        if (status == "error") box.text(job.value("error", ""), theme::warn);
        if (running) { progressBar(draw, {box.left(), box.y}, box.inner(), done, total, theme::accent); box.y += 14; }
        ImGui::PushFont(nullptr, theme::px(theme::caption));
        ImGui::SetCursorScreenPos({box.left(), box.y});
        if (ImGui::Button("시작 / 이어하기", {(box.inner() - 8) / 2, 30})) later_ = [this] {
            const auto client = client_;
            launch("자동 초안 시작", [client] { return JobResult{client.post("/api/auto-start", Json::object()), {}}; },
                   [this](JobResult result) { autoStatus_["job"] = result.data; message_ = "자동 초안을 시작했습니다. 초안과 사람의 확인은 별개입니다."; });
        };
        ImGui::SameLine(0, 8);
        if (ImGui::Button("중단", {(box.inner() - 8) / 2, 30})) later_ = [this] {
            const auto client = client_;
            launch("자동 초안 중단", [client] { return JobResult{client.post("/api/auto-stop", Json::object()), {}}; }, [this](JobResult result) { autoStatus_["job"] = result.data; });
        };
        ImGui::PopFont();
        box.y += 38;
        box.text("사람이 고친 이미지는 자동 초안이 덮어쓰지 않습니다. EfficientSAM-Ti를 이 PC에서 실행합니다.");
        sy = box.close();
    }
    if (!recordings.empty()) {
        SideBox box(draw, {stage.side.x, sy}, stage.sideWidth, "카메라");
        for (const auto& camera : recordings[0]["cameras"]) {
            const auto kind = camera.value("kind", "");
            theme::write(draw, {box.left(), box.y}, theme::text2, camera.value("name", ""), theme::caption);
            theme::write(draw, {box.left() + box.inner() - theme::extent(kind, theme::caption).x, box.y}, theme::text, kind, theme::caption);
            box.y += 26;
        }
        sy = box.close();
    }
    {
        SideBox box(draw, {stage.side.x, sy}, stage.sideWidth, "원본 보호");
        box.mark(Mark::Ok, "원본 촬영 파일은 읽기만 합니다", "복사, 수정, 삭제하지 않습니다.");
        box.text("입력 " + utf8(config_.source));
        box.text("라벨 " + utf8(config_.project));
        sy = box.close();
    }
    reserve(stage, std::max(y, sy));
}

// ---- split ----------------------------------------------------------------------------------

void Application::proposeSplit() {
    const auto client = client_; const auto groups = groups_;
    launch("배분안 만들기", [client, groups] { return JobResult{client.post("/api/native/split-preview", {{"groups", groups}}), {}}; },
           [this](JobResult result) { splits_ = result.data.at("splits"); planEdited_ = true; message_ = "배분안입니다. 촬영별 배정을 확인하고 확정하세요."; });
}
// Puts the ticked recordings, and the groups they are in, into one group. It takes the split of the first one ticked.
void Application::mergePicked() {
    if (picked_.size() < 2) { message_ = "같은 배치로 묶을 촬영을 표 왼쪽에서 두 개 이상 고르세요."; return; }
    mergeGroups(groups_, picked_);
    const auto group = groups_.at(picked_.front()).get<std::string>(); const auto split = splits_.value(picked_.front(), "unassigned");
    for (const auto& [recording, value] : groups_.items()) if (value.get<std::string>() == group) splits_[recording] = split;
    picked_.clear(); planEdited_ = true; message_ = "묶은 촬영은 항상 같은 쪽에 들어갑니다.";
}
// A group moves together.
void Application::assignSplit(const std::string& recording, const std::string& split) {
    const auto group = groups_.value(recording, recording);
    for (const auto& [other, value] : groups_.items()) if (value.get<std::string>() == group) splits_[other] = split;
    splits_[recording] = split; planEdited_ = true;
}
void Application::confirmSplit() {
    const auto client = client_; Json payload = {{"groups", groups_}, {"splits", splits_}, {"expected_revision", catalog_["summary"]["revision"]}};
    launch("분할 확정", [client, payload] { return JobResult{client.post("/api/native/split-confirm", payload), {}}; }, [this](JobResult result) {
        updateSummary(result.data.at("summary")); planEdited_ = false; catalog_["split_confirmed"] = true; catalog_["fingerprint"] = result.data.value("fingerprint", "");
        page_ = Page::Export; message_ = "분할을 확정했습니다. 이 데이터 버전으로 모델을 비교하세요."; refreshStage();
    });
}
void Application::splitPage() {
    auto draw = ImGui::GetWindowDrawList();
    const auto stage = stageFrame();
    const auto& stats = catalog_["stats"]; const auto& recordings = catalog_["recordings"];
    const float left = stage.body.x, right = left + stage.bodyWidth;
    static const char* const mergeLabel = "선택한 촬영을 같은 배치로 묶기"; static const char* const proposeLabel = "배분안 다시 제안";
    const float mergeWidth = theme::extent(mergeLabel, theme::sub).x + 28, proposeWidth = theme::extent(proposeLabel, theme::sub).x + 28;
    float y = heading(draw, stage.body, stage.bodyWidth - mergeWidth - proposeWidth - 28, "분할", "확인한 이미지를 학습·검증·평가로 나눕니다. 같은 촬영의 카메라와 연속 프레임은 항상 같은 쪽에 들어갑니다.");
    const int done = catalog_.value("reviewed_target_images", 0), target = catalog_.value("target_images", 0);
    const bool complete = target > 0 && done == target, confirmed = splitConfirmed();

    ImGui::PushFont(nullptr, theme::px(theme::sub));
    ImGui::SetCursorScreenPos({right - proposeWidth - 8 - mergeWidth, stage.body.y + 14});
    if (ImGui::Button(mergeLabel, {mergeWidth, 34})) later_ = [this] { mergePicked(); };
    ImGui::SameLine(0, 8);
    if (ImGui::Button(proposeLabel, {proposeWidth, 34})) later_ = [this] { proposeSplit(); };
    ImGui::PopFont();

    draw->AddRectFilled({left, y}, {right, y + 36}, complete ? theme::okSoft : theme::accentSoft, 10);
    draw->AddRect({left, y}, {right, y + 36}, complete ? theme::rgb(0x3dd68c, 100) : theme::rgb(0x5b9dff, 115), 10);
    if (complete) theme::check(draw, {left + 22, y + 18}, 11, theme::ok); else theme::icon(draw, theme::Icon::Info, {left + 22, y + 18}, 14, theme::accent);
    theme::write(draw, {left + 42, y + 10}, theme::text, complete ? "검수 대상 " + grouped(target) + "장을 모두 확인했습니다"
        : "확인한 이미지 " + grouped(done) + " / " + grouped(target) + "장 · 배분안은 지금 볼 수 있고, 확정은 모두 확인한 뒤에 할 수 있습니다", theme::sub);
    y += 36 + 12;

    // ratio
    auto totals = splitTotals(stats, splits_);
    int all = 0; for (const auto& [id, item] : stats.items()) all += item.value("images", 0);
    panel(draw, {left, y}, {right, y + 112});
    theme::write(draw, {left + 16, y + 12}, theme::text2, "이미지 기준 비율", theme::caption, theme::bold);
    static const char* const goal = "목표 80 / 10 / 10 · 흰 선이 목표 위치";
    theme::write(draw, {right - 16 - theme::extent(goal, theme::caption).x, y + 12}, theme::text3, goal, theme::caption);
    const float barLeft = left + 16, barWidth = stage.bodyWidth - 32, barTop = y + 40;
    draw->AddRectFilled({barLeft, barTop}, {barLeft + barWidth, barTop + 30}, theme::bg3, 6);
    float x = barLeft;
    for (int i = 0; i < 3; ++i) {
        const float share = all ? float(totals[splitKeys[i]].images) / all : 0, width = barWidth * share;
        if (width < 1) continue;
        const bool first = x == barLeft, last = x + width > barLeft + barWidth - 1;
        const ImDrawFlags corners = first && last ? ImDrawFlags_RoundCornersAll : first ? ImDrawFlags_RoundCornersLeft : last ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone;
        draw->AddRectFilled({x, barTop}, {x + width, barTop + 30}, splitColors[i], 6, corners);
        char label[48]; std::snprintf(label, sizeof label, "%s %.1f%%", splitNames[i], share * 100);
        if (share > .04f) theme::write(draw, {x + (width - theme::extent(label, theme::caption, theme::bold).x) / 2, barTop + 7}, theme::ink, label, theme::caption, theme::bold);
        x += width;
    }
    for (const float at : {.8f, .9f}) draw->AddRectFilled({barLeft + barWidth * at - 1, barTop - 5}, {barLeft + barWidth * at + 1, barTop + 35}, IM_COL32_WHITE, 1);
    for (int i = 0; i < 3; ++i) {
        const auto& total = totals[splitKeys[i]]; const float lx = barLeft + i * barWidth / 3;
        draw->AddRectFilled({lx, y + 86}, {lx + 10, y + 96}, splitColors[i], 3);
        theme::write(draw, {lx + 18, y + 83}, theme::text, splitNames[i], theme::sub, theme::bold);
        const float textAt = lx + 26 + theme::extent(splitNames[i], theme::sub, theme::bold).x, room = lx + barWidth / 3 - 8 - textAt;
        const std::string counts = "촬영 " + std::to_string(total.recordings) + "개 · " + grouped(total.images) + "장 · 공 " + grouped(total.balls) + "개";
        const std::string full = counts + " · 공 없는 이미지 " + grouped(total.empty) + "장";
        draw->PushClipRect({lx, y + 80}, {textAt + room, y + 104}, true);
        theme::write(draw, {textAt, y + 84}, theme::text2, theme::extent(full, theme::caption).x <= room ? full : counts, theme::caption);   // a narrow window drops the last figure
        draw->PopClipRect();
    }
    y += 112 + 12;

    // recording table
    const float assign = 174, assignAt = right - 16 - assign;
    const auto c = columnStarts(left + 80, assignAt - 16, {150, 120, 70, 100, 150, 110});
    static const char* const headers[] = {"촬영", "확인한 이미지", "공", "공 없는 이미지", "작은 공 (32픽셀 미만)", "같은 배치 묶음"};
    const float top = y;
    y = tableHead(draw, left, right, top, recordings);
    for (int i = 0; i < 6; ++i) theme::write(draw, {c[i], top + 9}, theme::text3, headers[i], theme::caption);
    theme::write(draw, {right - 16 - theme::extent("배정", theme::caption).x, top + 9}, theme::text3, "배정", theme::caption);
    std::map<std::string, int> members; std::map<std::string, char> letters;
    for (const auto& recording : recordings) { const auto id = recording.at("id").get<std::string>(); ++members[groups_.value(id, id)]; }
    for (const auto& recording : recordings) { const auto id = recording.at("id").get<std::string>(); const auto group = groups_.value(id, id); if (members[group] > 1 && !letters.count(group)) letters[group] = char('A' + letters.size()); }
    std::string folder; bool started = false;
    for (const auto& recording : recordings) {
        if (const auto name = folderOf(recording); !started || name != folder) {
            folder = name; started = true;
            const auto count = std::count_if(recordings.begin(), recordings.end(), [&](const Json& other) { return folderOf(other) == folder; });
            folderRow(draw, left, right, y, folder, "촬영 " + std::to_string(count) + "개", nullptr, "");
            y += 38;
        }
        const auto id = recording.at("id").get<std::string>();
        const auto item = stats.value(id, Json::object());
        const auto group = groups_.value(id, id);
        draw->AddLine({left + 1, y}, {right - 1, y}, theme::line);
        ImGui::PushID(id.c_str());
        bool picked = std::find(picked_.begin(), picked_.end(), id) != picked_.end();
        ImGui::SetCursorScreenPos({left + 16, y + 14});
        if (theme::checkbox("##pick", &picked)) { if (picked) picked_.push_back(id); else picked_.erase(std::remove(picked_.begin(), picked_.end(), id), picked_.end()); }
        badge(draw, {left + 40, y + 12}, item.value("number", 0));
        recordingCell(draw, {c[0], y}, recording, item);
        const int images = item.value("images", 0), balls = item.value("balls", 0), smallBalls = item.value("small", 0), empties = item.value("empty", 0);
        theme::write(draw, {c[1], y + 6}, theme::text, grouped(item.value("reviewed", 0)) + " / " + grouped(images) + "장", theme::sub, theme::mono);
        theme::write(draw, {c[1], y + 24}, theme::text3, "검수 대상", theme::caption);
        theme::write(draw, {c[2], y + 14}, theme::text, grouped(balls), theme::sub, theme::mono);
        if (empties) theme::write(draw, {c[3], y + 14}, theme::text, grouped(empties) + "장", theme::sub, theme::mono);
        else theme::write(draw, {c[3], y + 14}, theme::text3, "없음", theme::sub);
        const std::string count = grouped(smallBalls) + "개";
        theme::write(draw, {c[4], y + 14}, theme::text, count, theme::sub, theme::mono);
        theme::write(draw, {c[4] + theme::extent(count, theme::sub, theme::mono).x + 10, y + 15}, theme::text3, std::to_string(balls ? int(std::lround(100.0 * smallBalls / balls)) : 0) + "%", theme::caption, theme::mono);
        if (letters.count(group)) {
            const std::string chip = std::string("묶음 ") + letters[group];
            const float width = theme::extent(chip, theme::caption).x + 18;
            draw->AddRectFilled({c[5], y + 12}, {c[5] + width, y + 33}, theme::bg3, 10); draw->AddRect({c[5], y + 12}, {c[5] + width, y + 33}, theme::line2, 10);
            theme::write(draw, {c[5] + 9, y + 15}, theme::text, chip, theme::caption);
            ImGui::SetCursorScreenPos({c[5] + width + 4, y + 12});
            if (ImGui::InvisibleButton("##leave", {36, 21})) later_ = [this, id] { leaveGroup(groups_, id); planEdited_ = true; };
            theme::write(draw, {c[5] + width + 10, y + 15}, ImGui::IsItemHovered() ? theme::text : theme::text3, "풀기", theme::caption);
        } else theme::write(draw, {c[5], y + 14}, theme::text3, "단독", theme::sub);
        int value = -1; for (int i = 0; i < 3; ++i) if (splits_.value(id, "unassigned") == splitKeys[i]) value = i;
        ImGui::SetCursorScreenPos({assignAt, y + 9});
        if (theme::segmented("##assign", splitNames, 3, &value, splitColors, 28, assign)) assignSplit(id, splitKeys[value]);
        ImGui::PopID();
        y += 45;
    }

    // summary column
    float sy = stage.side.y;
    {
        SideBox box(draw, {stage.side.x, sy}, stage.sideWidth, "점검");
        for (const auto& check : splitChecks(stats, splits_, groups_)) box.mark(check.ok ? Mark::Ok : Mark::Warn, check.text, check.detail);
        sy = box.close();
    }
    {
        SideBox box(draw, {stage.side.x, sy}, stage.sideWidth, "확정");
        ImGui::PushFont(theme::bold, theme::px(theme::base));
        ImGui::SetCursorScreenPos({box.left(), box.y});
        if (theme::primaryButton(confirmed ? "확정됨 · 내보내기로" : "분할 확정", {box.inner(), 44})) later_ = [this, complete, confirmed, done, target] {
            if (confirmed) { openPage(Page::Export); return; }
            if (!complete) { message_ = "확인하지 않은 이미지가 " + grouped(target - done) + "장 남았습니다. 모두 확인한 뒤에 확정할 수 있습니다."; return; }
            if (guard()) confirmSplit();
        };
        ImGui::PopFont();
        box.y += 52;
        if (!complete) box.text("확인하지 않은 이미지 " + grouped(target - done) + "장이 남아 있어 아직 확정할 수 없습니다.", theme::hold);
        box.text("확정하면 이 배정과 라벨 상태가 데이터 버전으로 기록됩니다. 확정한 뒤 라벨을 고치면 다시 확정해야 내보낼 수 있습니다.");
        box.text("평가에 넣은 촬영은 첫 학습 뒤에는 바꾸지 않습니다. 모든 모델을 같은 평가 세트로 비교하기 위해서입니다.");
        sy = box.close();
    }
    reserve(stage, std::max(y, sy));
}

// ---- export ---------------------------------------------------------------------------------

// The fingerprint of the confirmed split, empty while there is none.
std::string Application::dataVersion() const {
    const auto found = catalog_.find("fingerprint");
    return splitConfirmed() && found != catalog_.end() && found->is_string() ? found->get<std::string>() : "";
}
void Application::startExport() {
    if (exportStatus_.value("status", "") == "running") { message_ = "내보내기가 진행 중입니다."; return; }
    if (!splitConfirmed()) { message_ = planEdited_ ? "바꾼 배정을 먼저 확정하세요." : "분할을 먼저 확정하세요."; openPage(Page::Split); return; }
    if (!guard()) return;
    const auto client = client_; const bool crops = crops_;
    launch("내보내기 시작", [client, crops] { return JobResult{client.post("/api/native/export", {{"crops", crops}}), {}}; },
           [this](JobResult result) { exportStatus_ = result.data; message_ = "확인하고 분할한 데이터를 내보내기 시작했습니다."; });
}
void Application::exportPage() {
    auto draw = ImGui::GetWindowDrawList();
    const auto stage = stageFrame();
    const auto& stats = catalog_["stats"];
    float y = heading(draw, stage.body, stage.bodyWidth, "내보내기", "확정한 데이터 버전을 학습용 폴더로 내보냅니다. 확인하지 않은 이미지는 들어가지 않습니다.");
    const bool confirmed = splitConfirmed();
    const std::string fingerprint = dataVersion();
    auto totals = splitTotals(stats, splits_);
    int all = 0; for (const auto& key : splitKeys) all += totals[key].images;

    const float left = stage.body.x, right = left + stage.bodyWidth, gap = 12, cardWidth = (stage.bodyWidth - 3 * gap) / 4;
    const bool versioned = fingerprint.size() >= 8;
    card(draw, {left, y}, cardWidth, "데이터 버전", versioned ? fingerprint.substr(0, 8) : "확정 전", confirmed ? "이미지 " + grouped(all) + "장 · 분할 확정됨" : "분할을 확정하면 정해집니다", 0, versioned ? theme::mono : nullptr);
    for (int i = 0; i < 3; ++i) card(draw, {left + (i + 1) * (cardWidth + gap), y}, cardWidth, splitNames[i], grouped(totals[splitKeys[i]].images) + "장",
                                     "촬영 " + std::to_string(totals[splitKeys[i]].recordings) + "개 · 공 " + grouped(totals[splitKeys[i]].balls) + "개", splitColors[i]);
    y += 86 + 16;

    theme::write(draw, {left, y}, theme::text2, "형식", theme::caption, theme::bold); y += 24;
    struct Format { const char* title; const char* box; const char* files; const char* target; const char* note; bool optional; };
    static const Format formats[] = {
        {"YOLO 검출 라벨", "보정한 검출 박스", "labels/{train,val,test}/*.txt · data.yaml", "YOLO26", "data.yaml을 그대로 지정", false},
        {"COCO 검출", "보정한 검출 박스", "annotations/instances_detection_{train,val,test}.json", "D-FINE-S · RT-DETRv2-S · Faster R-CNN", "같은 박스, 같은 분할", false},
        {"COCO 마스크", "마스크와 마스크 외곽 박스", "annotations/instances_{train,val,test}.json", "분할 실험용", "보정값과 무관하게 마스크 그대로", false},
        {"공 조각 이미지", "검출 박스로 잘라낸 PNG", "crops/{train,val,test}/*.png", "분류 실험용", "선택 사항", true}};
    panel(draw, {left, y}, {right, y + 58 * 4});
    float titleWidth = 0, targetWidth = stage.bodyWidth > 1100 ? 300.f : 0.f;   // the columns hug their text when the window is narrow
    for (const auto& format : formats) {
        titleWidth = std::max({titleWidth, theme::extent(format.title, theme::sub, theme::bold).x, theme::extent(format.box, theme::caption).x});
        targetWidth = std::max({targetWidth, theme::extent(format.target, theme::sub).x, theme::extent(format.note, theme::caption).x});
    }
    const float filesAt = left + 53 + titleWidth + 20;
    for (const auto& format : formats) {
        if (&format != formats) draw->AddLine({left + 1, y}, {right - 1, y}, theme::line);
        ImGui::SetCursorScreenPos({left + 17, y + 20});
        bool on = format.optional ? crops_ : true;
        ImGui::PushID(format.title);
        if (theme::checkbox("##format", &on, !format.optional)) { if (format.optional) crops_ = on; else message_ = std::string(format.title) + "은 항상 만듭니다."; }
        ImGui::PopID();
        theme::write(draw, {left + 53, y + 11}, theme::text, format.title, theme::sub, theme::bold);
        theme::write(draw, {left + 53, y + 31}, theme::text3, format.box, theme::caption);
        draw->PushClipRect({filesAt, y}, {right - 16 - targetWidth - 8, y + 58}, true);
        theme::write(draw, {filesAt, y + 21}, theme::text2, format.files, theme::caption, theme::mono);
        draw->PopClipRect();
        draw->PushClipRect({right - 16 - targetWidth, y}, {right - 12, y + 58}, true);
        theme::write(draw, {right - 16 - targetWidth, y + 11}, theme::text, format.target, theme::sub);
        theme::write(draw, {right - 16 - targetWidth, y + 31}, theme::text3, format.note, theme::caption);
        draw->PopClipRect();
        y += 58;
    }
    y += 16;
    theme::write(draw, {left, y}, theme::text2, "항상 포함", theme::caption, theme::bold); y += 24;
    panel(draw, {left, y}, {right, y + 68});
    static const char* const included[][2] = {{"원본 이미지", "images/{train,val,test}/*.png · 화면 밝기와 오버레이는 들어가지 않음"}, {"기록", "manifest.json · 원본 위치, 촬영별 배정, 묶음, 데이터 버전"}};
    for (int i = 0; i < 2; ++i) {
        theme::write(draw, {left + 16, y + 12 + 26.f * i}, theme::text2, included[i][0], theme::caption);
        theme::write(draw, {right - 16 - theme::extent(included[i][1], theme::caption, theme::mono).x, y + 12 + 26.f * i}, theme::text, included[i][1], theme::caption, theme::mono);
    }
    y += 68 + 16;
    theme::write(draw, {left, y}, theme::text2, "저장 위치", theme::caption, theme::bold); y += 24;
    draw->AddRectFilled({left, y}, {right, y + 40}, theme::bg0, 8); draw->AddRect({left, y}, {right, y + 40}, theme::line2, 8);
    theme::icon(draw, theme::Icon::Folder, {left + 22, y + 20}, 13, theme::text2);
    draw->PushClipRect({left, y}, {right - 12, y + 40}, true);
    theme::write(draw, {left + 40, y + 12}, theme::text, utf8(config_.exports), theme::sub, theme::mono);
    draw->PopClipRect();
    y += 40 + 16;

    const bool running = exportStatus_.value("status", "idle") == "running";
    ImGui::PushFont(theme::bold, theme::px(theme::base));
    ImGui::SetCursorScreenPos({left, y});
    if (theme::primaryButton(running ? "내보내는 중" : "내보내기", {130, 44})) later_ = [this] { startExport(); };
    ImGui::PopFont();
    const int total = exportStatus_.value("total", 0), done = exportStatus_.value("done", 0);
    draw->PushClipRect({left + 146, y}, {right, y + 44}, true);
    if (running) {
        theme::write(draw, {left + 146, y + 6}, theme::text2, grouped(done) + " / " + grouped(total) + "장", theme::caption, theme::mono);
        progressBar(draw, {left + 146, y + 28}, 300, done, total, theme::ok);
    } else if (exportStatus_.contains("error")) theme::write(draw, {left + 146, y + 14}, theme::warn, exportStatus_.value("error", ""), theme::caption);
    else if (exportStatus_.contains("result")) theme::write(draw, {left + 146, y + 14}, theme::ok, "완료 · " + exportStatus_["result"].value("path", ""), theme::caption);
    // Measured on the 2026-10-02 recordings: a PNG is about 1.8MB in color and 2.2MB in mono.
    else if (confirmed) theme::write(draw, {left + 146, y + 14}, theme::text3, "이미지 " + grouped(all) + "장 · 예상 크기 약 " + std::to_string(std::max(1, int(std::lround(all * 2.1 / 1024)))) + "GB · 끝나면 이 화면에 결과가 남습니다", theme::caption);
    else theme::write(draw, {left + 146, y + 14}, theme::text3, "분할을 확정한 뒤에 내보낼 수 있습니다.", theme::caption);
    draw->PopClipRect();
    y += 44;

    // summary column
    std::string latest = exportStatus_.contains("result") ? exportStatus_["result"].value("path", "") : "";
    if (latest.empty() && !exports_.empty()) latest = exports_[0].value("path", "");
    float sy = stage.side.y;
    {
        SideBox box(draw, {stage.side.x, sy}, stage.sideWidth, "학습 시작");
        theme::write(draw, {box.left(), box.y}, theme::text, "YOLO26", theme::sub, theme::bold);
        const std::string command = "yolo detect train data=\"" + latest + "\\data.yaml\" model=yolo26s.pt imgsz=1440 batch=-1";
        if (!latest.empty()) {
            ImGui::PushFont(nullptr, theme::px(theme::caption)); ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6, 3});
            ImGui::SetCursorScreenPos({box.left() + box.inner() - 52, box.y - 3});
            if (ImGui::Button("복사", {52, 24})) { ImGui::SetClipboardText(command.c_str()); message_ = "YOLO26 학습 명령을 복사했습니다."; }
            ImGui::PopStyleVar(); ImGui::PopFont();
        }
        box.y += 24;
        if (latest.empty()) box.text("먼저 내보내세요. 내보낸 폴더의 data.yaml로 학습 명령을 만듭니다.");
        else {
            box.text("가장 최근에 내보낸 폴더의 data.yaml을 지정합니다");
            const float height = theme::paragraph(draw, {box.left() + 12, box.y + 10}, theme::text, command, box.inner() - 24, theme::caption, theme::mono);
            draw->AddRect({box.left(), box.y}, {box.left() + box.inner(), box.y + height + 20}, theme::line2, 8);
            box.y += height + 32;
        }
        theme::write(draw, {box.left(), box.y}, theme::text, "D-FINE-S · RT-DETRv2-S · Faster R-CNN", theme::sub, theme::bold); box.y += 24;
        box.text("annotations/instances_detection_*.json과 images/를 지정합니다");
        sy = box.close();
    }
    {
        SideBox box(draw, {stage.side.x, sy}, stage.sideWidth, "내보내기 기록");
        if (exports_.empty()) box.text("아직 내보낸 적이 없습니다.");
        int shown = 0;
        for (const auto& entry : exports_) {
            if (shown++ >= 4) break;
            // Folder names start with the local time: 20261004-230533-xxxxxxxx
            const std::string name = entry.value("name", ""), version = entry.contains("fingerprint") && entry["fingerprint"].is_string() ? entry["fingerprint"].get<std::string>().substr(0, 8) : "";
            const bool stamped = name.size() >= 15 && name[8] == '-' && std::all_of(name.begin(), name.begin() + 8, [](unsigned char c) { return std::isdigit(c) != 0; });
            const std::string when = stamped ? name.substr(4, 2) + "-" + name.substr(6, 2) + " " + name.substr(9, 2) + ":" + name.substr(11, 2) : name;
            const auto counts = entry.value("counts", Json::object());
            auto count = [&](const char* key) { return counts.value(key, Json::object()).value("images", 0); };
            theme::check(draw, {box.left() + 7, box.y + 9}, 11, theme::ok);
            theme::write(draw, {box.left() + 24, box.y}, theme::text, version.empty() ? when : version + " · " + when, theme::caption, theme::mono);
            theme::write(draw, {box.left() + 24, box.y + 20}, theme::text3, grouped(count("train") + count("val") + count("test")) + "장 · 학습 " + grouped(count("train")) + " / 검증 " + grouped(count("val")) + " / 평가 " + grouped(count("test")), theme::caption);
            ImGui::PushID(name.c_str()); ImGui::PushFont(nullptr, theme::px(theme::tiny)); ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6, 3});
            ImGui::SetCursorScreenPos({box.left() + box.inner() - 66, box.y - 2});
            if (ImGui::Button("폴더 열기", {66, 22})) { const auto path = fromUtf8(entry.value("path", "")); ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
            ImGui::PopStyleVar(); ImGui::PopFont(); ImGui::PopID();
            box.y += 48;
        }
        sy = box.close();
    }
    {
        SideBox box(draw, {stage.side.x, sy}, stage.sideWidth, "비교 원칙");
        box.mark(Mark::Ok, "모든 모델은 같은 데이터 버전으로 학습하고 같은 평가 세트로 비교합니다", "라벨이나 배정이 하나라도 바뀌면 데이터 버전이 달라집니다.");
        sy = box.close();
    }
    reserve(stage, std::max(y, sy));
}
}
