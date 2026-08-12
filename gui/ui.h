/**
 * @file ui.h
 * @brief 主界面渲染（布局/控件/进度表/日志），见 gui-design.md §4
 *
 * 线程模型（R2）：本模块只在 UI 主线程（ImGui 渲染循环）调用；
 * 工作线程数据一律通过 CDownloadWorker::GetSnapshot 读取（mutex 保护）。
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */
#pragma once

#include "worker.h"

struct GLFWwindow;  /* 前向声明，避免在 ui.h 引入 GLFW 头 */

namespace ui {

/**
 * @brief 初始化 UI 模块（记录 GLFW 窗口指针，供自绘标题栏的最小化/最大化/关闭/拖动使用）
 * @param window GLFW 窗口（无边框窗口，系统标题栏由 UI 自绘）
 */
void Init(GLFWwindow* window);

/**
 * @brief 每帧渲染主界面（含自绘标题栏、表单、进度、日志、弹窗、设置菜单）
 * @param worker 后台下载工作线程（UI 只读快照 / 触发 Start/Cancel）
 * @return 是否请求退出（例如单实例检测失败等，main_gui 据此关闭窗口）
 */
bool Render(CDownloadWorker& worker);

}  // namespace ui
