/**
 * @file toggle.h
 * @brief 双向拨动开关组件（§3.2/F1）："文件下载" | "视频下载"，激活侧高亮 + 滑块动画
 *
 * ImGui 无原生 toggle，用 InvisibleButton 命中区域 + ImDrawList 绘制。
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */
#pragma once

#include "imgui.h"

/**
 * @brief 两段式拨动开关
 * @param left 左侧标签（如 i18n::T("mode.file")）
 * @param right 右侧标签（如 i18n::T("mode.video")）
 * @param videoMode 是否处于右侧（视频）模式；点击时取反
 * @param width 控件总宽度
 * @return 本帧是否被点击（videoMode 已被更新）
 */
bool ToggleMode(const char* left, const char* right, bool& videoMode,
                float width = 220.0f);
