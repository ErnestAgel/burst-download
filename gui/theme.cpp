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
    /* 输入框底色对齐网页 Demo：#21252B 深面板 + zinc-700 描边 */
    const ImVec4 cFrame     = C(0x21, 0x25, 0x2B);  /* 控件底(网页输入框色) */
    const ImVec4 cFrameHov  = C(0x27, 0x2C, 0x34);
    const ImVec4 cFrameAct  = C(0x2C, 0x32, 0x3B);
    const ImVec4 cBorder    = C(0x3E, 0x44, 0x52);  /* 边框(zinc-700) */
    const ImVec4 cText      = C(0xD7, 0xDA, 0xE0);  /* 主文本(调亮,暗色下更清晰) */
    const ImVec4 cTextDim   = C(0x7A, 0x82, 0x92);
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
    col[ImGuiCol_Separator]            = C(0, 0, 0, 102);  /* 分割线 40% */
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
    s.ChildRounding     = 8.0f;   /* 任务列表容器圆角 (rounded-lg) */
    s.FrameRounding     = 8.0f;   /* 输入框/下拉圆角 (rounded-lg) */
    s.PopupRounding     = 8.0f;   /* About 下拉圆角 */
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding      = 8.0f;
    s.TabRounding       = 8.0f;
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 1.0f;  /* 控件描边 → 立体感 */
    s.WindowPadding     = ImVec2(12, 10);
    s.FramePadding      = ImVec2(7, 3);
    s.ItemSpacing       = ImVec2(8, 5);
    s.ItemInnerSpacing  = ImVec2(5, 3);
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 10.0f;
    s.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
    s.AntiAliasedLines  = true;
    s.AntiAliasedFill   = true;
    s.WindowMenuButtonPosition = ImGuiDir_None;
}

}  // namespace theme
