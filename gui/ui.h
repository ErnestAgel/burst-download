/**
 * @file ui.h
 * @brief 主界面渲染（布局/控件/进度表/日志），见 gui-design.md §4
 *
 * 线程模型（R2）：本模块只在 UI 主线程（ImGui 渲染循环）调用；
 * 工作线程数据一律通过 DownloadWorker::GetSnapshot 读取（mutex 保护）。
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */
#pragma once

#include "worker.h"

namespace ui {

/**
 * @brief 每帧渲染主界面（含表单、进度、日志、弹窗、设置菜单）
 * @param worker 后台下载工作线程（UI 只读快照 / 触发 Start/Cancel）
 * @return 是否请求退出（例如单实例检测失败等，main_gui 据此关闭窗口）
 */
bool Render(DownloadWorker& worker);

}  // namespace ui
