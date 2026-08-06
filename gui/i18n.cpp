/**
 * @file i18n.cpp
 * @brief 国际化实现（§3.3）
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */

#include "i18n.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace i18n {

namespace {

/* ---- 字符串表：key → {zh, en}（源码 UTF-8；缺键返回 key 并告警，R19） ---- */
struct Entry {
    const char* key;
    const char* zh;
    const char* en;
};

const Entry kStrings[] = {
    /* 模式开关 */
    {"mode.file", "文件下载", "File Download"},
    {"mode.video", "视频下载", "Video Download"},
    {"placeholder.url.file", "输入文件下载地址…", "Enter file download URL..."},
    {"placeholder.url.video", "输入视频页面链接（B站/YouTube）…",
     "Enter video page URL (Bilibili/YouTube)..."},
    /* 表单 */
    {"label.url", "下载地址", "URL"},
    {"label.path", "保存路径", "Save to"},
    {"button.browse", "浏览…", "Browse..."},
    {"label.threads", "线程数", "Threads"},
    {"hint.threads", "本机可用 %d 线程", "%d threads available on this machine"},
    {"warn.threads.clamped", "线程数超出本机上限，已调整为 %d",
     "Thread count exceeds machine limit, adjusted to %d"},
    {"button.download", "开始下载", "Start Download"},
    {"button.downloading", "下载中…", "Downloading..."},
    {"button.cancel", "取消", "Cancel"},
    {"button.minimize", "最小化", "Minimize"},
    {"button.maximize", "最大化", "Maximize"},
    {"button.close", "关闭", "Close"},
    /* 进度区 */
    {"label.total", "总进度", "Total"},
    {"label.speed", "速度", "Speed"},
    {"label.eta", "剩余时间", "ETA"},
    {"label.thread", "分片", "Part"},
    {"label.size", "已下载", "Downloaded"},
    {"label.log", "日志", "Log"},
    /* 设置 */
    {"menu.settings", "设置", "Settings"},
    {"menu.language", "语言", "Language"},
    {"lang.zh", "中文", "中文"},
    {"lang.en", "English", "English"},
    /* 阶段状态（F8） */
    {"stage.idle", "空闲", "Idle"},
    {"stage.downloading", "下载中", "Downloading"},
    {"stage.parsing", "解析中", "Parsing"},
    {"stage.video", "下载视频轨", "Downloading video track"},
    {"stage.audio", "下载音频轨", "Downloading audio track"},
    {"stage.merging", "合并中", "Merging"},
    {"stage.done", "完成", "Done"},
    {"stage.canceled", "已取消", "Canceled"},
    {"stage.error", "错误", "Error"},
    /* 弹窗（F11/F12/F13） */
    {"dialog.error.title", "下载失败", "Download Failed"},
    {"dialog.error.ok", "确定", "OK"},
    {"dialog.exists.title", "文件已存在", "File Already Exists"},
    {"dialog.exists.prompt", "目标路径已存在，请选择操作：",
     "The target path already exists. Choose an action:"},
    {"dialog.exists.resume", "续传", "Resume"},
    {"dialog.exists.overwrite", "覆盖", "Overwrite"},
    {"dialog.exists.rename", "改名", "Rename"},
    {"dialog.exists.cancel", "取消", "Cancel"},
    {"dialog.done.title", "下载完成", "Download Complete"},
    {"dialog.done.ok", "确定", "OK"},
    /* 通用 */
    {"msg.canceled", "已取消，部分文件保留可续传",
     "Canceled. Partial files kept for resume."},
    {"msg.error.log", "详见 download.log", "See download.log for details"},
    {"window.title", "curlbolt 下载器", "curlbolt Downloader"},
    /* 错误弹窗分类指引（§8.3） */
    {"err.guide.init", "检查保存路径与目录权限；确认 URL 可访问后重试。",
     "Check the save path and folder permissions; verify the URL is reachable and retry."},
    {"err.guide.generic", "检查网络连接与 URL；可稍后重试。详情见 download.log。",
     "Check network connection and URL; retry later. See download.log for details."},
    {"err.url.invalid", "URL 无效：请输入以 http:// 或 https:// 开头的下载地址。",
     "Invalid URL: enter an address starting with http:// or https://."},
    {"err.video.phase2", "视频模式将在后续版本支持，当前请使用文件下载模式。",
     "Video mode is coming in a later version; use File Download mode for now."},
    {"err.path.empty", "保存路径为空：请填写或浏览选择保存位置。",
     "Save path is empty: enter or browse a destination."},
    {"err.busy", "已有任务在运行，请等待其完成或取消后再试。",
     "A task is already running; wait for it to finish or cancel it first."},
    {"label.status", "状态", "Status"},
};

Lang g_lang = Lang::En;
std::string g_exe_dir;

/* ---- config.ini 持久化（与 exe 同目录，格式 key=value，ASCII） ---- */
const char* kConfigFile = "config.ini";

#ifdef _WIN32
/** UTF-8 → UTF-16（Windows 中文路径） */
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
#endif

/** 读取 config.ini 的 lang 值；未设置返回空串 */
std::string ReadConfigLang() {
    std::string path = g_exe_dir + "/" + kConfigFile;
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(Utf8ToWide(path).c_str(), L"r");
#else
    f = fopen(path.c_str(), "r");
#endif
    if (f == nullptr) {
        return "";
    }
    char line[256];
    std::string lang;
    while (fgets(line, sizeof(line), f) != nullptr) {
        if (strncmp(line, "lang=", 5) == 0) {
            lang = std::string(line + 5);
            /* 去掉末尾换行/空白 */
            while (!lang.empty() &&
                   (lang.back() == '\n' || lang.back() == '\r' ||
                    lang.back() == ' ')) {
                lang.pop_back();
            }
            break;
        }
    }
    fclose(f);
    return lang;
}

/** 写 config.ini（保留其他 key 的前提下更新 lang=） */
void WriteConfigLang(const std::string& lang) {
    std::string path = g_exe_dir + "/" + kConfigFile;
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(Utf8ToWide(path).c_str(), L"w");
#else
    f = fopen(path.c_str(), "w");
#endif
    if (f == nullptr) {
        return;
    }
    fprintf(f, "lang=%s\n", lang.c_str());
    fclose(f);
}

/** 系统语言检测：Windows UI 语言 / Linux LANG 前缀 zh；检测失败默认 En */
Lang DetectSystemLang() {
#ifdef _WIN32
    LANGID lid = GetUserDefaultUILanguage();
    /* 主语言 ID 0x04 = 简体中文（0x0804/0x0404 等） */
    if ((lid & 0xFF) == 0x04) {
        return Lang::Zh;
    }
    return Lang::En;
#else
    const char* lang = getenv("LANG");
    if (lang != nullptr && strncmp(lang, "zh", 2) == 0) {
        return Lang::Zh;
    }
    return Lang::En;
#endif
}

}  // namespace

Lang Init(const std::string& exe_dir) {
    g_exe_dir = exe_dir;
    std::string cfg = ReadConfigLang();
    if (cfg == "zh") {
        g_lang = Lang::Zh;
    } else if (cfg == "en") {
        g_lang = Lang::En;
    } else {
        /* 未设置过才跟随系统（R20：手动切换 + 持久化兜底） */
        g_lang = DetectSystemLang();
    }
    return g_lang;
}

void SetLang(Lang lang) {
    g_lang = lang;
    WriteConfigLang(lang == Lang::Zh ? "zh" : "en");
}

Lang GetLang() {
    return g_lang;
}

const char* T(const char* key) {
    for (const auto& e : kStrings) {
        if (strcmp(e.key, key) == 0) {
            return g_lang == Lang::Zh ? e.zh : e.en;
        }
    }
    /* 缺键：返回 key 本身并告警（R19） */
    printf("[i18n] missing key: %s\n", key);
    return key;
}

const char* LangName(Lang lang) {
    return lang == Lang::Zh ? "zh" : "en";
}

}  // namespace i18n
