#pragma once
#include "native_labeler_view.hpp"
#include <imgui.h>
#include <string>

namespace boccia::labeler::theme {
constexpr ImU32 rgb(unsigned hex, unsigned alpha = 255) { return IM_COL32((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, alpha); }
inline constexpr ImU32 bg0 = rgb(0x0d0f12), bg1 = rgb(0x15181c), bg2 = rgb(0x1c2025), bg3 = rgb(0x252a31), bg4 = rgb(0x2f353d),
    line = rgb(0x2a3038), line2 = rgb(0x3b434d), text = rgb(0xe8ebef), text2 = rgb(0xa5aeba), text3 = rgb(0x727c88),
    accent = rgb(0x5b9dff), accentSoft = rgb(0x5b9dff, 38), ok = rgb(0x3dd68c), okSoft = rgb(0x3dd68c, 36), okFill = rgb(0x1f9d63),
    hold = rgb(0xf5b544), holdSoft = rgb(0xf5b544, 36), warn = rgb(0xff8a4c), mask = rgb(0x2ee6d0), selected = rgb(0xffd84a),
    proposal = rgb(0xff5ad9), train = rgb(0x5b9dff), val = rgb(0xb48cff), test = rgb(0xff7eb6), ink = rgb(0x08111f);
// Sizes are written like CSS pixels in the design. ImGui measures a font by its full line height, so text is drawn
// 1.3 times larger and re-centered; px() gives the size to pass to ImGui::PushFont.
inline constexpr float scale = 1.3f, base = 13.5f, sub = 13, caption = 12, tiny = 11, title = 15.5f, heading = 20, number = 22;
constexpr float px(float size) { return size * scale; }
extern ImFont* mono;
extern ImFont* bold;
enum class Icon { Cursor, Spark, Brush, Polygon, Box, Undo, Fit, Target, Trash, Plus, Left, Right, Up, Down, Clock, Folder, Refresh, Info };
void loadFonts();
void apply();
ImU32 stateColor(MemberState state);
const char* stateLabel(MemberState state);
void dot(ImDrawList* draw, ImVec2 center, MemberState state, float radius = 5);
bool chip(const char* id, MemberState state);
void keycap(const char* key);
// Draw-list variants for absolutely positioned tiles. keycapAt returns the width it used.
void write(ImDrawList* draw, ImVec2 pos, ImU32 color, const std::string& value, float size = base, ImFont* font = nullptr);
ImVec2 extent(const std::string& value, float size = base, ImFont* font = nullptr);
// Wraps at the given width with a line height of 1.5 and returns the height it used.
float paragraph(ImDrawList* draw, ImVec2 pos, ImU32 color, const std::string& value, float width, float size = caption, ImFont* font = nullptr);
float keycapAt(ImDrawList* draw, ImVec2 pos, const char* key);
float chipWidth(MemberState state);
void icon(ImDrawList* draw, Icon kind, ImVec2 center, float size, ImU32 color);
// width > 0 gives every segment the same share of that width; otherwise segments fit their labels.
bool segmented(const char* id, const char* const* labels, int count, int* value, const ImU32* colors = nullptr, float height = 28, float width = 0);
// A locked box keeps its value and looks muted; the click is still reported so the caller can say why.
bool checkbox(const char* label, bool* value, bool locked = false);
bool toggle(const char* id, bool* value);
bool primaryButton(const char* label, ImVec2 size);
void check(ImDrawList* draw, ImVec2 center, float size, ImU32 color);
void warning(ImDrawList* draw, ImVec2 center, float size, ImU32 color);
}
