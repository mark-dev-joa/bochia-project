#include "native_labeler_theme.hpp"
#include <filesystem>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace boccia::labeler::theme {
ImFont* mono = nullptr;
ImFont* bold = nullptr;
void loadFonts() {
    auto& io = ImGui::GetIO();
    const char* fonts = "C:/Windows/Fonts/";
    auto path = [&](const char* name) { return std::string(fonts) + name; };
    ImFontConfig merge; merge.MergeMode = true;
    if (std::filesystem::exists(path("segoeui.ttf"))) io.Fonts->AddFontFromFileTTF(path("segoeui.ttf").c_str());
    if (std::filesystem::exists(path("malgun.ttf"))) io.Fonts->AddFontFromFileTTF(path("malgun.ttf").c_str(), 0, io.Fonts->Fonts.empty() ? nullptr : &merge);
    const auto code = std::filesystem::exists(path("CascadiaMono.ttf")) ? path("CascadiaMono.ttf") : path("consola.ttf");
    if (std::filesystem::exists(code)) {
        mono = io.Fonts->AddFontFromFileTTF(code.c_str());
        if (std::filesystem::exists(path("malgun.ttf"))) io.Fonts->AddFontFromFileTTF(path("malgun.ttf").c_str(), 0, &merge);
    }
    if (std::filesystem::exists(path("segoeuib.ttf"))) {
        bold = io.Fonts->AddFontFromFileTTF(path("segoeuib.ttf").c_str());
        if (std::filesystem::exists(path("malgunbd.ttf"))) io.Fonts->AddFontFromFileTTF(path("malgunbd.ttf").c_str(), 0, &merge);
    }
    ImGui::GetStyle().FontSizeBase = px(base);
}
void apply() {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {0, 0}; style.FramePadding = {9, 6}; style.ItemSpacing = {8, 8}; style.FrameRounding = 6; style.ChildRounding = 8;
    style.PopupRounding = 10; style.WindowBorderSize = 0; style.ChildBorderSize = 1; style.ScrollbarSize = 10; style.ScrollbarRounding = 5;
    auto set = [&](ImGuiCol slot, ImU32 color) { style.Colors[slot] = ImGui::ColorConvertU32ToFloat4(color); };
    set(ImGuiCol_WindowBg, bg1); set(ImGuiCol_ChildBg, bg1); set(ImGuiCol_PopupBg, bg1); set(ImGuiCol_Border, line); set(ImGuiCol_Text, text);
    set(ImGuiCol_TextDisabled, text3); set(ImGuiCol_Button, bg2); set(ImGuiCol_ButtonHovered, bg4); set(ImGuiCol_ButtonActive, bg3);
    set(ImGuiCol_FrameBg, bg2); set(ImGuiCol_FrameBgHovered, bg3); set(ImGuiCol_FrameBgActive, bg3); set(ImGuiCol_Header, accentSoft);
    set(ImGuiCol_HeaderHovered, bg2); set(ImGuiCol_HeaderActive, accentSoft); set(ImGuiCol_CheckMark, accent); set(ImGuiCol_SliderGrab, text2);
    set(ImGuiCol_SliderGrabActive, text); set(ImGuiCol_Separator, line); set(ImGuiCol_ScrollbarBg, rgb(0, 0)); set(ImGuiCol_ScrollbarGrab, bg4);
    set(ImGuiCol_TableHeaderBg, bg2); set(ImGuiCol_TableBorderLight, line); set(ImGuiCol_TableBorderStrong, line); set(ImGuiCol_ModalWindowDimBg, rgb(0x050608, 158));
    set(ImGuiCol_PlotHistogram, ok);
}
ImU32 stateColor(MemberState state) {
    return state == MemberState::Done || state == MemberState::Propagated ? ok : state == MemberState::Hold ? hold : state == MemberState::Editing ? accent : state == MemberState::Duplicate ? line2 : text3;
}
const char* stateLabel(MemberState state) {
    return state == MemberState::Done ? "확인됨" : state == MemberState::Propagated ? "전파됨" : state == MemberState::Hold ? "보류" : state == MemberState::Editing ? "수정 중" : state == MemberState::Duplicate ? "반복" : "미검수";
}
void dot(ImDrawList* draw, ImVec2 center, MemberState state, float radius) {
    const ImU32 color = stateColor(state);
    if (state == MemberState::Duplicate) draw->AddLine({center.x - radius, center.y}, {center.x + radius, center.y}, color, 2);
    else if (state == MemberState::Propagated) draw->AddCircle(center, radius - .75f, color, 0, 2.f);   // a green ring: confirmed by propagation, not by a person
    else if (state == MemberState::Done || state == MemberState::Hold) draw->AddCircleFilled(center, radius, color);
    else { if (state == MemberState::Editing) draw->AddCircleFilled(center, radius, accentSoft); draw->AddCircle(center, radius - .75f, color, 0, 1.5f); }
}
void write(ImDrawList* draw, ImVec2 pos, ImU32 color, const std::string& value, float size, ImFont* font) { draw->AddText(font, px(size), {pos.x, pos.y - (px(size) - size) / 2}, color, value.c_str()); }
ImVec2 extent(const std::string& value, float size, ImFont* font) { return (font ? font : ImGui::GetFont())->CalcTextSizeA(px(size), FLT_MAX, 0, value.c_str()); }
float paragraph(ImDrawList* draw, ImVec2 pos, ImU32 color, const std::string& value, float width, float size, ImFont* font) {
    ImFont* face = font ? font : ImGui::GetFont();
    const float step = std::round(size * 1.5f);
    const char* at = value.c_str(); const char* const end = at + value.size();
    float y = pos.y;
    while (at < end) {
        const char* stop = face->CalcWordWrapPosition(px(size), at, end, width);
        if (stop == at) { ++stop; while (stop < end && (*stop & 0xC0) == 0x80) ++stop; }   // a glyph wider than the line still moves on
        draw->AddText(face, px(size), {pos.x, y - (px(size) - size) / 2}, color, at, stop);
        at = stop;
        while (at < end && (*at == ' ' || *at == '\n')) ++at;
        y += step;
    }
    return y - pos.y;
}
float keycapAt(ImDrawList* draw, ImVec2 pos, const char* key) {
    const float width = std::max(18.f, extent(key, 11, mono).x + 10);
    draw->AddRectFilled(pos, {pos.x + width, pos.y + 18}, bg3, 4); draw->AddRect(pos, {pos.x + width, pos.y + 18}, line2, 4);
    write(draw, {pos.x + (width - extent(key, 11, mono).x) / 2, pos.y + 3}, text2, key, 11, mono);
    return width;
}
float chipWidth(MemberState state) { return extent(stateLabel(state), caption).x + (state == MemberState::Done ? 30 : 18); }
void icon(ImDrawList* draw, Icon kind, ImVec2 c, float size, ImU32 color) {
    const float h = size / 2, w = 1.6f;
    auto at = [&](float x, float y) { return ImVec2{c.x + x * size, c.y + y * size}; };
    auto stroke = [&](float x0, float y0, float x1, float y1) { draw->AddLine(at(x0, y0), at(x1, y1), color, w); };
    auto path = [&](std::initializer_list<ImVec2> points, bool closed) { draw->AddPolyline(points.begin(), int(points.size()), color, closed ? ImDrawFlags_Closed : ImDrawFlags_None, w); };
    switch (kind) {
    case Icon::Cursor: path({at(-.38f, -.48f), at(.46f, -.06f), at(.08f, .08f), at(-.08f, .48f)}, true); break;
    case Icon::Spark: {
        ImVec2 points[8];
        for (int i = 0; i < 8; ++i) { const float radius = i % 2 ? h * .36f : h, angle = float(i) * 3.14159265f / 4 - 1.5707963f; points[i] = {c.x + std::cos(angle) * radius, c.y + std::sin(angle) * radius}; }
        draw->AddPolyline(points, 8, color, ImDrawFlags_Closed, w); break;
    }
    case Icon::Brush: draw->AddLine(at(.46f, -.46f), at(-.06f, .1f), color, 2.4f); draw->AddCircleFilled(at(-.24f, .3f), size * .17f, color); break;
    case Icon::Polygon: {
        ImVec2 points[5];
        for (int i = 0; i < 5; ++i) { const float angle = float(i) * 6.2831853f / 5 - 1.5707963f; points[i] = {c.x + std::cos(angle) * h, c.y + std::sin(angle) * h}; }
        draw->AddPolyline(points, 5, color, ImDrawFlags_Closed, w); break;
    }
    case Icon::Box: draw->AddRect(at(-.48f, -.36f), at(.48f, .36f), color, 1, 0, w); break;
    case Icon::Undo: draw->PathArcTo(at(.04f, .1f), size * .34f, -2.6f, 1.3f); draw->PathStroke(color, 0, w); stroke(-.44f, -.4f, -.26f, -.08f); stroke(-.26f, -.08f, .08f, -.2f); break;
    case Icon::Fit: path({at(-.48f, -.14f), at(-.48f, -.48f), at(-.14f, -.48f)}, false); path({at(.14f, -.48f), at(.48f, -.48f), at(.48f, -.14f)}, false);
        path({at(-.48f, .14f), at(-.48f, .48f), at(-.14f, .48f)}, false); path({at(.14f, .48f), at(.48f, .48f), at(.48f, .14f)}, false); break;
    case Icon::Target: draw->AddCircle(c, size * .26f, color, 0, w); stroke(0, -.5f, 0, -.3f); stroke(0, .3f, 0, .5f); stroke(-.5f, 0, -.3f, 0); stroke(.3f, 0, .5f, 0); break;
    case Icon::Trash: stroke(-.42f, -.3f, .42f, -.3f); path({at(-.14f, -.3f), at(-.14f, -.48f), at(.14f, -.48f), at(.14f, -.3f)}, false); path({at(-.3f, -.3f), at(-.24f, .48f), at(.24f, .48f), at(.3f, -.3f)}, false); break;
    case Icon::Plus: stroke(0, -.42f, 0, .42f); stroke(-.42f, 0, .42f, 0); break;
    case Icon::Left: path({at(.16f, -.36f), at(-.2f, 0), at(.16f, .36f)}, false); break;
    case Icon::Right: path({at(-.16f, -.36f), at(.2f, 0), at(-.16f, .36f)}, false); break;
    case Icon::Down: path({at(-.36f, -.16f), at(0, .2f), at(.36f, -.16f)}, false); break;
    case Icon::Up: path({at(-.36f, .16f), at(0, -.2f), at(.36f, .16f)}, false); break;
    case Icon::Clock: draw->AddCircle(c, h, color, 0, w); path({at(0, -.26f), at(0, 0), at(.2f, .12f)}, false); break;
    case Icon::Folder: path({at(-.48f, -.3f), at(-.14f, -.3f), at(0, -.16f), at(.48f, -.16f), at(.48f, .36f), at(-.48f, .36f)}, true); break;
    case Icon::Refresh: draw->PathArcTo(c, size * .38f, -.5f, 4.4f); draw->PathStroke(color, 0, w); path({at(.46f, -.48f), at(.46f, -.14f), at(.12f, -.14f)}, false); break;
    case Icon::Info: draw->AddCircle(c, h, color, 0, w); stroke(0, -.06f, 0, .26f); draw->AddCircleFilled(at(0, -.26f), 1.2f, color); break;
    }
}
bool chip(const char* id, MemberState state) {
    const char* label = stateLabel(state);
    const bool filled = state == MemberState::Done || state == MemberState::Hold || state == MemberState::Editing;
    ImGui::PushFont(nullptr, px(caption));
    const ImVec2 size{ImGui::CalcTextSize(label).x + (state == MemberState::Done ? 30 : 18), 22}, pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    auto draw = ImGui::GetWindowDrawList();
    const ImU32 soft = state == MemberState::Done ? okSoft : state == MemberState::Hold ? holdSoft : accentSoft;
    if (filled) draw->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, soft, 11); else draw->AddRect(pos, {pos.x + size.x, pos.y + size.y}, line2, 11);
    float x = pos.x + 9;
    if (state == MemberState::Done) { check(draw, {x + 4, pos.y + 11}, 10, ok); x += 12; }
    draw->AddText({x, pos.y + 3}, filled ? stateColor(state) : text2, label);
    ImGui::PopFont();
    return clicked;
}
void keycap(const char* key) {
    ImGui::PushFont(mono, px(tiny));
    const ImVec2 size{std::max(18.f, ImGui::CalcTextSize(key).x + 10), 18}, pos = ImGui::GetCursorScreenPos();
    auto draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, bg3, 4); draw->AddRect(pos, {pos.x + size.x, pos.y + size.y}, line2, 4);
    draw->AddText({pos.x + (size.x - ImGui::CalcTextSize(key).x) / 2, pos.y + 2}, text2, key);
    ImGui::PopFont(); ImGui::Dummy(size);
}
bool segmented(const char* id, const char* const* labels, int count, int* value, const ImU32* colors, float height, float total) {
    ImGui::PushID(id); ImGui::PushFont(nullptr, px(caption));
    bool changed = false; auto draw = ImGui::GetWindowDrawList(); const ImVec2 start = ImGui::GetCursorScreenPos(); float x = start.x;
    for (int i = 0; i < count; ++i) {
        const float label = ImGui::CalcTextSize(labels[i]).x, width = total > 0 ? total / count : label + 30;
        ImGui::SetCursorScreenPos({x, start.y}); ImGui::PushID(i);
        if (ImGui::InvisibleButton("##segment", {width, height}) && *value != i) { *value = i; changed = true; }
        const bool on = *value == i, hovered = ImGui::IsItemHovered(); ImGui::PopID();
        if (on) draw->AddRectFilled({x, start.y}, {x + width, start.y + height}, colors ? colors[i] : bg4, 6, i == 0 ? ImDrawFlags_RoundCornersLeft : i == count - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
        else if (hovered) draw->AddRectFilled({x, start.y}, {x + width, start.y + height}, bg3);
        if (i) draw->AddLine({x, start.y}, {x, start.y + height}, line2);
        draw->AddText({x + (width - label) / 2, start.y + (height - ImGui::GetFontSize()) / 2}, on ? (colors ? ink : text) : text2, labels[i]);
        x += width;
    }
    draw->AddRect(start, {x, start.y + height}, line2, 6);
    ImGui::SetCursorScreenPos(start); ImGui::Dummy({x - start.x, height});
    ImGui::PopFont(); ImGui::PopID();
    return changed;
}
bool checkbox(const char* label, bool* value, bool locked) {
    ImGui::PushFont(nullptr, px(caption));
    const char* hidden = std::strstr(label, "##");   // what follows ## is only the ImGui id
    const char* const end = hidden ? hidden : label + std::strlen(label);
    const float labelWidth = end > label ? 7 + ImGui::CalcTextSize(label, end).x : 0;
    const ImVec2 pos = ImGui::GetCursorScreenPos(), size{14 + labelWidth, 18};
    const bool clicked = ImGui::InvisibleButton(label, size);
    if (clicked && !locked) *value = !*value;
    auto draw = ImGui::GetWindowDrawList();
    if (*value) { draw->AddRectFilled({pos.x, pos.y + 2}, {pos.x + 14, pos.y + 16}, locked ? bg4 : accent, 3); check(draw, {pos.x + 7, pos.y + 9}, 9, locked ? text2 : ink); }
    else draw->AddRect({pos.x, pos.y + 2}, {pos.x + 14, pos.y + 16}, line2, 3, 0, 1.5f);
    if (end > label) draw->AddText({pos.x + 21, pos.y + 1}, text2, label, end);
    ImGui::PopFont();
    return clicked;
}
bool toggle(const char* id, bool* value) {
    const ImVec2 pos = ImGui::GetCursorScreenPos(), size{30, 16};
    const bool clicked = ImGui::InvisibleButton(id, size);
    if (clicked) *value = !*value;
    auto draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, *value ? accent : bg4, 8);
    draw->AddCircleFilled({pos.x + (*value ? 22.f : 8.f), pos.y + 8}, 6, *value ? rgb(0xffffff) : text2);
    return clicked;
}
bool primaryButton(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, okFill); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(0x23ad6e)); ImGui::PushStyleColor(ImGuiCol_ButtonActive, okFill);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(0xffffff));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}
void check(ImDrawList* draw, ImVec2 center, float size, ImU32 color) {
    const float s = size / 2;
    const ImVec2 points[] = {{center.x - s, center.y}, {center.x - s * .25f, center.y + s * .7f}, {center.x + s, center.y - s * .7f}};
    draw->AddPolyline(points, 3, color, ImDrawFlags_None, 2);
}
void warning(ImDrawList* draw, ImVec2 center, float size, ImU32 color) {
    const float s = size / 2;
    draw->AddTriangle({center.x, center.y - s}, {center.x + s, center.y + s * .85f}, {center.x - s, center.y + s * .85f}, color, 1.5f);
    draw->AddLine({center.x, center.y - s * .25f}, {center.x, center.y + s * .3f}, color, 1.5f);
    draw->AddCircleFilled({center.x, center.y + s * .58f}, 1, color);
}
}
