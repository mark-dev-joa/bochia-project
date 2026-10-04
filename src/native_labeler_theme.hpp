#pragma once
#include "native_labeler_view.hpp"
#include <imgui.h>

namespace boccia::labeler::theme {
constexpr ImU32 rgb(unsigned hex, unsigned alpha = 255) { return IM_COL32((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, alpha); }
inline constexpr ImU32 bg0 = rgb(0x0d0f12), bg1 = rgb(0x15181c), bg2 = rgb(0x1c2025), bg3 = rgb(0x252a31), bg4 = rgb(0x2f353d),
    line = rgb(0x2a3038), line2 = rgb(0x3b434d), text = rgb(0xe8ebef), text2 = rgb(0xa5aeba), text3 = rgb(0x727c88),
    accent = rgb(0x5b9dff), accentSoft = rgb(0x5b9dff, 38), ok = rgb(0x3dd68c), okSoft = rgb(0x3dd68c, 36), okFill = rgb(0x1f9d63),
    hold = rgb(0xf5b544), holdSoft = rgb(0xf5b544, 36), warn = rgb(0xff8a4c), mask = rgb(0x2ee6d0), selected = rgb(0xffd84a),
    proposal = rgb(0xff5ad9), train = rgb(0x5b9dff), val = rgb(0xb48cff), test = rgb(0xff7eb6), ink = rgb(0x08111f);
inline constexpr float base = 14, sub = 13, caption = 12, title = 16, heading = 21, number = 23;
extern ImFont* mono;
void loadFonts();
void apply();
ImU32 stateColor(MemberState state);
const char* stateLabel(MemberState state);
void dot(ImDrawList* draw, ImVec2 center, MemberState state, float radius = 5);
bool chip(const char* id, MemberState state);
void keycap(const char* key);
bool segmented(const char* id, const char* const* labels, int count, int* value, const ImU32* colors = nullptr, float height = 28);
bool toggle(const char* id, bool* value);
bool primaryButton(const char* label, ImVec2 size);
void check(ImDrawList* draw, ImVec2 center, float size, ImU32 color);
void warning(ImDrawList* draw, ImVec2 center, float size, ImU32 color);
}
