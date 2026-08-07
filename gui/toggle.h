/**
 * @file toggle.h
 * @brief 模式切换 Switch 开关组件（§3.2/F1）："文件下载" | "视频下载"
 *
 * iOS 风格拨杆开关：圆形滑块 + 圆角底槽，滑块位置与底槽颜色表达状态
 * （OFF=左侧/灰色=文件模式，ON=右侧/主题色=视频模式），点击滑块切换，
 * 滑块滑动动画（~180ms）+ 两端标签文字激活侧高亮。
 *
 * 实现：InvisibleButton（标准命中/激活处理）+ ImDrawList 绘制，
 * 不依赖 imgui_internal 手写 ButtonBehavior（旧实现状态管理易失效）。
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */
#pragma once

#include "imgui.h"

/**
 * @brief 模式切换 Switch 开关
 * @param left 左侧模式名（如 i18n::T("mode.file")）
 * @param right 右侧模式名（如 i18n::T("mode.video")）
 * @param videoMode true=右侧模式（视频），false=左侧模式（文件）；点击取反
 * @param width 保留兼容参数（自适应内容宽度，忽略）
 * @return 本帧是否被点击（videoMode 已被更新）
 */
bool ToggleMode(const char* left, const char* right, bool& videoMode,
                float width = 220.0f);
