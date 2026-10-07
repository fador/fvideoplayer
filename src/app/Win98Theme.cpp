#include "app/Win98Theme.hpp"

#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <unordered_map>

namespace player::win98 {
namespace {

// The Win98 UI font is "MS Sans Serif" 8pt. Windows still ships the outline
// version of it in micross.ttf, which is as close as it gets; the rest are
// fallbacks that keep the metrics roughly right.
constexpr const char* kUiFontFiles[] = {
    "C:/Windows/Fonts/micross.ttf",
    "C:/Windows/Fonts/sserife.ttf",
    "C:/Windows/Fonts/mss.ttf",
    "C:/Windows/Fonts/sansserif.ttf",
    "C:/Windows/Fonts/tahoma.ttf",
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/verdana.ttf",
    "/usr/share/fonts/truetype/msttcorefonts/Andale_Sans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
};

float snapf(float value) {
    return IM_ROUND(value);
}

// Bevels are drawn as solid runs of whole pixels, so every coordinate has to
// land on the pixel grid or the edges turn into grey smears.
ImVec2 snapv(const ImVec2& value) {
    return ImVec2(IM_ROUND(value.x), IM_ROUND(value.y));
}

ImRect snapr(const ImRect& box) {
    return ImRect(snapv(box.Min), snapv(box.Max));
}

void fillRect(ImDrawList* draw_list, float x0, float y0, float x1, float y1, ImU32 color) {
    draw_list->AddRectFilled(ImVec2(snapf(x0), snapf(y0)), ImVec2(snapf(x1), snapf(y1)), color);
}

ImVec4 rgba(ImU32 color) {
    return ImVec4(static_cast<float>((color >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
                  static_cast<float>((color >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                  static_cast<float>((color >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f,
                  static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f);
}

bool itemDisabled() {
    return (ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0;
}

// Everything from a "###" or "##" marker on is not shown by ImGui either.
const char* renderedTextEnd(const char* text) {
    for (const char* c = text; *c != '\0'; ++c) {
        if (c[0] == '#' && c[1] == '#') {
            return c;
        }
    }
    return text + std::strlen(text);
}

// The row a Selectable occupies, including the gap to its neighbours and the
// padding of the menu, so that a highlighted row reaches the edges of the menu
// and the rows tile without seams.
ImRect menuRow(const ImRect& item) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float upper = IM_TRUNC(style.ItemSpacing.y * 0.5f);
    return ImRect(item.Min.x - style.WindowPadding.x, item.Min.y - upper,
                  item.Max.x + style.WindowPadding.x, item.Max.y + (style.ItemSpacing.y - upper));
}

// The mark column of a menu row: a check for a toggle, a bullet for a radio
// group. The colour follows the row text so the mark stays visible on the
// navy highlight.
void drawMark(ImDrawList* draw_list, const ImRect& box, ImU32 color, bool radio) {
    const ImRect bb = snapr(box);
    if (radio) {
        const float radius = std::max(1.0f, snapf(bb.GetHeight() * 0.20f));
        draw_list->AddCircleFilled(bb.GetCenter(), radius, color);
        return;
    }
    const ImVec2 elbow(bb.Min.x + bb.GetWidth() * 0.40f, bb.Min.y + bb.GetHeight() * 0.72f);
    const float thickness = std::max(1.0f, snapf(bb.GetWidth() / 6.0f));
    draw_list->AddLine(ImVec2(bb.Min.x + bb.GetWidth() * 0.16f, bb.Min.y + bb.GetHeight() * 0.54f),
                       elbow, color, thickness);
    draw_list->AddLine(elbow,
                       ImVec2(bb.Min.x + bb.GetWidth() * 0.84f, bb.Min.y + bb.GetHeight() * 0.24f),
                       color, thickness);
}

void menuItemChrome(ImDrawList* draw_list, const ImRect& row, const ImVec2& text_pos,
                    const char* label, const char* shortcut, bool selected, bool enabled,
                    bool radio) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImRect bb = snapr(row);
    const ImU32 text_color = enabled ? kCaptionText : kGrayText;
    const float mark_width = ImGui::GetFontSize() * 1.20f;

    // Win98 highlights the whole row in the active caption colour and switches
    // the text to white, so ImGui's own text has to be painted over.
    fillRect(draw_list, bb.Min.x, bb.Min.y, bb.Max.x, bb.Max.y, kActiveCaption);

    draw_list->AddText(text_pos, text_color, label, renderedTextEnd(label));
    if (shortcut != nullptr && shortcut[0] != '\0') {
        const ImVec2 size = ImGui::CalcTextSize(shortcut);
        const float x = bb.Max.x - style.FramePadding.x - size.x - (selected ? mark_width : 0.0f);
        draw_list->AddText(ImVec2(snapf(x), text_pos.y), text_color, shortcut);
    }
    if (selected) {
        const float side = bb.GetHeight() * 0.55f;
        const float cx = bb.Max.x - mark_width * 0.5f;
        const float cy = bb.GetCenter().y;
        drawMark(draw_list,
                 ImRect(ImVec2(cx - side * 0.5f, cy - side * 0.5f),
                        ImVec2(cx + side * 0.5f, cy + side * 0.5f)),
                 text_color, radio);
    }
}

bool menuItemImpl(const char* label, const char* shortcut, bool selected, bool enabled,
                  bool radio) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 pos = window->DC.CursorPos;
    const float text_y = pos.y + window->DC.CurrLineTextBaseOffset;
    const float label_x = pos.x + window->DC.MenuColumns.OffsetLabel;

    // The mark is drawn here rather than by ImGui so a radio group can use a
    // bullet; ImGui only knows the check mark form.
    const bool pressed = ImGui::MenuItem(label, shortcut, nullptr, enabled);
    if (!ImGui::IsItemVisible()) {
        return pressed;
    }
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImRect item(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (enabled && ImGui::IsItemHovered()) {
        menuItemChrome(draw_list, menuRow(item), ImVec2(label_x, text_y), label, shortcut,
                       selected, enabled, radio);
    } else if (selected) {
        const float mark_width = ImGui::GetFontSize() * 1.20f;
        const float side = item.GetHeight() * 0.55f;
        const float cx = item.Max.x - mark_width * 0.5f;
        const float cy = item.GetCenter().y;
        drawMark(draw_list,
                 ImRect(ImVec2(cx - side * 0.5f, cy - side * 0.5f),
                        ImVec2(cx + side * 0.5f, cy + side * 0.5f)),
                 kWindowText, radio);
    }
    return pressed;
}

// A menu opened from the menu bar is a popup of its own parent, so IsPopupOpen()
// alone does not see it; look for it in the stack.
bool menuPopupOpen(ImGuiID id) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int n = 0; n < g.OpenPopupStack.Size; ++n) {
        if (g.OpenPopupStack[n].PopupId == id) {
            return true;
        }
    }
    return false;
}

void drawTrackbar(const ImRect& item, float position, bool held, bool disabled) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImRect bb = snapr(item);

    // Cover what ImGui has just drawn; a trackbar is a channel with a thumb on
    // it, not a filled bar.
    fillRect(draw_list, bb.Min.x, bb.Min.y, bb.Max.x, bb.Max.y, disabled ? kLight : kMenu);

    const float grab = std::min(style.GrabMinSize, bb.GetWidth() - 4.0f);
    const float from = bb.Min.x + 2.0f + grab * 0.5f;
    const float to = bb.Max.x - 2.0f - grab * 0.5f;
    const float center = from + position * (to - from);
    const ImRect thumb(center - grab * 0.5f, bb.Min.y, center + grab * 0.5f, bb.Max.y);

    // The channel is a groove etched into the face of the control.
    const float channel_y = snapf(bb.GetCenter().y) - 1.0f;
    fillRect(draw_list, bb.Min.x, channel_y, bb.Max.x, channel_y + 1.0f, kShadow);
    fillRect(draw_list, bb.Min.x, channel_y + 1.0f, bb.Max.x, channel_y + 2.0f, kHighlight);

    bevel(draw_list, thumb.Min, thumb.Max, !held, true, kBevel);

    // The grip of the thumb: two grooves in the middle.
    const float grip_x = snapf(thumb.GetCenter().x) - 2.0f;
    for (int i = 0; i < 2; ++i) {
        const float x = grip_x + static_cast<float>(i) * 3.0f;
        fillRect(draw_list, x, thumb.Min.y + 2.0f, x + 1.0f, thumb.Max.y - 2.0f, kHighlight);
        fillRect(draw_list, x + 1.0f, thumb.Min.y + 2.0f, x + 2.0f, thumb.Max.y - 2.0f, kShadow);
    }
}

// The width a slider is about to use, which is the width of its frame. It has
// to be read before the slider is submitted, because afterwards the item
// rectangle of the slider also covers its label, and the trackbar must not.
float nextItemWidth() {
    return ImGui::CalcItemWidth();
}

void drawTrackbarItem(const ImRect& frame, float value, float min, float max) {
    const float range = max - min;
    const float position = range > 0.0f ? std::clamp((value - min) / range, 0.0f, 1.0f) : 0.0f;
    const bool disabled = itemDisabled();
    const bool held = !disabled && ImGui::IsItemActive();
    if (ImGui::IsItemVisible()) {
        drawTrackbar(frame, position, held, disabled);
    }
}

// The rectangle a maximized window restores to. ImGui has no maximize of its
// own, so the state lives here, keyed by the window id.
struct RestoreRect {
    ImVec2 pos = ImVec2(0.0f, 0.0f);
    ImVec2 size = ImVec2(0.0f, 0.0f);
    bool maximized = false;
};

RestoreRect& restoreRect(ImGuiID id) {
    static std::unordered_map<ImGuiID, RestoreRect> rects;
    return rects[id];
}

// kind: 0 minimize, 1 maximize or restore, 2 close.
void captionGlyph(ImDrawList* draw_list, const ImRect& box, int kind, bool held, bool maximized) {
    const float offset = held ? 1.0f : 0.0f;
    const float cx = snapf(box.GetCenter().x) + offset;
    const float cy = snapf(box.GetCenter().y) + offset;
    if (kind == 0) {
        fillRect(draw_list, cx - 3.0f, cy + 2.0f, cx + 4.0f, cy + 3.0f, kWindowText);
        return;
    }
    if (kind == 2) {
        draw_list->AddLine(ImVec2(cx - 3.0f, cy - 3.0f), ImVec2(cx + 4.0f, cy + 4.0f), kWindowText,
                           2.0f);
        draw_list->AddLine(ImVec2(cx - 3.0f, cy + 4.0f), ImVec2(cx + 4.0f, cy - 3.0f), kWindowText,
                           2.0f);
        return;
    }
    const float x0 = cx - 4.0f;
    const float y0 = cy - 4.0f;
    const float x1 = cx + 4.0f;
    const float y1 = cy + 3.0f;
    if (!maximized) {
        // A frame with a thicker title bar.
        fillRect(draw_list, x0, y0, x1, y0 + 2.0f, kWindowText);
        fillRect(draw_list, x0, y0, x0 + 1.0f, y1, kWindowText);
        fillRect(draw_list, x1 - 1.0f, y0, x1, y1, kWindowText);
        fillRect(draw_list, x0, y1 - 1.0f, x1, y1, kWindowText);
        return;
    }
    // Two overlapping frames, the back one peeking out at the top right.
    fillRect(draw_list, x0 + 3.0f, y0 - 2.0f, x1, y0 - 1.0f, kWindowText);
    fillRect(draw_list, x1 - 1.0f, y0 - 2.0f, x1, y0 + 1.0f, kWindowText);
    fillRect(draw_list, x0, y0, x1 - 2.0f, y0 + 1.0f, kWindowText);
    fillRect(draw_list, x0, y0, x0 + 1.0f, y1, kWindowText);
    fillRect(draw_list, x1 - 3.0f, y0, x1 - 2.0f, y1, kWindowText);
    fillRect(draw_list, x0, y1 - 1.0f, x1 - 2.0f, y1, kWindowText);
}

}  // namespace

ImFont* loadUiFont(const char** used_file) {
    ImFontAtlas& atlas = *ImGui::GetIO().Fonts;
    if (used_file != nullptr) {
        *used_file = "<built-in>";
    }

    // Pixel snapping keeps the advances on whole pixels, which is what makes
    // the text land on the same grid as the bevels around it.
    ImFontConfig config;
    config.PixelSnapH = true;
    config.OversampleH = 1;
    config.OversampleV = 1;

    for (const char* path : kUiFontFiles) {
        if (!std::filesystem::exists(path)) {
            continue;
        }
        if (ImFont* font = atlas.AddFontFromFileTTF(path, kFontSize, &config)) {
            if (used_file != nullptr) {
                *used_file = path;
            }
            return font;
        }
    }
    return atlas.AddFontDefault(&config);
}

void applyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Windows 98 has no rounded corners anywhere and no soft borders: every
    // edge is a hard bevel drawn by this file.
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.MenuItemRounding = 0.0f;
    style.SelectableRounding = 0.0f;
    style.ImageRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 0.0f;
    style.AntiAliasedLines = false;
    style.AntiAliasedLinesUseTex = false;
    style.AntiAliasedFill = false;
    style.CurveTessellationTol = 0.1f;

    // The caption buttons of this file replace ImGui's own ones.
    style.WindowMenuButtonPosition = ImGuiDir_None;

    // MS Sans Serif at 11px has a 13px line, so 4px of padding gives the 21px
    // height of a Win98 push button.
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(6.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.CellPadding = ImVec2(4.0f, 2.0f);
    style.WindowPadding = ImVec2(7.0f, 7.0f);
    style.DisplayWindowPadding = ImVec2(0.0f, 0.0f);
    style.ScrollbarSize = kScrollbarSize;
    style.ScrollbarPadding = 0.0f;
    style.GrabMinSize = 11.0f;
    style.IndentSpacing = 20.0f;
    style.SeparatorSize = 2.0f;
    style.MouseCursorScale = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = rgba(kWindowText);
    colors[ImGuiCol_TextDisabled] = rgba(kGrayText);
    colors[ImGuiCol_WindowBg] = rgba(kMenu);
    colors[ImGuiCol_ChildBg] = rgba(kMenu);
    colors[ImGuiCol_PopupBg] = rgba(kMenu);
    colors[ImGuiCol_Border] = rgba(kDarkShadow);
    colors[ImGuiCol_BorderShadow] = rgba(kShadow);
    colors[ImGuiCol_FrameBg] = rgba(kMenu);
    colors[ImGuiCol_FrameBgHovered] = rgba(kMenu);
    colors[ImGuiCol_FrameBgActive] = rgba(kMenu);
    colors[ImGuiCol_TitleBg] = rgba(kInactiveCaption);
    colors[ImGuiCol_TitleBgActive] = rgba(kActiveCaption);
    colors[ImGuiCol_TitleBgCollapsed] = rgba(kInactiveCaption);
    colors[ImGuiCol_MenuBarBg] = rgba(kMenu);
    colors[ImGuiCol_ScrollbarBg] = rgba(kLight);
    colors[ImGuiCol_ScrollbarGrab] = rgba(kMenu);
    colors[ImGuiCol_ScrollbarGrabHovered] = rgba(kMenu);
    colors[ImGuiCol_ScrollbarGrabActive] = rgba(kMenu);
    colors[ImGuiCol_CheckMark] = rgba(kWindowText);
    colors[ImGuiCol_CheckboxSelectedBg] = rgba(kMenu);
    colors[ImGuiCol_SliderGrab] = rgba(kMenu);
    colors[ImGuiCol_SliderGrabActive] = rgba(kMenu);
    colors[ImGuiCol_Button] = rgba(kMenu);
    colors[ImGuiCol_ButtonHovered] = rgba(kMenu);
    colors[ImGuiCol_ButtonActive] = rgba(kMenu);
    colors[ImGuiCol_Header] = rgba(kMenu);
    colors[ImGuiCol_HeaderHovered] = rgba(kLight);
    colors[ImGuiCol_HeaderActive] = rgba(kLight);
    colors[ImGuiCol_Separator] = rgba(kShadow);
    colors[ImGuiCol_SeparatorHovered] = rgba(kShadow);
    colors[ImGuiCol_SeparatorActive] = rgba(kDarkShadow);
    colors[ImGuiCol_ResizeGrip] = rgba(IM_COL32(0, 0, 0, 0));
    colors[ImGuiCol_ResizeGripHovered] = rgba(IM_COL32(0, 0, 0, 0));
    colors[ImGuiCol_ResizeGripActive] = rgba(IM_COL32(0, 0, 0, 0));
    colors[ImGuiCol_InputTextCursor] = rgba(kWindowText);
    colors[ImGuiCol_TabHovered] = rgba(kLight);
    colors[ImGuiCol_Tab] = rgba(kMenu);
    colors[ImGuiCol_TabSelected] = rgba(kMenu);
    colors[ImGuiCol_TabSelectedOverline] = rgba(kActiveCaption);
    colors[ImGuiCol_TabDimmed] = rgba(kMenu);
    colors[ImGuiCol_TabDimmedSelected] = rgba(kMenu);
    colors[ImGuiCol_TabDimmedSelectedOverline] = rgba(kInactiveCaption);
    colors[ImGuiCol_PlotLines] = rgba(kActiveCaption);
    colors[ImGuiCol_PlotLinesHovered] = rgba(kWindowText);
    colors[ImGuiCol_PlotHistogram] = rgba(kActiveCaption);
    colors[ImGuiCol_PlotHistogramHovered] = rgba(kWindowText);
    colors[ImGuiCol_TableHeaderBg] = rgba(kLight);
    colors[ImGuiCol_TableBorderStrong] = rgba(kShadow);
    colors[ImGuiCol_TableBorderLight] = rgba(kLight);
    colors[ImGuiCol_TableRowBg] = rgba(kWindow);
    colors[ImGuiCol_TableRowBgAlt] = rgba(kMenu);
    colors[ImGuiCol_TextLink] = rgba(kActiveCaption);
    colors[ImGuiCol_TextSelectedBg] = rgba(kActiveCaption);
    colors[ImGuiCol_TreeLines] = rgba(kShadow);
    colors[ImGuiCol_DragDropTarget] = rgba(kActiveCaption);
    colors[ImGuiCol_DragDropTargetBg] = rgba(IM_COL32(0x00, 0x00, 0x80, 0x40));
    colors[ImGuiCol_UnsavedMarker] = rgba(kActiveCaption);
    colors[ImGuiCol_NavCursor] = rgba(IM_COL32(0x00, 0x00, 0x00, 0x60));
    colors[ImGuiCol_NavWindowingHighlight] = rgba(kActiveCaption);
    colors[ImGuiCol_NavWindowingDimBg] = rgba(IM_COL32(0x00, 0x00, 0x00, 0x40));
    colors[ImGuiCol_ModalWindowDimBg] = rgba(IM_COL32(0x00, 0x00, 0x00, 0x40));
}

void stylePlots() {
    ImPlotStyle& style = ImPlot::GetStyle();
    style.PlotBorderSize = 1.0f;
    style.MajorGridSize = ImVec2(1.0f, 1.0f);
    style.MinorGridSize = ImVec2(0.0f, 0.0f);
    style.MajorTickLen = ImVec2(4.0f, 4.0f);
    style.MinorTickLen = ImVec2(2.0f, 2.0f);
    style.MinorAlpha = 0.0f;
    style.PlotPadding = ImVec2(4.0f, 4.0f);
    style.LabelPadding = ImVec2(4.0f, 2.0f);

    style.Colors[ImPlotCol_FrameBg] = rgba(kMenu);
    style.Colors[ImPlotCol_PlotBg] = rgba(kWindow);
    style.Colors[ImPlotCol_PlotBorder] = rgba(kShadow);
    style.Colors[ImPlotCol_LegendBg] = rgba(kMenu);
    style.Colors[ImPlotCol_LegendBorder] = rgba(kShadow);
    style.Colors[ImPlotCol_LegendText] = rgba(kWindowText);
    style.Colors[ImPlotCol_TitleText] = rgba(kWindowText);
    style.Colors[ImPlotCol_InlayText] = rgba(kWindowText);
    style.Colors[ImPlotCol_AxisText] = rgba(kWindowText);
    style.Colors[ImPlotCol_AxisGrid] = rgba(kLight);
    style.Colors[ImPlotCol_AxisTick] = rgba(kShadow);
    style.Colors[ImPlotCol_AxisBg] = rgba(IM_COL32(0, 0, 0, 0));
    style.Colors[ImPlotCol_AxisBgHovered] = rgba(kLight);
    style.Colors[ImPlotCol_AxisBgActive] = rgba(kLight);
    style.Colors[ImPlotCol_Selection] = rgba(IM_COL32(0x00, 0x00, 0x80, 0x40));
    style.Colors[ImPlotCol_Crosshairs] = rgba(kShadow);
}

void bevel(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max, bool raised, bool filled,
           float thickness) {
    const ImRect bb = snapr(ImRect(min, max));
    const float outer = 1.0f;
    const float inner = thickness - outer;

    const ImU32 outer_tl = raised ? kHighlight : kShadow;
    const ImU32 outer_br = raised ? kDarkShadow : kHighlight;
    const ImU32 inner_tl = raised ? kLight : kDarkShadow;
    const ImU32 inner_br = raised ? kShadow : kLight;

    if (filled) {
        fillRect(draw_list, bb.Min.x, bb.Min.y, bb.Max.x, bb.Max.y, kMenu);
    }
    fillRect(draw_list, bb.Min.x, bb.Min.y, bb.Max.x, bb.Min.y + outer, outer_tl);
    fillRect(draw_list, bb.Min.x, bb.Min.y, bb.Min.x + outer, bb.Max.y, outer_tl);
    fillRect(draw_list, bb.Min.x, bb.Max.y - outer, bb.Max.x, bb.Max.y, outer_br);
    fillRect(draw_list, bb.Max.x - outer, bb.Min.y, bb.Max.x, bb.Max.y, outer_br);

    if (inner > 0.0f) {
        fillRect(draw_list, bb.Min.x + outer, bb.Min.y + outer, bb.Max.x - outer,
                 bb.Min.y + thickness, inner_tl);
        fillRect(draw_list, bb.Min.x + outer, bb.Min.y + outer, bb.Min.x + thickness,
                 bb.Max.y - outer, inner_tl);
        fillRect(draw_list, bb.Min.x + outer, bb.Max.y - thickness, bb.Max.x - outer, bb.Max.y,
                 inner_br);
        fillRect(draw_list, bb.Max.x - thickness, bb.Min.y + outer, bb.Max.x - outer,
                 bb.Max.y - outer, inner_br);
    }
}

void etchedLine(ImDrawList* draw_list, float x0, float x1, float y) {
    const float top = snapf(y);
    fillRect(draw_list, x0, top, x1, top + 1.0f, kShadow);
    fillRect(draw_list, x0, top + 1.0f, x1, top + 2.0f, kHighlight);
}

void sizeGrip(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max) {
    const ImRect bb = snapr(ImRect(min, max));
    const float ox = bb.Max.x - kWindowFrame - 2.0f;
    const float oy = bb.Max.y - kWindowFrame - 2.0f;
    // Three short diagonals stepping towards the corner, each with a shadow
    // one pixel below and right.
    for (int i = 0; i < 3; ++i) {
        const float off = static_cast<float>(i) * 4.0f;
        const ImVec2 from(ox - off - 4.0f, oy - off);
        const ImVec2 to(ox - off, oy - off - 4.0f);
        draw_list->AddLine(ImVec2(from.x + 1.0f, from.y + 1.0f), ImVec2(to.x + 1.0f, to.y + 1.0f),
                           kShadow);
        draw_list->AddLine(from, to, kHighlight);
    }
}

void checkMark(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max, bool checked) {
    if (!checked) {
        return;
    }
    drawMark(draw_list, ImRect(min, max), kWindowText, false);
}

void plusMinus(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max, bool open) {
    const ImRect bb = snapr(ImRect(min, max));
    const float cx = snapf(bb.GetCenter().x);
    const float cy = snapf(bb.GetCenter().y);
    fillRect(draw_list, bb.Min.x + 2.0f, cy - 1.0f, bb.Max.x - 2.0f, cy + 1.0f, kWindowText);
    if (!open) {
        fillRect(draw_list, cx - 1.0f, bb.Min.y + 2.0f, cx + 1.0f, bb.Max.y - 2.0f, kWindowText);
    }
}

void sunkenBorder(const ImVec2& min, const ImVec2& max) {
    bevel(ImGui::GetWindowDrawList(), min, max, false, false, 1.0f);
}

bool button(const char* label, const ImVec2& size) {
    const bool pressed = ImGui::Button(label, size);
    if (!ImGui::IsItemVisible()) {
        return pressed;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImRect bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const bool disabled = itemDisabled();
    const bool held = !disabled && ImGui::IsItemActive();

    bevel(draw_list, bb.Min, bb.Max, !held, true, kBevel);

    // Win98 keeps the face colour and sinks the button instead; the label moves
    // along with it. A disabled button keeps its bevel and greys the label,
    // where Win98 would dither it. Everything from a "##" marker on is the
    // item id and is not part of the label.
    const char* label_end = renderedTextEnd(label);
    const float offset = held ? 1.0f : 0.0f;
    const ImVec2 text_size = ImGui::CalcTextSize(label, label_end, false);
    draw_list->AddText(ImVec2(snapf(bb.Min.x + (bb.GetWidth() - text_size.x) * 0.5f) + offset,
                              snapf(bb.Min.y + (bb.GetHeight() - text_size.y) * 0.5f) + offset),
                       disabled ? kGrayText : kWindowText, label, label_end);
    return pressed;
}

namespace {

// Draws a transport symbol centred in a button, from solid primitives so no
// icon font or image is needed. All coordinates land on the pixel grid.
void drawTransportIcon(ImDrawList* draw_list, const ImRect& box, TransportIcon icon, ImU32 color,
                       float offset) {
    const float cx = snapf(box.GetCenter().x) + offset;
    const float cy = snapf(box.GetCenter().y) + offset;
    const float h = snapf(std::min(box.GetWidth(), box.GetHeight()) * 0.3f);
    switch (icon) {
        case TransportIcon::Play:
            draw_list->AddTriangleFilled(ImVec2(snapf(cx - h * 0.45f), snapf(cy - h)),
                                         ImVec2(snapf(cx - h * 0.45f), snapf(cy + h)),
                                         ImVec2(snapf(cx + h * 0.75f), cy), color);
            break;
        case TransportIcon::Pause: {
            const float bar = std::max(2.0f, snapf(h * 0.4f));
            const float gap = std::max(1.0f, snapf(h * 0.35f));
            fillRect(draw_list, cx - gap - bar, cy - h, cx - gap, cy + h, color);
            fillRect(draw_list, cx + gap, cy - h, cx + gap + bar, cy + h, color);
            break;
        }
        case TransportIcon::Stop: {
            const float side = std::max(5.0f, snapf(h * 1.5f));
            fillRect(draw_list, cx - side * 0.5f, cy - side * 0.5f, cx + side * 0.5f,
                     cy + side * 0.5f, color);
            break;
        }
        case TransportIcon::Previous:
        case TransportIcon::Next: {
            // |< and >|: a bar on the outer edge and a triangle whose point
            // sits next to it.
            const float dir = icon == TransportIcon::Next ? 1.0f : -1.0f;
            const float bar_inner = cx + dir * h * 0.5f;
            const float bar_outer = cx + dir * h * 0.8f;
            fillRect(draw_list, std::min(bar_inner, bar_outer), cy - h,
                     std::max(bar_inner, bar_outer), cy + h, color);
            draw_list->AddTriangleFilled(ImVec2(snapf(cx - dir * h * 0.65f), snapf(cy - h)),
                                         ImVec2(snapf(cx - dir * h * 0.65f), snapf(cy + h)),
                                         ImVec2(snapf(cx + dir * h * 0.35f), cy), color);
            break;
        }
    }
}

}  // namespace

bool iconButton(const char* id, TransportIcon icon, const ImVec2& size) {
    const bool pressed = ImGui::Button(id, size);
    if (!ImGui::IsItemVisible()) {
        return pressed;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImRect bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const bool disabled = itemDisabled();
    const bool held = !disabled && ImGui::IsItemActive();

    bevel(draw_list, bb.Min, bb.Max, !held, true, kBevel);
    drawTransportIcon(draw_list, bb, icon, disabled ? kGrayText : kWindowText, held ? 1.0f : 0.0f);
    return pressed;
}

bool checkbox(const char* label, bool* value) {
    // Without any vertical padding the box is exactly one font line tall, and
    // the label lands right next to it the way Win98 draws it.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    const bool changed = ImGui::Checkbox(label, value);
    ImGui::PopStyleVar();
    if (!ImGui::IsItemVisible()) {
        return changed;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImRect bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const float side = ImGui::GetFontSize();
    fillRect(draw_list, bb.Min.x, bb.Min.y, bb.Min.x + side, bb.Min.y + side, kWindow);
    bevel(draw_list, bb.Min, ImVec2(bb.Min.x + side, bb.Min.y + side), false, false, 1.0f);
    checkMark(draw_list, bb.Min, ImVec2(bb.Min.x + side, bb.Min.y + side), *value);
    return changed;
}

bool inputText(const char* label, char* buffer, size_t buffer_size, ImGuiInputTextFlags flags) {
    // Win98 text fields are a white sunken well; ImGui's frame background is
    // the window colour, so it is repainted around the call.
    const ImVec4 white = rgba(kWindow);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, white);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, white);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, white);
    const bool changed = ImGui::InputText(label, buffer, buffer_size, flags);
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemVisible()) {
        const ImRect bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        bevel(ImGui::GetWindowDrawList(), bb.Min, bb.Max, false, false, 1.0f);
    }
    return changed;
}

bool sliderFloat(const char* label, float* value, float min, float max, ImGuiSliderFlags flags) {
    const float width = nextItemWidth();
    // The empty format keeps ImGui from printing the value over the trackbar.
    const bool changed = ImGui::SliderFloat(label, value, min, max, "", flags);
    const ImVec2 origin = ImGui::GetItemRectMin();
    drawTrackbarItem(ImRect(origin, ImVec2(origin.x + width, origin.y + ImGui::GetFrameHeight())),
                     *value, min, max);
    return changed;
}

bool sliderInt(const char* label, int* value, int min, int max, ImGuiSliderFlags flags) {
    const float width = nextItemWidth();
    const bool changed = ImGui::SliderInt(label, value, min, max, "", flags);
    const ImVec2 origin = ImGui::GetItemRectMin();
    drawTrackbarItem(ImRect(origin, ImVec2(origin.x + width, origin.y + ImGui::GetFrameHeight())),
                     static_cast<float>(*value), static_cast<float>(min),
                     static_cast<float>(max));
    return changed;
}

void separator() {
    ImGui::Separator();
    if (!ImGui::IsItemVisible()) {
        return;
    }
    const ImRect bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    etchedLine(ImGui::GetWindowDrawList(), bb.Min.x, bb.Max.x, bb.Min.y);
}

bool collapsingHeader(const char* label, ImGuiTreeNodeFlags flags) {
    // ImGui leaves room for its own arrow in front of the label, which is more
    // than the Win98 expander needs.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(2.0f, ImGui::GetStyle().FramePadding.y));
    const bool open = ImGui::CollapsingHeader(label, flags);
    ImGui::PopStyleVar();
    if (!ImGui::IsItemVisible()) {
        return open;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImRect bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ImGui draws a triangle in the space it reserves in front of the label;
    // wipe it and put the Win98 expander there instead.
    const float box_x = bb.Min.x + IM_TRUNC(style.WindowPadding.x * 0.5f) + style.FramePadding.x;
    const float box_y = bb.Min.y + (bb.GetHeight() - 9.0f) * 0.5f;
    fillRect(draw_list, bb.Min.x, bb.Min.y, box_x + ImGui::GetFontSize(), bb.Max.y,
             ImGui::IsItemHovered() ? kLight : kMenu);
    plusMinus(draw_list, ImVec2(box_x, box_y), ImVec2(box_x + 9.0f, box_y + 9.0f), open);
    return open;
}

bool beginMenu(const char* label) {
    ImGuiWindow* bar = ImGui::GetCurrentWindow();
    const ImVec2 pos = bar->DC.CursorPos;
    const float text_y = pos.y + bar->DC.CurrLineTextBaseOffset;
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    const ImGuiID id = bar->GetID(label);

    const bool open = ImGui::BeginMenu(label);
    const bool highlighted = open && menuPopupOpen(id);
    const float text_x = snapf(pos.x) + IM_TRUNC(ImGui::GetStyle().ItemSpacing.x * 0.5f);

    // Win98 only highlights a menu bar entry while its menu is open.
    if (highlighted) {
        ImDrawList* draw_list = bar->DrawList;
        const float left = snapf(pos.x) + 2.0f;
        const ImRect row(left, snapf(pos.y), left + label_size.x + 6.0f,
                         snapf(pos.y) + ImGui::GetFrameHeight());
        fillRect(draw_list, row.Min.x, row.Min.y, row.Max.x, row.Max.y, kActiveCaption);
        draw_list->AddText(ImVec2(text_x, text_y), kCaptionText, label, renderedTextEnd(label));
    }

    // Win98 underlines the accelerator key of every menu bar entry. There is
    // no keyboard navigation to attach here, so the first character stands in
    // for it, which is the letter Windows would underline for these menus.
    const float first_width = ImGui::CalcTextSize(label, label + 1, false).x;
    const float underline_y = snapf(text_y) + ImGui::GetFontSize() - 1.0f;
    bar->DrawList->AddLine(ImVec2(text_x, underline_y), ImVec2(text_x + first_width, underline_y),
                           highlighted ? kCaptionText : kWindowText, 1.0f);
    return open;
}

void endMenu() {
    ImGui::EndMenu();
}

bool menuItem(const char* label, const char* shortcut, bool selected, bool enabled) {
    return menuItemImpl(label, shortcut, selected, enabled, false);
}

bool menuItemRadio(const char* label, const char* shortcut, bool selected, bool enabled) {
    return menuItemImpl(label, shortcut, selected, enabled, true);
}

bool menuItemToggle(const char* label, const char* shortcut, bool* selected) {
    if (menuItem(label, shortcut, *selected, true)) {
        *selected = !*selected;
        return true;
    }
    return false;
}

void endMainMenuBar() {
    ImGui::EndMainMenuBar();

    ImGuiWindow* bar = ImGui::FindWindowByName("##MainMenuBar");
    if (bar == nullptr || bar->DrawList == nullptr) {
        return;
    }
    // The menu bar ends in a groove, which is what separates it from the client
    // area of the window below.
    const ImRect bb = bar->Rect();
    etchedLine(bar->DrawList, bb.Min.x, bb.Max.x, bb.Max.y - 2.0f);
}

void windowFrame(const char* window_name, bool* open) {
    ImGuiWindow* window = ImGui::FindWindowByName(window_name);
    if (window == nullptr || window->DrawList == nullptr || window->Hidden ||
        window->Size.x <= 0.0f || window->Size.y <= 0.0f) {
        return;
    }
    ImDrawList* draw_list = window->DrawList;
    const ImRect bb = window->Rect();
    // ImGui paints the caption while End() runs, with the title of the focused
    // window in the active caption colour. Repeat that decision here, otherwise
    // the text would end up black on navy.
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiWindow* highlighted = g.NavWindowingTarget ? g.NavWindowingTarget : g.NavWindow;
    const bool active = (window->LastFrameActive < g.FrameCount - 1) ||
                        (highlighted != nullptr &&
                         window->RootWindowForTitleBarHighlight ==
                             highlighted->RootWindowForTitleBarHighlight);

    bevel(draw_list, bb.Min, bb.Max, true, false, kWindowFrame);

    if (!(window->Flags & ImGuiWindowFlags_NoTitleBar)) {
        const float inset = kWindowFrame;
        const float title_top = snapf(bb.Min.y);
        const float title_bottom = snapf(bb.Min.y + window->TitleBarHeight);
        const float top = snapf(bb.Min.y + (window->TitleBarHeight - kCaptionButtonHeight) * 0.5f);

        // ImGui painted the caption in the system colour but its title text in
        // the body colour, which is unreadable on it. Repaint the strip that
        // holds the text with the Win98 gradient, then draw the title the way
        // Win98 does.
        const float text_x = bb.Min.x + inset;
        draw_list->AddRectFilledMultiColor(ImVec2(text_x, title_top),
                                           ImVec2(snapf(bb.Max.x) - inset, title_bottom),
                                           active ? kActiveCaption : kInactiveCaption,
                                           active ? kActiveCaptionEnd : kInactiveCaptionEnd,
                                           active ? kActiveCaptionEnd : kInactiveCaptionEnd,
                                           active ? kActiveCaption : kInactiveCaption);
        const char* label_end = renderedTextEnd(window->Name);
        if (label_end != window->Name) {
            const ImVec2 size = ImGui::CalcTextSize(window->Name, label_end, false);
            const ImVec2 pos(text_x + kCaptionButtonWidth + 6.0f,
                             snapf(bb.Min.y + (window->TitleBarHeight - size.y) * 0.5f));
            const ImU32 text_color = active ? kCaptionText : kInactiveCaptionText;
            // Win98 draws the caption in bold; the only bold face of the font
            // around is not, so smear it one pixel to the right instead.
            draw_list->AddText(pos, text_color, window->Name, label_end);
            draw_list->AddText(ImVec2(pos.x + 1.0f, pos.y), text_color, window->Name, label_end);
        }

        // The system menu box on the left is decoration only: there is no
        // system menu to open. The glyph is a miniature of a window.
        const ImRect system_box(text_x + 2.0f, top, text_x + 2.0f + kCaptionButtonWidth,
                                top + kCaptionButtonHeight);
        bevel(draw_list, system_box.Min, system_box.Max, true, true, kBevel);
        const float gx = snapf(system_box.Min.x) + 4.0f;
        const float gy = snapf(system_box.Min.y) + 4.0f;
        fillRect(draw_list, gx, gy, gx + 7.0f, gy + 1.0f, kActiveCaption);
        fillRect(draw_list, gx, gy + 1.0f, gx + 1.0f, gy + 5.0f, kWindowText);
        fillRect(draw_list, gx + 6.0f, gy + 1.0f, gx + 7.0f, gy + 5.0f, kWindowText);
        fillRect(draw_list, gx, gy + 4.0f, gx + 7.0f, gy + 5.0f, kWindowText);

        if (open != nullptr) {
            // The three caption buttons of a Win98 window, laid out from the
            // right edge: minimize, maximize/restore and close. They are not
            // items of the window, so their state comes straight from the
            // mouse.
            const float right = snapf(bb.Max.x) - inset - 2.0f;
            const ImRect close_box(right - kCaptionButtonWidth, top, right,
                                   top + kCaptionButtonHeight);
            const ImRect maximize_box(close_box.Min.x - 2.0f - kCaptionButtonWidth, top,
                                      close_box.Min.x - 2.0f, top + kCaptionButtonHeight);
            const ImRect minimize_box(maximize_box.Min.x - 2.0f - kCaptionButtonWidth, top,
                                      maximize_box.Min.x - 2.0f, top + kCaptionButtonHeight);

            RestoreRect& restore = restoreRect(window->ID);
            const ImRect boxes[3] = {minimize_box, maximize_box, close_box};
            for (int i = 0; i < 3; ++i) {
                const ImRect box = snapr(boxes[i]);
                const bool hovered = ImGui::IsMouseHoveringRect(box.Min, box.Max, false);
                const bool held = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
                const bool clicked = held && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
                bevel(draw_list, box.Min, box.Max, !held, true, kBevel);
                captionGlyph(draw_list, box, i, held, restore.maximized);
                if (!clicked) {
                    continue;
                }
                if (i == 0) {
                    ImGui::SetWindowCollapsed(window_name, true);
                } else if (i == 1) {
                    ImGui::SetWindowCollapsed(window_name, false);
                    if (restore.maximized) {
                        ImGui::SetWindowPos(window_name, restore.pos, ImGuiCond_Always);
                        ImGui::SetWindowSize(window_name, restore.size, ImGuiCond_Always);
                        restore.maximized = false;
                    } else {
                        const ImGuiViewport* viewport = ImGui::GetMainViewport();
                        restore.pos = window->Pos;
                        restore.size = window->Size;
                        ImGui::SetWindowPos(window_name, viewport->WorkPos, ImGuiCond_Always);
                        ImGui::SetWindowSize(window_name, viewport->WorkSize, ImGuiCond_Always);
                        restore.maximized = true;
                    }
                } else {
                    *open = false;
                }
            }
        }
    }

    if (!(window->Flags & ImGuiWindowFlags_NoResize)) {
        sizeGrip(draw_list, bb.Min, bb.Max);
    }
}
void decoratePopups() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int i = 0; i < g.BeginPopupStack.Size; ++i) {
        ImGuiWindow* window = g.BeginPopupStack[i].Window;
        if (window == nullptr || window->DrawList == nullptr || window->Size.x <= 0.0f ||
            window->Size.y <= 0.0f) {
            continue;
        }
        ImDrawList* draw_list = window->DrawList;
        const ImVec2 max(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y);
        // The clip rect of a finished window is not ours to guess, so state it.
        draw_list->PushClipRect(window->Pos, max, true);
        if (window->Flags & ImGuiWindowFlags_Tooltip) {
            // Win98 tooltips are a pale yellow note with a black edge.
            fillRect(draw_list, window->Pos.x, window->Pos.y, max.x, max.y, kTooltipBg);
            fillRect(draw_list, window->Pos.x, window->Pos.y, max.x, window->Pos.y + 1.0f,
                     kWindowText);
            fillRect(draw_list, window->Pos.x, window->Pos.y, window->Pos.x + 1.0f, max.y,
                     kWindowText);
            fillRect(draw_list, window->Pos.x, max.y - 1.0f, max.x, max.y, kWindowText);
            fillRect(draw_list, max.x - 1.0f, window->Pos.y, max.x, max.y, kWindowText);
        } else {
            bevel(draw_list, window->Pos, max, true, false, kBevel);
        }
        draw_list->PopClipRect();
    }
}

}  // namespace player::win98