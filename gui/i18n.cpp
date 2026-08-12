/**
 * @file i18n.cpp
 * @brief Internationalization implementation.
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

/* ---- String table: key -> {zh, en} (source UTF-8; a missing key returns
 * the key itself with a warning) ---- */
struct Entry {
    const char* key;
    const char* zh;
    const char* en;
};

const Entry kStrings[] = {
    /* Mode switch */
    {"mode.file", "文件下载", "File Download"},
    {"mode.video", "视频下载", "Video Download"},
    {"placeholder.url.file", "输入文件下载地址…", "Enter file download URL..."},
    {"placeholder.url.video", "输入视频页面链接（B站/YouTube）…",
     "Enter video page URL (Bilibili/YouTube)..."},
    {"placeholder.path.file", "保存目录或完整文件路径…",
     "Save folder or full file path..."},
    {"placeholder.path.video", "选择视频保存目录…",
     "Choose a folder to save the video..."},
    /* Form */
    {"label.url", "下载地址", "URL"},
    {"label.path", "保存路径", "Save to"},
    {"button.browse", "浏览…", "Browse..."},
    {"label.threads", "线程数", "Threads"},
    {"hint.threads", "本机可用 %d 线程", "%d threads available on this machine"},
    {"warn.threads.clamped", "线程数超出本机上限，已调整为 %d",
     "Thread count exceeds machine limit, adjusted to %d"},
    {"button.download", "开始下载", "Start Download"},
    {"button.downloading", "下载中…", "Downloading..."},
    {"button.resume", "继续", "Resume"},
    {"button.stop", "停止", "Stop"},
    {"button.cancel", "取消", "Cancel"},
    {"button.add", "添加任务", "Add Task"},
    {"button.pause", "暂停", "Pause"},
    {"button.remove", "移除", "Remove"},
    {"label.tasks", "任务列表", "Tasks"},
    {"label.add_hint", "输入 URL 后添加任务；任务可随时继续添加", "Enter a URL and add a task; more tasks can be added anytime"},
    {"label.clear_finished", "清空已完成", "Clear Finished"},
    {"label.active", "进行中", "Active"},
    {"label.no_tasks", "暂无任务", "No tasks"},
    {"task.pending", "等待中", "Pending"},
    {"dir.title", "选择保存目录", "Select Save Folder"},
    {"dir.select", "选择此目录", "Select This Folder"},
    {"dir.empty", "空目录", "empty"},
    {"button.minimize", "最小化", "Minimize"},
    {"button.maximize", "最大化", "Maximize"},
    {"button.close", "关闭", "Close"},
    /* Progress area */
    {"label.total", "总进度", "Total"},
    {"label.speed", "速度", "Speed"},
    {"label.eta", "剩余时间", "ETA"},
    {"label.thread", "线程", "Thread"},
    {"label.size", "已下载", "Downloaded"},
    {"label.log", "日志", "Log"},
    {"log.copy", "复制日志", "Copy log"},
    /* Settings */
    {"menu.settings", "设置", "Settings"},
    {"menu.language", "语言", "Language"},
    {"menu.about", "关于", "About"},
    /* Language switch entry shows the TARGET language hint: "language" on a
     * Chinese UI, "中文" on an English UI (guides the user to switch). */
    {"menu.lang_hint", "language", "中文"},
    {"lang.zh", "中文", "中文"},
    {"lang.en", "English", "English"},
    /* Stage states */
    {"stage.idle", "空闲", "Idle"},
    {"stage.downloading", "下载中", "Downloading"},
    {"stage.parsing", "解析中", "Parsing"},
    {"stage.video", "下载视频轨", "Downloading video track"},
    {"stage.audio", "下载音频轨", "Downloading audio track"},
    {"stage.merging", "合并中", "Merging"},
    {"stage.done", "完成", "Done"},
    {"stage.canceled", "已取消", "Canceled"},
    {"stage.paused", "已暂停", "Paused"},
    {"stage.error", "错误", "Error"},
    /* Dialogs */
    {"dialog.error.title", "下载失败", "Download Failed"},
    {"dialog.error.copy", "复制", "Copy"},
    {"dialog.error.ok", "确定", "OK"},
    {"dialog.error.delete_partial", "删除半成品", "Delete Partial File"},
    {"dialog.exists.title", "文件已存在", "File Already Exists"},
    {"dialog.exists.prompt", "目标路径已存在，请选择操作：",
     "The target path already exists. Choose an action:"},
    {"dialog.exists.resume", "续传", "Resume"},
    {"dialog.exists.overwrite", "覆盖", "Overwrite"},
    {"dialog.exists.rename", "改名", "Rename"},
    {"dialog.exists.cancel", "取消", "Cancel"},
    {"dialog.done.title", "下载完成", "Download Complete"},
    {"dialog.done.ok", "确定", "OK"},
    /* About dialog */
    {"dialog.about.title", "关于 Burst Download", "About Burst Download"},
    {"dialog.about.version", "版本", "Version"},
    {"dialog.about.platform", "平台", "Platform"},
    {"dialog.about.license", "开源协议", "License"},
    {"dialog.about.ok", "确定", "OK"},
    /* Common */
    {"msg.canceled", "已取消，部分文件保留可续传",
     "Canceled. Partial files kept for resume."},
    {"msg.error.log", "详见 download.log", "See download.log for details"},
    {"window.title", "Burst Download", "Burst Download"},
    /* Error dialog category guidance */
    {"err.guide.init", "检查保存路径与目录权限；确认 URL 可访问后重试。",
     "Check the save path and folder permissions; verify the URL is "
     "reachable and retry."},
    {"err.guide.generic", "检查网络连接与 URL；可稍后重试。详情见 download.log。",
     "Check network connection and URL; retry later. See download.log for "
     "details."},
    {"err.url.invalid", "URL 无效：请输入以 http:// 或 https:// 开头的下载地址。",
     "Invalid URL: enter an address starting with http:// or https://."},
    {"err.thunder.invalid", "迅雷链接解码失败：无法提取有效的 http/https 下载地址。",
     "Thunder link decode failed: no valid http/https URL extracted."},
    {"err.path.empty", "保存路径为空：请填写或浏览选择保存位置。",
     "Save path is empty: enter or browse a destination."},
    {"err.busy", "已有任务在运行，请等待其完成或取消后再试。",
     "A task is already running; wait for it to finish or cancel it first."},
    {"label.status", "状态", "Status"},
    /* Video-mode error guidance (parse failure / merge failure) */
    {"err.guide.parse", "解析失败：请确认视频页面 URL 有效且可访问；"
     "网站改版时可稍后重试，或用 --update-parser 更新解析组件。",
     "Parsing failed: verify the video page URL is valid and reachable; "
     "if the site changed, retry later or run --update-parser."},
    {"err.guide.merge", "音视频轨已下载成功，但自动合并失败。两轨文件已保留，"
     "可用外部工具手动合并（ffmpeg -i <视频轨> -i <音频轨> -c copy <输出>）。",
     "Tracks downloaded, but auto-merge failed. Both track files are kept; "
     "merge manually with an external tool (ffmpeg -i <video> -i <audio> "
     "-c copy <out>)."},
};

