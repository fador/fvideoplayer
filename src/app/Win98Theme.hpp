#pragma once

#include <imgui.h>

// A Windows 98 look for Dear ImGui.
//
// Dear ImGui draws its widgets itself and offers no way to replace that code
// from the outside, so the classic 3D edges are produced by painting over the
// stock widgets: every helper here submits the regular ImGui item first (which
// keeps all the input handling) and then redraws the pixels with the Win98
// palette and bevels. That only works while ImGui does not round corners and
// does not add borders of its own, which applyStyle() takes care of.
namespace player::win98 {

// The Win98 system palette, i.e. GetSysColor() of a default scheme. The two
// captions also have a right-hand end colour: Win98 draws title bars as a
// horizontal gradient from the caption colour to a lighter blue.
constexpr ImU32 kActiveCaption      = IM_COL32(0x00, 0x00, 0x80, 0xFF);
constexpr ImU32 kActiveCaptionEnd   = IM_COL32(0x10, 0x84, 0xD0, 0xFF);
constexpr ImU32 kInactiveCaption    = IM_COL32(0x80, 0x80, 0x80, 0xFF);
constexpr ImU32 kInactiveCaptionEnd = IM_COL32(0xB5, 0xB5, 0xB5, 0xFF);
constexpr ImU32 kMenu               = IM_COL32(0xC0, 0xC0, 0xC0, 0xFF);
constexpr ImU32 kWindow             = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
constexpr ImU32 kWindowText         = IM_COL32(0x00, 0x00, 0x00, 0xFF);
constexpr ImU32 kHighlight          = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
constexpr ImU32 kLight              = IM_COL32(0xDF, 0xDF, 0xDF, 0xFF);
constexpr ImU32 kShadow             = IM_COL32(0x80, 0x80, 0x80, 0xFF);
constexpr ImU32 kDarkShadow         = IM_COL32(0x00, 0x00, 0x00, 0xFF);
constexpr ImU32 kGrayText           = IM_COL32(0x80, 0x80, 0x80, 0xFF);
constexpr ImU32 kCaptionText        = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
constexpr ImU32 kInactiveCaptionText = IM_COL32(0xC0, 0xC0, 0xC0, 0xFF);
constexpr ImU32 kTooltipBg          = IM_COL32(0xFF, 0xFF, 0xE1, 0xFF);

// Metrics in logical pixels, as on a 96 dpi display.
constexpr float kFontSize            = 11.0f;  // "MS Sans Serif" 8pt
constexpr float kBevel               = 2.0f;   // 3D edge of a control
constexpr float kWindowFrame         = 4.0f;   // 3D edge of a window
constexpr float kCaptionButtonWidth  = 16.0f;
constexpr float kCaptionButtonHeight = 14.0f;
constexpr float kScrollbarSize       = 16.0f;

// Loads "MS Sans Serif" as the UI font and returns it. Runs before the first
// frame, and before applyStyle() so the metrics can match the font. The file it
// settled on is reported back, for the log.
ImFont* loadUiFont(const char** used_file);

// Installs the palette and the metrics. Runs before the first frame.
void applyStyle();

// Matches the ImPlot styling to the palette. Runs after ImPlot::CreateContext().
void stylePlots();

// 3D edges and other primitives, all in unscaled pixels. Rectangles are given
// as a minimum and a maximum corner.
void bevel(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max, bool raised, bool filled,
           float thickness = kBevel);
void etchedLine(ImDrawList* draw_list, float x0, float x1, float y);
void sizeGrip(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max);
void checkMark(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max, bool checked);
void plusMinus(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max, bool open);

// Puts a sunken edge around a finished child window, as Win98 does around text
// fields and lists. Draw on the parent window, right after ImGui::EndChild().
void sunkenBorder(const ImVec2& min, const ImVec2& max);

// Widgets. Each one submits the matching ImGui item and repaints it.
bool button(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
bool checkbox(const char* label, bool* value);
bool inputText(const char* label, char* buffer, size_t buffer_size,
               ImGuiInputTextFlags flags = 0);
bool sliderFloat(const char* label, float* value, float min, float max,
                 ImGuiSliderFlags flags = 0);
bool sliderInt(const char* label, int* value, int min, int max, ImGuiSliderFlags flags = 0);
void separator();
bool collapsingHeader(const char* label,
                      ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen);

// Menus. The highlighting is drawn over the item because ImGui only offers one
// colour for the text of a menu item, and Win98 draws it white on navy. The
// radio form marks the selected item with a bullet instead of a check, the way
// Win98 groups mutually exclusive entries.
bool beginMenu(const char* label);
void endMenu();
bool menuItem(const char* label, const char* shortcut = nullptr, bool selected = false,
              bool enabled = true);
bool menuItemRadio(const char* label, const char* shortcut = nullptr, bool selected = false,
                   bool enabled = true);
bool menuItemToggle(const char* label, const char* shortcut, bool* selected);

// Draws the bottom edge of the main menu bar. Call after ImGui::EndMainMenuBar().
void endMainMenuBar();

// Adds the 3D frame, the caption buttons and the size grip of a window. ImGui
// paints the window background and its caption while End() runs, so this has to
// come after ImGui::End(). Pass the same name that was given to Begin(), and
// nullptr as p_open there so that ImGui does not draw a close button of its own.
void windowFrame(const char* window_name, bool* open);

// Menus and tooltips are drawn as ImGui popup windows; this puts a 3D edge on
// all of them. Call once per frame, after every window has been submitted.
void decoratePopups();

}  // namespace player::win98