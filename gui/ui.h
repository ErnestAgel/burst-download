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

namespace ui {

/**
 * @brief 初始化 UI 模块（记录 GLFW 窗口指针，供自绘标题栏的最小化/最大化/关闭/拖动使用）
 * @param window GLFW 窗口（无边框窗口，系统标题栏由 UI 自绘）
 */
void Init(GLFWwindow* window);

/**
 * @brief Render the main interface each frame.
 * @param cModel Multi-task queue model (UI reads rows / issues actions).
 * @return TRUE while the window should stay open.
 */
bool Render(CTaskModel& cModel);

}  // namespace ui