Lang g_lang = Lang::En;
std::string g_exe_dir;

/* ---- config.ini persistence (next to the exe, key=value, ASCII) ---- */
const char* kConfigFile = "config.ini";

#ifdef _WIN32
/** UTF-8 -> UTF-16 (Windows paths with CJK characters). */
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
#endif

/** Read the lang value from config.ini; empty when not set. */
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
            /* Strip trailing newline/whitespace. */
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

/** Write config.ini, preserving unknown keys while updating lang=. */
void WriteConfigLang(const std::string& lang) {
    std::string path = g_exe_dir + "/" + kConfigFile;
    std::string content;
    char line[256];
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(Utf8ToWide(path).c_str(), L"r");
#else
    f = fopen(path.c_str(), "r");
#endif
    if (f != nullptr) {
        while (fgets(line, sizeof(line), f) != nullptr) {
            content += line;
        }
        fclose(f);
    }

    /* Replace the lang= line, or append it; keep all other keys. */
    const std::string new_line = "lang=" + lang + "\n";
    const size_t pos = content.find("lang=");
    if (pos != std::string::npos) {
        const size_t end = content.find('\n', pos);
        if (end == std::string::npos) {
            content = content.substr(0, pos) + new_line;
        } else {
            content = content.substr(0, pos) + new_line +
                      content.substr(end + 1);
        }
    } else {
        content += new_line;
    }

    /* Atomic replace: write a temp file then move it over the target, so a
     * crash cannot leave a torn config file (issue R11). */
    const std::string tmp = path + ".tmp";
#ifdef _WIN32
    f = _wfopen(Utf8ToWide(tmp).c_str(), L"w");
#else
    f = fopen(tmp.c_str(), "w");
#endif
    if (f == nullptr) {
        return;
    }
    fputs(content.c_str(), f);
    fclose(f);
#ifdef _WIN32
    MoveFileExW(Utf8ToWide(tmp).c_str(), Utf8ToWide(path).c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    std::rename(tmp.c_str(), path.c_str());
#endif
}

/** System language detection: Windows UI language / Linux LANG zh prefix;
 *  falls back to En when detection fails. */
Lang DetectSystemLang() {
#ifdef _WIN32
    const LANGID lid = GetUserDefaultUILanguage();
    /* Primary language ID 0x04 = Simplified Chinese (0x0804/0x0404 etc.). */
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
    const std::string cfg = ReadConfigLang();
    if (cfg == "zh") {
        g_lang = Lang::Zh;
    } else if (cfg == "en") {
        g_lang = Lang::En;
    } else {
        /* Follow the system only when the user never switched manually. */
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
    /* Missing key: return the key itself and warn. */
    printf("[i18n] missing key: %s\n", key);
    return key;
}

const char* LangName(Lang lang) {
    return lang == Lang::Zh ? "zh" : "en";
}

}  // namespace i18n
