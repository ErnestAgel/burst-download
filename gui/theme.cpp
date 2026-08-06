/**
 * @file theme.cpp
 * @brief Atom One Dark 主题实现（见 theme.h）
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

void ApplyOneDark() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* col = s.Colors;

    /* ---- One Dark 调色板 ---- */
    const ImVec4 cBg        = C(0x28, 0x2C, 0x34);  /* 窗口背景 */
    const ImVec4 cPanel     = C(0x21, 0x25, 0x2B);  /* 面板/弹层 */
    const ImVec4 cPanelDeep = C(0x1B, 0x1E, 0x24);  /* 更深面板 */
    const ImVec4 cFrame     = C(0x3B, 0x40, 0x48);  /* 控件底 */
    const ImVec4 cFrameHov  = C(0x4B, 0x52, 0x63);
    const ImVec4 cFrameAct  = C(0x5B, 0x64, 0x72);
    const ImVec4 cBorder    = C(0x3E, 0x44, 0x51);
    const ImVec4 cText      = C(0xAB, 0xB2, 0xBF);
    const ImVec4 cTextDim   = C(0x5C, 0x63, 0x70);
    const ImVec4 cBlue      = C(0x61, 0xAF, 0xEF);
    const ImVec4 cBlueAct   = C(0x52, 0x8B, 0xFF);
    const ImVec4 cGreen     = C(0x98, 0xC3, 0x79);

    /* ---- 颜色映射 ---- */
    col[ImGuiCol_Text]                 = cText;
    col[ImGuiCol_TextDisabled]         = cTextDim;
    col[ImGuiCol_WindowBg]             = cBg;
    col[ImGuiCol_ChildBg]              = cPanel;
    col[ImGuiCol_PopupBg]              = cPanel;
    col[ImGuiCol_Border]               = cBorder;
    col[ImGuiCol_BorderShadow]         = C(0, 0, 0, 0);
    col[ImGuiCol_FrameBg]              = cFrame;
    col[ImGuiCol_FrameBgHovered]       = cFrameHov;
    col[ImGuiCol_FrameBgActive]        = cFrameAct;
    col[ImGuiCol_TitleBg]              = cPanelDeep;
    col[ImGuiCol_TitleBgActive]        = cPanelDeep;
    col[ImGuiCol_TitleBgCollapsed]     = cPanel;
    col[ImGuiCol_MenuBarBg]            = cPanel;
    col[ImGuiCol_ScrollbarBg]          = cPanelDeep;
    col[ImGuiCol_ScrollbarGrab]        = cBorder;
    col[ImGuiCol_ScrollbarGrabHovered] = cFrameHov;
    col[ImGuiCol_ScrollbarGrabActive]  = cFrameAct;
    col[ImGuiCol_CheckMark]            = cBlue;
    col[ImGuiCol_SliderGrab]           = cBlue;
    col[ImGuiCol_SliderGrabActive]     = cBlueAct;
    col[ImGuiCol_Button]               = cFrame;
    col[ImGuiCol_ButtonHovered]        = cFrameHov;
    col[ImGuiCol_ButtonActive]         = cFrameAct;
    col[ImGuiCol_Header]               = cFrame;
    col[ImGuiCol_HeaderHovered]        = cFrameHov;
    col[ImGuiCol_HeaderActive]         = cFrameAct;
    col[ImGuiCol_Separator]            = cBorder;
    col[ImGuiCol_SeparatorHovered]     = cBlue;
    col[ImGuiCol_SeparatorActive]      = cBlue;
    col[ImGuiCol_ResizeGrip]           = cBorder;
    col[ImGuiCol_ResizeGripHovered]    = cBlue;
    col[ImGuiCol_ResizeGripActive]     = cBlueAct;
    col[ImGuiCol_Tab]                  = cPanel;
    col[ImGuiCol_TabHovered]           = cFrameHov;
    col[ImGuiCol_TabActive]            = cFrame;
    col[ImGuiCol_TabUnfocused]         = cPanel;
    col[ImGuiCol_TabUnfocusedActive]   = cPanelDeep;
    col[ImGuiCol_TableHeaderBg]        = cPanel;
    col[ImGuiCol_TableBorderStrong]    = cBorder;
    col[ImGuiCol_TableBorderLight]     = cBorder;
    col[ImGuiCol_TextSelectedBg]       = C(0x61, 0xAF, 0xEF, 0.35f);
    col[ImGuiCol_DragDropTarget]       = cBlue;
    col[ImGuiCol_NavHighlight]         = cBlue;
    col[ImGuiCol_PlotLines]            = cText;
    col[ImGuiCol_PlotLinesHovered]     = cGreen;
    col[ImGuiCol_PlotHistogram]        = cBlue;       /* ProgressBar 前景 */
    col[ImGuiCol_PlotHistogramHovered] = cBlueAct;
    col[ImGuiCol_ModalWindowDimBg]     = C(0, 0, 0, 0.55f);

    /* ---- 样式（圆角/间距） ---- */
    s.WindowRounding    = 4.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 4.0f;
    s.PopupRounding     = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.GrabRounding      = 4.0f;
    s.TabRounding       = 4.0f;
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = ImVec2(8, 4);
    s.ItemSpacing       = ImVec2(8, 6);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 10.0f;
    s.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
    s.AntiAliasedLines  = true;
    s.AntiAliasedFill   = true;
    s.WindowMenuButtonPosition = ImGuiDir_None;
}

}  // namespace theme
