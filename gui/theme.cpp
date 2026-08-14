/**
 * @file theme.cpp
 * @brief Burst Download 深色主题实现（视觉规格附录 C，见 theme.h）
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */

#include "theme.h"

#include "imgui.h"

namespace theme {

namespace {

/** @brief 便捷颜色构造：0xRRGGBB + 不透明度 */
ImVec4 C(unsigned r, unsigned g, unsigned b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

}  // namespace

void ApplyBurst() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* pCol = s.Colors;

    /* C.1 palette. */
    const ImVec4 cBg          = C(0x28, 0x2C, 0x34);
    const ImVec4 cPanel       = C(0x21, 0x25, 0x2B);
    const ImVec4 cPanelDeep   = C(0x18, 0x1C, 0x23);
    const ImVec4 cTooltip     = C(0x18, 0x1B, 0x21, 0.95f);
    const ImVec4 cTrack       = C(0x3B, 0x40, 0x50);
    const ImVec4 cText        = C(0xAB, 0xB2, 0xBF);
    const ImVec4 cTextDim     = C(0xA1, 0xA1, 0xAA);
    const ImVec4 cTextFaint   = C(0x71, 0x71, 0x7A);
    const ImVec4 cTextStrong  = C(0xE4, 0xE4, 0xE7);
    const ImVec4 cBorder      = C(0x3F, 0x3F, 0x46, 0.6f);
    const ImVec4 cHover       = C(0x3F, 0x3F, 0x46, 0.40f);
    const ImVec4 cBlue        = C(0x3B, 0x82, 0xF6);
    const ImVec4 cBlueHov     = C(0x60, 0xA5, 0xFA);
    const ImVec4 cDivider     = C(0, 0, 0, 102);

    pCol[ImGuiCol_Text]                  = cText;
    pCol[ImGuiCol_TextDisabled]          = cTextFaint;
    pCol[ImGuiCol_WindowBg]              = cBg;
    pCol[ImGuiCol_ChildBg]               = cPanel;
    pCol[ImGuiCol_PopupBg]               = cPanel;
    pCol[ImGuiCol_Border]                = cBorder;
    pCol[ImGuiCol_BorderShadow]          = C(0, 0, 0, 0);
    pCol[ImGuiCol_FrameBg]               = cPanel;
    pCol[ImGuiCol_FrameBgHovered]        = cHover;
    pCol[ImGuiCol_FrameBgActive]         = C(0x3F, 0x3F, 0x46, 0.55f);
    pCol[ImGuiCol_TitleBg]               = cPanel;
    pCol[ImGuiCol_TitleBgActive]         = cPanel;
    pCol[ImGuiCol_TitleBgCollapsed]      = cPanel;
    pCol[ImGuiCol_MenuBarBg]             = cPanel;
    pCol[ImGuiCol_ScrollbarBg]           = cTooltip;
    pCol[ImGuiCol_ScrollbarGrab]         = C(0x3F, 0x3F, 0x46);
    pCol[ImGuiCol_ScrollbarGrabHovered]  = C(0x52, 0x52, 0x5B);
    pCol[ImGuiCol_ScrollbarGrabActive]   = C(0x71, 0x71, 0x7A);
    pCol[ImGuiCol_CheckMark]             = cBlue;
    pCol[ImGuiCol_CheckboxSelectedBg]    = C(0x3B, 0x82, 0xF6, 0.25f);
    pCol[ImGuiCol_SliderGrab]            = cBlue;
    pCol[ImGuiCol_SliderGrabActive]      = cBlueHov;
    pCol[ImGuiCol_Button]                = cPanel;
    pCol[ImGuiCol_ButtonHovered]         = cHover;
    pCol[ImGuiCol_ButtonActive]          = C(0x3F, 0x3F, 0x46, 0.55f);
    pCol[ImGuiCol_Header]                = cPanel;
    pCol[ImGuiCol_HeaderHovered]         = cHover;
    pCol[ImGuiCol_HeaderActive]          = C(0x3F, 0x3F, 0x46, 0.55f);
    pCol[ImGuiCol_Separator]             = cDivider;
    pCol[ImGuiCol_SeparatorHovered]      = cBlue;
    pCol[ImGuiCol_SeparatorActive]       = cBlue;
    pCol[ImGuiCol_ResizeGrip]            = cBorder;
    pCol[ImGuiCol_ResizeGripHovered]     = cBlue;
    pCol[ImGuiCol_ResizeGripActive]      = cBlueHov;
    pCol[ImGuiCol_Tab]                   = cPanel;
    pCol[ImGuiCol_TabHovered]            = cHover;
    pCol[ImGuiCol_TabSelected]           = C(0x3F, 0x3F, 0x46, 0.40f);
    pCol[ImGuiCol_TabSelectedOverline]   = cBlue;
    pCol[ImGuiCol_TabDimmed]             = cPanel;
    pCol[ImGuiCol_TabDimmedSelected]     = C(0x3F, 0x3F, 0x46, 0.30f);
    pCol[ImGuiCol_TabDimmedSelectedOverline] = C(0x3B, 0x82, 0xF6, 0.6f);
    pCol[ImGuiCol_TableHeaderBg]         = cPanel;
    pCol[ImGuiCol_TableBorderStrong]     = cBorder;
    pCol[ImGuiCol_TableBorderLight]      = C(0x3F, 0x3F, 0x46, 0.5f);
    pCol[ImGuiCol_TableRowBg]            = C(0, 0, 0, 0);
    pCol[ImGuiCol_TableRowBgAlt]         = C(0, 0, 0, 0.05f);
    pCol[ImGuiCol_TextSelectedBg]        = C(0x3B, 0x82, 0xF6, 0.35f);
    pCol[ImGuiCol_DragDropTarget]        = cBlue;
    pCol[ImGuiCol_NavCursor]             = cBlue;
    pCol[ImGuiCol_PlotLines]             = cText;
    pCol[ImGuiCol_PlotLinesHovered]      = cBlueHov;
    pCol[ImGuiCol_PlotHistogram]         = cBlue;
    pCol[ImGuiCol_PlotHistogramHovered]  = cBlueHov;
    pCol[ImGuiCol_ModalWindowDimBg]      = C(0, 0, 0, 0.55f);

    /* C.2 rounding / C.3 spacing. */
    s.WindowRounding       = 16.0f;
    s.ChildRounding        = 8.0f;
    s.FrameRounding        = 8.0f;
    s.PopupRounding        = 8.0f;
    s.ScrollbarRounding    = 8.0f;
    s.GrabRounding         = 8.0f;
    s.TabRounding          = 8.0f;
    s.WindowBorderSize     = 1.0f;
    s.ChildBorderSize      = 1.0f;
    s.PopupBorderSize      = 1.0f;
    s.FrameBorderSize      = 1.0f;
    s.WindowPadding        = ImVec2(16.0f, 16.0f);
    s.FramePadding         = ImVec2(12.0f, 8.0f);
    s.ItemSpacing          = ImVec2(8.0f, 8.0f);
    s.ItemInnerSpacing     = ImVec2(8.0f, 8.0f);
    s.ScrollbarSize        = 8.0f;
    s.GrabMinSize          = 10.0f;
    s.WindowTitleAlign     = ImVec2(0.5f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.AntiAliasedLines     = true;
    s.AntiAliasedFill      = true;
}

}  // namespace theme
