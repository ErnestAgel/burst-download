/**
 * @file ui.h
 * @brief Main GUI rendering (form, task list, progress, logs), P5-4.
 *
 * Thread rules (R2): this module runs only on the UI thread (ImGui loop);
 * task data is read through CTaskModel (mutex protected).
 */
#pragma once

#include "taskmodel.h"

struct GLFWwindow;  /* 前向声明，避免在 ui.h 引入 GLFW 头 */
struct ImFont;      /* 前向声明，字体指针由 main_gui 注入 */

namespace ui {

/**
 * @brief 初始化 UI 模块（记录 GLFW 窗口指针，供自绘标题栏的最小化/最大化/关闭/拖动使用）
 * @param window GLFW 窗口（无边框窗口，系统标题栏由 UI 自绘）
 */
void Init(GLFWwindow* window);

/**
 * @brief 注入主/次级字体指针（由 main_gui.cpp 加载后调用）
 * @param pFontMain 主字号（16px，输入框/按钮/文件名）
 * @param pFontSmall 次级字号（12px，标签/徽标/状态栏/工具卡）
 */
void SetFonts(ImFont* pFontMain, ImFont* pFontSmall);

/**
 * @brief Render the main interface each frame.
 * @param cModel Multi-task queue model (UI reads rows / issues actions).
 * @return TRUE while the window should stay open.
 */
bool Render(CTaskModel& cModel);

}  // namespace ui
