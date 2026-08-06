/**
 * @file dialogs.h
 * @brief 模态弹窗（§8.3/F11/F12/F13）：错误指引 / 文件已存在四选一 / 完成提示
 *
 * 全部文案走 i18n::T()；curl 原生错误串保留英文原文（§3.3）。
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */
#pragma once

#include <string>

namespace dialogs {

/** @brief 文件已存在弹窗的用户选择（F11） */
enum class ExistsChoice {
    None = 0,      /**< 未选择（弹窗仍打开） */
    Resume,        /**< 续传：断点续传（Ccurl 自动检测本地文件） */
    Overwrite,     /**< 覆盖：删除本地文件重新下载 */
    Rename,        /**< 改名：追加时间戳避让（调用方需用新路径重启任务） */
    Cancel,        /**< 取消：不下载 */
};

/**
 * @brief 错误弹窗（F12，模态）：标题 + 错误信息 + 分类指引 + "确定"
 * @param title 标题（i18n key 已翻译文本）
 * @param message 错误信息
 * @param guide 分类指引文本（§8.3 表格）
 * @param open 弹窗开关（打开时置 true；点"确定"后置 false）
 */
void ShowError(const std::string& title, const std::string& message,
               const std::string& guide, bool& open);

/**
 * @brief 文件已存在四选一弹窗（F11，模态）
 * @param path 目标路径
 * @param open 弹窗开关（点任一按钮后置 false）
 * @return 用户选择（未点击时 ExistsChoice::None）
 */
ExistsChoice ShowFileExists(const std::string& path, bool& open);

/**
 * @brief 完成提示弹窗（F13，模态）：下载完成 + "确定"
 * @param path 输出文件路径
 * @param open 弹窗开关（点"确定"后置 false）
 */
void ShowDone(const std::string& path, bool& open);

}  // namespace dialogs
