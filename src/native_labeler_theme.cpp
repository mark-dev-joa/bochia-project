#include "native_labeler_theme.hpp"
#include <filesystem>

namespace boccia::labeler::theme {
ImFont* mono = nullptr;
void loadFonts() {
    auto& io = ImGui::GetIO();
    const char* fonts = "C:/Windows/Fonts/";
    auto path = [&](const char* name) { return std::string(fonts) + name; };
    ImFontConfig merge; merge.MergeMode = true;
    if (std::filesystem::exists(path("segoeui.ttf"))) io.Fonts->AddFontFromFileTTF(path("segoeui.ttf").c_str());
    if (std::filesystem::exists(path("malgun.ttf"))) io.Fonts->AddFontFromFileTTF(path("malgun.ttf").c_str(), 0, io.Fonts->Fonts.empty() ? nullptr : &merge);
    const auto code = std::filesystem::exists(path("CascadiaMono.ttf")) ? path("CascadiaMono.ttf") : path("consola.ttf");
    if (std::filesystem::exists(code)) mono = io.Fonts->AddFontFromFileTTF(code.c_str());
    ImGui::GetStyle().FontSizeBase = base;
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
    return state == MemberState::Done ? ok : state == MemberState::Hold ? hold : state == MemberState::Editing ? accent : state == MemberState::Duplicate ? line2 : text3;
}
const char* stateLabel(MemberState state) {
    return state == MemberState::Done ? "확인됨" : state == MemberState::Hold ? "보류" : state == MemberState::Editing ? "수정 중" : state == MemberState::Duplicate ? "반복" : "미검수";
}
void dot(ImDrawList* draw, ImVec2 center, MemberState state, float radius) {
    const ImU32 color = stateColor(state);
    if (state == MemberState::Duplicate) draw->AddLine({center.x - radius, center.y}, {center.x + radius, center.y}, color, 2);
    else if (state == MemberState::Done || state == MemberState::Hold) draw->AddCircleFilled(center, radius, color);
    else { if (state == MemberState::Editing) draw->AddCircleFilled(center, radius, accentSoft); draw->AddCircle(center, radius - .75f, color, 0, 1.5f); }
}
bool chip(const char* id, MemberState state) {
    const char* label = stateLabel(state);
    const bool filled = state == MemberState::Done || state == MemberState::Hold || state == MemberState::Editing;
    ImGui::PushFont(nullptr, caption);
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
    ImGui::PushFont(mono, 11);
    const ImVec2 size{std::max(18.f, ImGui::CalcTextSize(key).x + 10), 18}, pos = ImGui::GetCursorScreenPos();
    auto draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, bg3, 4); draw->AddRect(pos, {pos.x + size.x, pos.y + size.y}, line2, 4);
    draw->AddText({pos.x + (size.x - ImGui::CalcTextSize(key).x) / 2, pos.y + 2}, text2, key);
    ImGui::PopFont(); ImGui::Dummy(size);
}
bool segmented(const char* id, const char* const* labels, int count, int* value, const ImU32* colors, float height) {
    ImGui::PushID(id); ImGui::PushFont(nullptr, caption);
    bool changed = false; auto draw = ImGui::GetWindowDrawList(); const ImVec2 start = ImGui::GetCursorScreenPos(); float x = start.x;
    for (int i = 0; i < count; ++i) {
        const float width = ImGui::CalcTextSize(labels[i]).x + 30;
        ImGui::SetCursorScreenPos({x, start.y}); ImGui::PushID(i);
        if (ImGui::InvisibleButton("##segment", {width, height}) && *value != i) { *value = i; changed = true; }
        const bool on = *value == i, hovered = ImGui::IsItemHovered(); ImGui::PopID();
        if (on) draw->AddRectFilled({x, start.y}, {x + width, start.y + height}, colors ? colors[i] : bg4, 6, i == 0 ? ImDrawFlags_RoundCornersLeft : i == count - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
        else if (hovered) draw->AddRectFilled({x, start.y}, {x + width, start.y + height}, bg3);
        if (i) draw->AddLine({x, start.y}, {x, start.y + height}, line2);
        draw->AddText({x + 15, start.y + (height - ImGui::GetFontSize()) / 2}, on ? (colors ? ink : text) : text2, labels[i]);
        x += width;
    }
    draw->AddRect(start, {x, start.y + height}, line2, 6);
    ImGui::SetCursorScreenPos(start); ImGui::Dummy({x - start.x, height});
    ImGui::PopFont(); ImGui::PopID();
    return changed;
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
