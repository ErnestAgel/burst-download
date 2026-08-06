/**
 * @file i18n.h
 * @brief 国际化：中英双语字符串表 + 系统语言检测 + 手动切换 + config.ini 持久化（§3.3）
 *
 * 约定：
 * - 全部 UI 文本/弹窗指引/日志前缀走 T(key)，源码 UTF-8；
 * - curl 原生错误串保留英文原文（外文错误不翻译，避免误导）；
 * - 缺键返回 key 本身并打印告警（R19）；
 * - 一套 Noto Sans SC 子集字体渲染中英两界面，切换语言不换字体。
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */
#pragma once

#include <string>

namespace i18n {

enum class Lang { Zh, En };

/**
 * @brief 初始化：确定语言（config.ini 持久化优先，否则跟随系统）并加载
 * @param exe_dir 可执行文件所在目录（config.ini 与之同目录）
 * @return 实际生效的语言
 */
Lang Init(const std::string& exe_dir);

/**
 * @brief 手动切换语言（设置菜单），即时生效并持久化到 config.ini
 */
void SetLang(Lang lang);

/** @brief 当前语言 */
Lang GetLang();

/**
 * @brief 查表返回当前语言字符串；缺键返回 key 本身并告警（R19）
 */
const char* T(const char* key);

/** @brief 当前语言短名（"zh"/"en"，日志/调试用） */
const char* LangName(Lang lang);

}  // namespace i18n
