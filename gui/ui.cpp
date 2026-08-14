/**
 * @file ui.cpp
 * @brief Main UI rendering implementation (see ui.h).
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */

#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

#include "dialogs.h"
#include "i18n.h"
#include "imgui.h"
#include "Ccurl.h"  /* MaxThread */
#include "pathutil.h"
#include "version.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>  /* IFileDialog folder picker */
#else
#include <sys/wait.h>
#include <unistd.h>    /* fork/execlp: open URLs via xdg-open */
#include <sys/stat.h>
#include <filesystem>  /* Linux directory browser initial directory */
#include "dirbrowser.h"
#endif

namespace ui {

namespace {

/* Custom title bar height (borderless window, replaces the system title
 * bar). */
#ifdef _WIN32
const float kTitleBarH = 38.0f; /* Windows custom title bar height (web: py-2.5) */
#else
const float kTitleBarH = 0.0f;  /* Linux uses the system title bar; the
                                 * content area starts at the window top */
#endif

/* GLFW window pointer (injected by ui::Init; used by the title bar buttons,
 * drag and resize). */
GLFWwindow* g_window = nullptr;

/* Fonts injected by main_gui.cpp: main 14px / small 11px (web density). */
ImFont* g_pFontMain = nullptr;
ImFont* g_pFontSmall = nullptr;

/* ---- Form state (UI thread only; worker threads never touch) ---- */
/* Machine thread cap: adapts 4~8 to the CPU core count (see
 * BurstMaxThreads). */
const int kHardwareMax = BurstMaxThreads();

char g_url[2048] = {0};
char g_path[2048] = {0};
int g_threads = BurstDefaultThreads();   /* default = sensible 2~4 */

/* Forward declarations. */
void AddTaskFromForm(CTaskModel& cModel);
void RenderTaskList(CTaskModel& cModel,
                    std::vector<CTaskModel::TTaskRow>& vecRowsOut);
void RenderLog(const std::vector<std::string>& log, float fHeight);
void RenderStatusBar(const std::vector<CTaskModel::TTaskRow>& vecRows,
                     u32 dwMaxSlots);
void RenderLogSection(CTaskModel& cModel);
void PushSmallFont();
void PopSmallFont();
std::string FormatSpeed(double dBytesPerSec);
std::string FormatSizeBytes(long long llBytes);

/* Popup state */
bool g_error_open = false;
std::string g_error_title, g_error_msg, g_error_guide;
std::string g_error_partial_path;
bool g_error_delete_requested = false;
bool g_about_open = false;

/* Selected task row (detail panel). */
u64 g_selected_model_id = 0;

#ifndef _WIN32
/* Linux built-in directory browser state (Windows uses the native
 * IFileDialog, see RenderForm). */
bool g_dirbrowse_open = false;
std::string g_dirbrowse_dir;
#endif

/* Log auto-scroll */
bool g_log_autoscroll = true;

/* This frame's reserved log-section height (computed by RenderTaskList so
 * the status bar always stays visible even with the log open). */
float g_log_h = 0.0f;

/* URL input focus request (example chips) and task-log expand state. */
bool g_focus_url_input = false;
bool g_log_open = false;
float g_task_row_h = 0.0f;  /* measured row height (hover bg); 0 = not yet */

/** Push the 11px secondary font when available. */
void PushSmallFont() {
    if (g_pFontSmall != nullptr) {
        ImGui::PushFont(g_pFontSmall);
    }
}

/** Pop the secondary font (paired with PushSmallFont). */
void PopSmallFont() {
    if (g_pFontSmall != nullptr) {
        ImGui::PopFont();
    }
}

/* ---- Widgets per UI spec appendix E.4 ---- */

/** One thread chunk view (derived from the task model snapshot). */
struct ThreadView {
    float     fProgress;   /* 0..1 within this chunk */
    float     fSpeedMBs;   /* MB/s */
    long long llRangeStart; /* chunk start byte */
    long long llRangeEnd;   /* chunk end byte (exclusive) */
};

/** Convert a task row into the flat chunk view array. */
std::vector<ThreadView> BuildThreadViews(const CTaskModel::TTaskRow& tRow) {
    int nSegs = (int)tRow.vecThreads.size();
    if (nSegs <= 0) {
        nSegs = (tRow.nThreads > 0) ? tRow.nThreads : 1;
    }
    std::vector<ThreadView> vecViews(static_cast<size_t>(nSegs));
    for (int i = 0; i < nSegs; ++i) {
        ThreadView& v = vecViews[static_cast<size_t>(i)];
        if (i < (int)tRow.vecThreads.size()) {
            const ThreadProgress& t = tRow.vecThreads[i];
            const double dSegTotal = (double)(t.total - t.file_start);
            v.fProgress = (dSegTotal > 0.0)
                              ? (float)((double)(t.downloaded - t.file_start) /
                                        dSegTotal)
                              : (float)(tRow.dPercent / 100.0);
            v.fSpeedMBs = (float)(t.speed / (1024.0 * 1024.0));
            v.llRangeStart = t.file_start;
            v.llRangeEnd = t.total;
        } else {
            v.fProgress = (float)(tRow.dPercent / 100.0);
            v.fSpeedMBs = 0.0f;
            v.llRangeStart = 0;
            v.llRangeEnd = (tRow.llFileTotal > 0)
                               ? tRow.llFileTotal / nSegs
                               : 0;
        }
        if (v.fProgress < 0.0f) v.fProgress = 0.0f;
        if (v.fProgress > 1.0f) v.fProgress = 1.0f;
    }
    return vecViews;
}

/**
 * @brief Segmented progress bar: nThreads chunks side by side; hovering a
 *        chunk shows its thread tooltip (top-level window, never clipped).
 */
void SegmentProgressBar(int nThreads, const ThreadView* pThreads,
                        float fWidth, float fHeight) {
    if (nThreads <= 0 || pThreads == nullptr) {
        return;
    }
    ImDrawList* pDraw = ImGui::GetWindowDrawList();
    const ImVec2 vPos = ImGui::GetCursorScreenPos();
    const float fGap = 3.0f;
    const float fSegWidth = (fWidth - fGap * (nThreads - 1)) / nThreads;
    const ImU32 uTrack = IM_COL32(0x3B, 0x40, 0x50, 255);
    const ImU32 uFillTop = IM_COL32(0xA8, 0xD5, 0x84, 255);
    const ImU32 uFillBot = IM_COL32(0x6F, 0xA3, 0x4C, 255);
    const ImU32 uHilite = IM_COL32(255, 255, 255, 89);

    for (int i = 0; i < nThreads; ++i) {
        const ImVec2 vMin(vPos.x + i * (fSegWidth + fGap), vPos.y);
        const ImVec2 vMax(vMin.x + fSegWidth, vPos.y + fHeight);
        const float fProg =
            std::clamp(pThreads[i].fProgress, 0.0f, 1.0f);
        const float fFillX = vMin.x + fSegWidth * fProg;
        pDraw->AddRectFilled(vMin, vMax, uTrack, 3.0f);
        if (fFillX > vMin.x + 0.5f) {
            const ImVec2 vFillMax(fFillX, vMax.y);
            /* Vertical gradient #A8D584 -> #6FA34C (web inset style). */
            pDraw->AddRectFilledMultiColor(
                vMin, vFillMax, uFillTop, uFillTop, uFillBot, uFillBot);
            pDraw->AddRectFilled(ImVec2(vMin.x, vMin.y),
                                 ImVec2(fFillX, vMin.y + 1.0f), uHilite);
        }
    }

    ImGui::InvisibleButton("##segs", ImVec2(fWidth, fHeight));
    if (ImGui::IsItemHovered()) {
        for (int i = 0; i < nThreads; ++i) {
            const ImVec2 vMin(vPos.x + i * (fSegWidth + fGap), vPos.y);
            const ImVec2 vMax(vMin.x + fSegWidth, vPos.y + fHeight);
            if (!ImGui::IsMouseHoveringRect(vMin, vMax)) {
                continue;
            }
            const double dChunkMB =
                (double)(pThreads[i].llRangeEnd - pThreads[i].llRangeStart) /
                (1024.0 * 1024.0);
            const int nStart =
                (int)((double)i / nThreads * 100.0);
            const int nEnd =
                (int)((double)(i + 1) / nThreads * 100.0);
            char buf[64];
            snprintf(buf, sizeof(buf), "%s %d · %s",
                     i18n::T("label.thread"), i + 1,
                     FormatSizeBytes((long long)(dChunkMB * 1024.0 * 1024.0))
                         .c_str());
            ImGui::PushStyleColor(ImGuiCol_PopupBg,
                                  IM_COL32(0x18, 0x1B, 0x21, 242));
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  IM_COL32(0xD4, 0xD4, 0xD8, 255));
            PushSmallFont();
            ImGui::SetItemTooltip(
                "%s\n%s %d - %d%%\n%s %.0f%%\n%s %s", buf,
                i18n::T("chunk"), nStart, nEnd, i18n::T("progress"),
                pThreads[i].fProgress * 100.0f, i18n::T("speed"),
                FormatSpeed(pThreads[i].fSpeedMBs * 1024.0 * 1024.0).c_str());
            PopSmallFont();
            ImGui::PopStyleColor(2);
            break;
        }
    }
}

/** @brief Total progress bar: capsule track + horizontal gradient fill. */
void TotalProgressBar(float fProgress, float fWidth, float fHeight) {
    ImDrawList* pDraw = ImGui::GetWindowDrawList();
    const ImVec2 vMin = ImGui::GetCursorScreenPos();
    const ImVec2 vMax(vMin.x + fWidth, vMin.y + fHeight);
    const float fRadius = fHeight * 0.5f;
    pDraw->AddRectFilled(vMin, vMax, IM_COL32(0x3B, 0x40, 0x50, 255),
                         fRadius);
    const float fProg = std::clamp(fProgress, 0.0f, 1.0f);
    if (fProg > 0.0f) {
        const ImVec2 vFillMax(vMin.x + fWidth * fProg, vMax.y);
        pDraw->AddRectFilledMultiColor(
            vMin, vFillMax, IM_COL32(0x3B, 0x82, 0xF6, 255),
            IM_COL32(0x38, 0xBD, 0xF8, 255),
            IM_COL32(0x38, 0xBD, 0xF8, 255),
            IM_COL32(0x3B, 0x82, 0xF6, 255));
        pDraw->AddRectFilled(ImVec2(vMin.x, vMin.y),
                             ImVec2(vFillMax.x, vMin.y + 1.0f),
                             IM_COL32(255, 255, 255, 60));
    }
    ImGui::Dummy(ImVec2(fWidth, fHeight));
}

#ifdef _WIN32
/** UTF-16 -> UTF-8. */
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0,
                                NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL,
                        NULL);
    return s;
}

/** UTF-8 -> UTF-16. */
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
#endif

/** URL scheme pre-check (http/https). */
bool UrlSchemeOk(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

/** Base64 decode (used for thunder links; no third-party dependency).
 * Algorithm: every 4 base64 chars -> 3 bytes (shifted in groups; a trailing
 * group of 2 chars -> 1 byte, 3 chars -> 2 bytes). */
std::string Base64Decode(const std::string& in) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int buf[4] = {0, 0, 0, 0};
    int n = 0;
    for (char c : in) {
        if (c == '=') {
            break;  /* padding: stop */
        }
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        }
        const char* p = strchr(tbl, c);
        if (p == nullptr) {
            continue;
        }
        buf[n++] = (int)(p - tbl);
        if (n == 4) {
            out.push_back((char)((buf[0] << 2) | (buf[1] >> 4)));
            out.push_back((char)(((buf[1] & 0xF) << 4) | (buf[2] >> 2)));
            out.push_back((char)(((buf[2] & 0x3) << 6) | buf[3]));
            n = 0;
        }
    }
    /* Trailing group < 4 chars (2 chars -> 1 byte; 3 chars -> 2 bytes). */
    if (n == 2) {
        out.push_back((char)((buf[0] << 2) | (buf[1] >> 4)));
    } else if (n == 3) {
        out.push_back((char)((buf[0] << 2) | (buf[1] >> 4)));
        out.push_back((char)(((buf[1] & 0xF) << 4) | (buf[2] >> 2)));
    }
    return out;
}

/**
 * @brief Decode a thunder:// link: thunder:// + Base64("AA" + real URL + "ZZ").
 * @param url Link to decode.
 * @return The decoded real URL; non-thunder:// inputs are unchanged.
 */
std::string ThunderDecode(const std::string& url) {
    const char* prefix = "thunder://";
    if (url.rfind(prefix, 0) != 0) {
        return url;
    }
    std::string dec = Base64Decode(url.substr(strlen(prefix)));
    /* Strip the leading "AA" and the trailing "ZZ". */
    if (dec.size() >= 4) {
        dec = dec.substr(2, dec.size() - 4);
    }
    return dec;
}

/** Whether the target path exists (F11 trigger). */
bool PathExists(const std::string& path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(Utf8ToWide(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
#else
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
#endif
}

/** Delete a local file (overwrite choice). */
void RemoveFile(const std::string& path) {
#ifdef _WIN32
    DeleteFileW(Utf8ToWide(path).c_str());
#else
    remove(path.c_str());
#endif
}

/** Current timestamp (YYYYMMDD_HHMMSS, used for renaming). */
std::string CurrentTimeStamp() {
    char buf[32];
    time_t t = time(nullptr);
    struct tm* tm_now = localtime(&t);
    if (tm_now != nullptr) {
        strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", tm_now);
    } else {
        snprintf(buf, sizeof(buf), "%ld", (long)t);
    }
    return std::string(buf);
}

/** Derive a file name from a URL (query/fragment/trailing slashes stripped)
 *  and sanitize it for safe local storage (issue S3). */
std::string UrlFileName(const std::string& url) {
    std::string u = url;
    size_t q = u.find_first_of("?#");
    if (q != std::string::npos) {
        u = u.substr(0, q);
    }
    while (!u.empty() && (u.back() == '/' || u.back() == '\\')) {
        u.pop_back();
    }
    size_t slash = u.find_last_of("/\\");
    std::string name = (slash != std::string::npos) ? u.substr(slash + 1) : u;
    return SanitizeFileName(name);
}

/**
 * @brief URL mode auto-detection (UI spec 2.1): known video hosts take
 *        priority, then path features, then video file extensions.
 * @param strUrl Input URL (http/https or already decoded real URL).
 * @return TRUE when the URL should be handled as a video download.
 */
bool IsVideoUrl(const std::string& strUrl) {
    std::string u = strUrl;
    for (char& c : u) {
        if ((c >= 'A') && (c <= 'Z')) {
            c += 32;  /* lowercase for matching */
        }
    }
    /* Split scheme / host / path. */
    size_t scheme = u.find("://");
    size_t hostBegin = (scheme != std::string::npos) ? scheme + 3 : 0;
    size_t pathBegin = u.find('/', hostBegin);
    if (pathBegin == std::string::npos) {
        pathBegin = u.size();
    }
    std::string host = u.substr(hostBegin, pathBegin - hostBegin);
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        host = host.substr(0, colon);
    }
    /* 1. Known video domains (host or any subdomain). */
    const char* kVideoHosts[] = {
        "bilibili.com",   "youtube.com",   "youtu.be",
        "vimeo.com",      "dailymotion.com", "twitch.tv",
        "tiktok.com",     "douyin.com",    "iqiyi.com",
        "youku.com",      "v.qq.com",      "sohu.com",
        "mgtv.com",
    };
    for (const char* pszHost : kVideoHosts) {
        if ((host == pszHost) ||
            (host.size() > strlen(pszHost) &&
             host.compare(host.size() - strlen(pszHost) - 1,
                          strlen(pszHost) + 1, std::string(".") + pszHost) ==
                 0)) {
            return true;
        }
    }
    /* 2. Path features: /video/ segment or /watch|/play|/bangumi prefix. */
    const std::string path = u.substr(pathBegin);
    if (path.find("/video/") != std::string::npos) {
        return true;
    }
    if ((path.rfind("/watch", 0) == 0) || (path.rfind("/play", 0) == 0) ||
        (path.rfind("/bangumi", 0) == 0)) {
        return true;
    }
    /* 3. Direct video file extensions. */
    const char* kVideoExts[] = {
        ".mp4", ".mkv", ".webm", ".mov", ".flv",
        ".avi", ".ts",  ".m3u8", ".m4v",
    };
    for (const char* pszExt : kVideoExts) {
        if (path.size() >= strlen(pszExt) &&
            path.compare(path.size() - strlen(pszExt), strlen(pszExt),
                         pszExt) == 0) {
            return true;
        }
    }
    return false;
}

/** Base64 encode (used to build the thunder:// example chip; no
 *  third-party dependency, mirrors Base64Decode above). */
std::string Base64Encode(const std::string& in) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const u32 dw0 = (u8)in[i];
        const u32 dw1 = (i + 1 < in.size()) ? (u8)in[i + 1] : 0u;
        const u32 dw2 = (i + 2 < in.size()) ? (u8)in[i + 2] : 0u;
        const u32 dwTri = (dw0 << 16) | (dw1 << 8) | dw2;
        out.push_back(tbl[(dwTri >> 18) & 0x3Fu]);
        out.push_back(tbl[(dwTri >> 12) & 0x3Fu]);
        out.push_back((i + 1 < in.size()) ? tbl[(dwTri >> 6) & 0x3Fu] : '=');
        out.push_back((i + 2 < in.size()) ? tbl[dwTri & 0x3Fu] : '=');
    }
    return out;
}

/** Format a byte rate (B/s) as "8.3 MB/s" / "512 KB/s". */
std::string FormatSpeed(double dBytesPerSec) {
    char buf[48];
    const double dMb = dBytesPerSec / (1024.0 * 1024.0);
    if (dMb >= 1.0) {
        snprintf(buf, sizeof(buf), "%.1f MB/s", dMb);
    } else {
        snprintf(buf, sizeof(buf), "%.0f KB/s", dMb * 1024.0);
    }
    return std::string(buf);
}

/** Format a byte count as "512 MB" / "1.5 GB". */
std::string FormatSizeBytes(long long llBytes) {
    char buf[48];
    const double dMb = (double)llBytes / (1024.0 * 1024.0);
    if (dMb >= 1024.0) {
        snprintf(buf, sizeof(buf), "%.1f GB", dMb / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%.0f MB", dMb);
    }
    return std::string(buf);
}

/** Open a URL in the system default browser (UI thread only). */
void OpenUrl(const std::string& strUrl) {
#ifdef _WIN32
    ShellExecuteW(NULL, L"open", Utf8ToWide(strUrl).c_str(), NULL, NULL,
                  SW_SHOWNORMAL);
#else
    pid_t pid = fork();
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", strUrl.c_str(), (char*)nullptr);
        _exit(1);
    }
#endif
}

/** Whether the path should be treated as a directory: it exists and is a
 *  directory, or it ends with a separator. */
bool IsDirectoryPath(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    char last = path.back();
    if (last == '/' || last == '\\') {
        return true;
    }
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(Utf8ToWide(path).c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
#else
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
#endif
    return false;
}

/** Join a directory and a file name (adds the platform separator). */
std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) {
        return name;
    }
    char last = dir.back();
    if (last == '/' || last == '\\') {
        return dir + name;
    }
#ifdef _WIN32
    return dir + "\\" + name;
#else
    return dir + "/" + name;
#endif
}

/** Timestamp rename: file.zip -> file_ts.zip (F11 rename semantics, same as
 *  the CLI). */
std::string StampName(const std::string& path) {
    std::string base = path;
    std::string ts = CurrentTimeStamp();
    size_t dot = base.find_last_of('.');
    size_t slash = base.find_last_of("/\\");
    if (dot != std::string::npos &&
        (slash == std::string::npos || dot > slash)) {
        return base.substr(0, dot) + "_" + ts + base.substr(dot);
    }
    return base + "_" + ts;
}

/** Trigger the error popup (F12). */
void ShowErrorPopup(const std::string& title, const std::string& msg,
                    const std::string& guide = "",
                    const std::string& partial_path = "") {
    g_error_title = title;
    g_error_msg = msg;
    g_error_guide = guide;
    g_error_partial_path = partial_path;
    g_error_delete_requested = false;
    g_error_open = true;
}

/** Classify an error by its message and return the matching guide. */
std::string ErrorGuide(const std::string& err) {
    if (err.find("parsing failed") != std::string::npos) {
        return i18n::T("err.guide.parse");
    }
    if (err.find("merge failed") != std::string::npos) {
        return i18n::T("err.guide.merge");
    }
    if (err.find("init failed") != std::string::npos) {
        return i18n::T("err.guide.init");
    }
    return i18n::T("err.guide.generic");
}

/* ---- Small UI widgets (pills / text buttons / chips) ---- */

/** Rounded detection chip ("识别: 文件 / 识别: 视频"), overlaid inside the
 *  right edge of the URL box (spec 2.1, web mockup style). */
void RenderChip(const char* pszText, BOOL32 bVideo, const ImVec2& pos) {
    PushSmallFont();
    const ImVec2 sz = ImGui::CalcTextSize(pszText);
    const float fH = sz.y + 6.0f;
    const float fW = sz.x + 16.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 colText = (bVideo != FALSE)
                              ? IM_COL32(0xD8, 0xB4, 0xFE, 255)
                              : IM_COL32(0x7D, 0xD3, 0xFC, 255);
    const ImU32 colBg = (bVideo != FALSE)
                            ? IM_COL32(0xA8, 0x5B, 0xF7, 38)
                            : IM_COL32(0x0E, 0xA5, 0xE9, 38);
    dl->AddRectFilled(pos, ImVec2(pos.x + fW, pos.y + fH), colBg, fH * 0.5f);
    dl->AddText(ImVec2(pos.x + 8.0f, pos.y + 3.0f), colText, pszText);
    PopSmallFont();
}

/** Blue download button with a small download glyph (spec layout). */
bool RenderDownloadButton(const char* pszLabel, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0x3B, 0x82, 0xF6, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          IM_COL32(0x60, 0xA5, 0xFA, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          IM_COL32(0x25, 0x63, 0xEB, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0xFF, 0xFF, 0xFF, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool bClicked = ImGui::Button("##download_btn", size);
    ImGui::PopStyleVar(2);
    const ImVec2 rMin = ImGui::GetItemRectMin();
    const ImVec2 rMax = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = (rMin.y + rMax.y) * 0.5f;
    const float cx = rMin.x + 22.0f;
    const ImU32 col = IM_COL32(0xFF, 0xFF, 0xFF, 235);
    dl->AddLine(ImVec2(cx, cy - 4.0f), ImVec2(cx, cy + 2.0f), col, 1.5f);
    dl->AddLine(ImVec2(cx - 3.5f, cy - 1.0f), ImVec2(cx, cy + 2.0f), col,
                1.5f);
    dl->AddLine(ImVec2(cx + 3.5f, cy - 1.0f), ImVec2(cx, cy + 2.0f), col,
                1.5f);
    dl->AddLine(ImVec2(cx - 4.5f, cy + 4.5f), ImVec2(cx + 4.5f, cy + 4.5f),
                col, 1.5f);
    const ImVec2 ts = ImGui::CalcTextSize(pszLabel);
    dl->AddText(ImVec2(cx + 14.0f, cy - ts.y * 0.5f), col, pszLabel);
    ImGui::PopStyleColor(4);
    return bClicked;
}

/** Small text-style action button (colored text, tinted hover background). */
bool RenderTextButton(const char* pszLabel, ImU32 colText, ImU32 colHoverBg,
                      const ImVec2& size) {
    PushSmallFont();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colHoverBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colHoverBg);
    ImGui::PushStyleColor(ImGuiCol_Text, colText);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
    const bool bClicked = ImGui::Button(pszLabel, size);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    PopSmallFont();
    return bClicked;
}

/** Rounded pill badge (mode / status), translucent background. An
 *  optional fixed width makes badges on one row share the same capsule
 *  size; the label stays centered. */
void RenderPill(const char* pszId, const char* pszText, ImU32 colText,
                ImU32 colBg, ImU32 colBorder, float fWidth = 0.0f) {
    PushSmallFont();
    const ImVec2 sz = ImGui::CalcTextSize(pszText);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float fW = std::max(fWidth, sz.x + 16.0f);
    const ImVec2 size(fW, sz.y + 4.0f);
    ImGui::InvisibleButton(pszId, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fRadius = size.y * 0.5f;
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), colBg,
                      fRadius);
    if (colBorder != 0) {
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), colBorder,
                    fRadius, 0, 1.0f);
    }
    dl->AddText(ImVec2(pos.x + (size.x - sz.x) * 0.5f,
                       pos.y + (size.y - sz.y) * 0.5f),
                colText, pszText);
    PopSmallFont();
}

/** Small bordered title-bar button (web mockup style). Text is drawn
 *  manually so it stays visually centered (CJK glyphs otherwise sit low
 *  inside the button frame). */
bool TitleBarButton(const char* pszLabel, const ImVec2& size) {
    PushSmallFont();
    const ImVec2 vMin = ImGui::GetCursorScreenPos();
    const ImVec2 vMax(vMin.x + size.x, vMin.y + size.y);
    ImGui::InvisibleButton(pszLabel, size);
    const bool bHovered = ImGui::IsItemHovered();
    const bool bClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImDrawList* pDraw = ImGui::GetWindowDrawList();
    if (bHovered) {
        pDraw->AddRectFilled(vMin, vMax,
                             IM_COL32(0x3F, 0x3F, 0x46, 102), 4.0f);
    }
    pDraw->AddRect(vMin, vMax, IM_COL32(0x3F, 0x3F, 0x46, 255), 4.0f, 0,
                   1.0f);
    const ImVec2 ts = ImGui::CalcTextSize(pszLabel);
    const float fLift = ts.y * 0.10f; /* CJK 字形在行框内偏下，向上补偿 */
    pDraw->AddText(
        ImVec2(vMin.x + (size.x - ts.x) * 0.5f,
               vMin.y + (size.y - ts.y) * 0.5f - fLift),
        IM_COL32(0xA1, 0xA1, 0xAA, 255), pszLabel);
    PopSmallFont();
    return bClicked;
}

/* ---- Task row rendering (spec 2.4: segmented per-thread bars) ---- */

/** Status pill text: percent until done (done/error keep their labels). */
std::string StatusText(const CTaskModel::TTaskRow& tRow) {
    switch (tRow.emState) {
        case emTaskDone:
            return std::string(i18n::T("stage.done"));
        case emTaskError:
            return std::string(i18n::T("stage.error"));
        default:
            break;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.0f%%", tRow.dPercent);
    return std::string(buf);
}

/** Status pill colors (web mockup palette). */
void StatusColors(const CTaskModel::TTaskRow& tRow, ImU32& colText,
                  ImU32& colBg) {
    switch (tRow.emState) {
        case emTaskRunning:
            colText = IM_COL32(0x93, 0xC5, 0xFD, 255);  /* blue-300 */
            colBg = IM_COL32(0x3B, 0x82, 0xF6, 38);     /* blue-500/15 */
            break;
        case emTaskPending:
            colText = IM_COL32(0xA1, 0xA1, 0xAA, 255);  /* zinc-400 */
            colBg = IM_COL32(0x71, 0x71, 0x7A, 38);     /* zinc-500/15 */
            break;
        case emTaskCanceled:
            colText = IM_COL32(0xFA, 0xCC, 0x15, 255);  /* yellow-400 */
            colBg = IM_COL32(0xEA, 0xB3, 0x08, 38);     /* yellow-500/15 */
            break;
        case emTaskDone:
            colText = IM_COL32(0x4A, 0xDE, 0x80, 255);  /* green-400 */
            colBg = IM_COL32(0x22, 0xC5, 0x5E, 38);     /* green-500/15 */
            break;
        case emTaskError:
        default:
            colText = IM_COL32(0xF8, 0x71, 0x71, 255);  /* red-400 */
            colBg = IM_COL32(0xEF, 0x44, 0x44, 38);     /* red-500/15 */
            break;
    }
}

/** Progress row: segmented per-thread bar + total percent/speed + action
 *  buttons (stop / resume / delete / remove), all vertically centered on
 *  the 20px bar. Every button uses the shared row capsule width so the
 *  group forms exact columns under the badges above (Delete/Remove land
 *  under the status pill, Stop/Resume under the mode badge). Delete and
 *  remove hover in red. */
void RenderProgressRow(CTaskModel& cModel, const CTaskModel::TTaskRow& tRow,
                       const float fCapsuleW) {
    const float fGap = ImGui::GetStyle().ItemSpacing.x;
    char szInfo[96];
    snprintf(szInfo, sizeof(szInfo), "%s", FormatSpeed(tRow.dSpeed).c_str());

    /* Action buttons (delete/remove hover red). */
    std::vector<const char*> vecActions;
    if ((tRow.emState == emTaskRunning) || (tRow.emState == emTaskPending)) {
        vecActions.push_back(i18n::T("button.stop"));
        vecActions.push_back(i18n::T("button.delete"));
    } else if (tRow.emState == emTaskCanceled) {
        vecActions.push_back(i18n::T("button.resume"));
        vecActions.push_back(i18n::T("button.delete"));
    } else {
        vecActions.push_back(i18n::T("button.remove"));
    }

    PushSmallFont();
    /* Clamp the percent/speed text to the 112px column so long speeds
     * never push into the fixed action buttons. */
    std::string strInfo = szInfo;
    while (!strInfo.empty() &&
           ImGui::CalcTextSize(strInfo.c_str()).x > 108.0f) {
        strInfo.pop_back();
        while (!strInfo.empty() &&
               (((unsigned char)strInfo.back() & 0xC0) == 0x80)) {
            strInfo.pop_back();
        }
    }
    if (strInfo != szInfo) {
        strInfo += "...";
    }
    /* All visible action buttons share the same capsule width as the
     * badges above, so the row reads as an aligned column under them. */
    const float fButtonsW =
        fCapsuleW * (float)vecActions.size() +
        fGap * (float)((int)vecActions.size() - 1);
    const float fTextH = ImGui::GetTextLineHeight();
    const float fBtnH = fTextH + 4.0f;

    /* Trailing gap keeps the button group's right edge aligned with the
     * row badges (content right minus one item spacing). */
    const float fRightW = 112.0f + fGap * 2.0f + fButtonsW;
    float fAvail = ImGui::GetContentRegionAvail().x - fRightW;
    if (fAvail < 80.0f) {
        fAvail = 80.0f;
    }
    const float fBarH = 20.0f;
    const float fBarY = ImGui::GetCursorPosY();
    const std::vector<ThreadView> vecViews = BuildThreadViews(tRow);
    SegmentProgressBar((int)vecViews.size(), vecViews.data(), fAvail, fBarH);

    /* Total percent / speed, right-aligned in the 112px column and
     * vertically centered on the bar. */
    ImGui::SameLine();
    const float fTextW = ImGui::CalcTextSize(strInfo.c_str()).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         (112.0f - fGap - fTextW));
    ImGui::SetCursorPosY(fBarY + (fBarH - fTextH) * 0.5f);
    ImGui::TextColored(
        ImGui::ColorConvertU32ToFloat4(IM_COL32(0xA1, 0xA1, 0xAA, 255)),
        "%s", strInfo.c_str());

    /* Action buttons on the same line, centered on the bar. SameLine()
     * alone would pin later buttons to the previous line's top and drop
     * the first button's centering, so every button gets an explicit Y. */
    const float fBtnTop = fBarY + (fBarH - fBtnH) * 0.5f;
    ImGui::SameLine(0, fGap);
    for (size_t i = 0; i < vecActions.size(); ++i) {
        if (i > 0) {
            ImGui::SameLine(0, fGap);
        }
        ImGui::SetCursorPosY(fBtnTop);
        const char* pszAction = vecActions[i];
        const BOOL32 bStop =
            (strcmp(pszAction, i18n::T("button.stop")) == 0);
        const BOOL32 bResume =
            (strcmp(pszAction, i18n::T("button.resume")) == 0);
        const BOOL32 bDelete =
            (strcmp(pszAction, i18n::T("button.delete")) == 0);
        ImU32 colText = IM_COL32(0xF8, 0x71, 0x71, 255);
        ImU32 colHover = IM_COL32(0xEF, 0x44, 0x44, 25);
        if (bResume) {
            colText = IM_COL32(0x4A, 0xDE, 0x80, 255);
            colHover = IM_COL32(0x22, 0xC5, 0x5E, 25);
        }
        if (RenderTextButton(pszAction, colText, colHover,
                             ImVec2(fCapsuleW, 0))) {
            if (bStop) {
                cModel.CancelTask(tRow.dwModelId);
            } else if (bResume) {
                cModel.ResumeTask(tRow.dwModelId);
            } else if (bDelete) {
                cModel.DeleteTask(tRow.dwModelId);
            } else {
                cModel.RemoveTask(tRow.dwModelId);
            }
            if (g_selected_model_id == tRow.dwModelId) {
                g_selected_model_id = 0;
            }
        }
    }
    PopSmallFont();
}

/** One task row: name + url, pills, segmented bar, meta + actions. */
void RenderTaskRow(CTaskModel& cModel, const CTaskModel::TTaskRow& tRow) {
    const float fGap = ImGui::GetStyle().ItemSpacing.x;
    const float fAvail = ImGui::GetContentRegionAvail().x;
    /* First row: seed the hover height with the exact expected row height
     * (rows are uniform), so the very first hover has no bleed below. */
    if (g_task_row_h <= 0.0f) {
        PushSmallFont();
        const float fSmallH = ImGui::GetTextLineHeight();
        PopSmallFont();
        const float fMainH = ImGui::GetTextLineHeight();
        const float fLine1 = std::max(fMainH, fSmallH + 6.0f);
        const float fLine3 = std::max(20.0f, fSmallH + 4.0f);
        const float fSpacing = ImGui::GetStyle().ItemSpacing.y;
        g_task_row_h = fLine1 + fSmallH + fLine3 + fSmallH +
                       fSpacing * 4.0f;
    }
    /* Row hover background (web: hover:bg rgba(63,63,70,0.40)); the height
     * is the previous row's measured height, uniform across rows. */
    const float fRowTop = ImGui::GetCursorScreenPos().y;
    {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float fRowX0 = ImGui::GetCursorScreenPos().x;
        if ((m.y >= fRowTop) && (m.y <= fRowTop + g_task_row_h) &&
            (m.x >= fRowX0) && (m.x <= fRowX0 + fAvail)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(fRowX0, fRowTop),
                              ImVec2(fRowX0 + fAvail, fRowTop + g_task_row_h),
                              IM_COL32(0x3F, 0x3F, 0x46, 102));
        }
    }

    /* Display name: output basename (+.mp4 for video tasks). */
    std::string strName = tRow.strOutput;
    const size_t slash = strName.find_last_of("/\\");
    if (slash != std::string::npos) {
        strName = strName.substr(slash + 1);
    }
    if (tRow.bVideo != FALSE) {
        const char* kVideoExts[] = {".mp4", ".mkv", ".webm", ".mov", ".flv"};
        BOOL32 bHasExt = FALSE;
        for (const char* pszExt : kVideoExts) {
            if (strName.size() >= strlen(pszExt) &&
                strName.compare(strName.size() - strlen(pszExt),
                                strlen(pszExt), pszExt) == 0) {
                bHasExt = TRUE;
                break;
            }
        }
        if (bHasExt == FALSE) {
            strName += ".mp4";
        }
    }
    if (strName.empty()) {
        strName = UrlFileName(tRow.strUrl);
    }

    const char* pszMode = (tRow.bVideo != FALSE)
                              ? i18n::T("label.type_video")
                              : i18n::T("label.type_file");

    const std::string strStatus = StatusText(tRow);
    ImU32 colStatusText = 0;
    ImU32 colStatusBg = 0;
    StatusColors(tRow, colStatusText, colStatusBg);

    /* One shared capsule width for the whole badge + action cluster:
     * mode, status and every possible action label (stop / delete /
     * resume / remove) are measured so the two rows always form aligned
     * columns regardless of task state or language. */
    PushSmallFont();
    float fCapsuleW = std::max(ImGui::CalcTextSize(pszMode).x,
                               ImGui::CalcTextSize(strStatus.c_str()).x);
    const char* apszActions[] = {
        i18n::T("button.stop"), i18n::T("button.delete"),
        i18n::T("button.resume"), i18n::T("button.remove")};
    for (const char* pszAction : apszActions) {
        fCapsuleW = std::max(fCapsuleW, ImGui::CalcTextSize(pszAction).x);
    }
    fCapsuleW += 16.0f;
    PopSmallFont();
    const float fRightW = fCapsuleW * 2.0f + fGap * 2.0f;
    const float fLeftW = fAvail - fRightW - fGap;
    const float fRowY = ImGui::GetCursorPosY();

    /* Right side: mode badge + status pill on the same line. */
    ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + fLeftW + fGap);
    RenderPill("##mode_pill", pszMode, IM_COL32(0xA1, 0xA1, 0xAA, 255),
               IM_COL32(0xA1, 0xA1, 0xAA, 25),
               IM_COL32(0x3F, 0x3F, 0x46, 255), fCapsuleW);
    ImGui::SameLine(0, fGap);
    RenderPill("##status_pill", strStatus.c_str(), colStatusText,
               colStatusBg, 0, fCapsuleW);

    /* Left side: selectable file name (selects the detail log). */
    ImGui::SetCursorPosY(fRowY);
    ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
    std::string strNameShow = strName;
    while (!strNameShow.empty() &&
           ImGui::CalcTextSize(strNameShow.c_str()).x > fLeftW) {
        strNameShow.pop_back();
    }
    if (strNameShow != strName) {
        strNameShow += "...";
    }
    /* The name keeps one color for every row. The selection (auto-selected
     * first row / clicked row) stays internal so the detail log below has
     * a target, but it is not shown by recoloring the name. */
    const bool bSelected = (g_selected_model_id == tRow.dwModelId);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0xE4, 0xE4, 0xE7, 255));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0, 0, 0, 0));
    if (ImGui::Selectable(strNameShow.c_str(), bSelected, 0,
                          ImVec2(fLeftW, 0))) {
        g_selected_model_id = tRow.dwModelId;
    }
    ImGui::PopStyleColor(4);
    /* URL line (thunder:// prefix when decoded), truncated to fit. */
    const std::string strUrlLine =
        (tRow.bDecoded != FALSE) ? "thunder:// → " + tRow.strUrl
                                 : tRow.strUrl;
    std::string strUrlShow = strUrlLine;
    while (!strUrlShow.empty() &&
           ImGui::CalcTextSize(strUrlShow.c_str()).x > fLeftW) {
        strUrlShow.pop_back();
    }
    if (strUrlShow != strUrlLine) {
        strUrlShow += "...";
    }
    PushSmallFont();
    ImGui::TextDisabled("%s", strUrlShow.c_str());
    PopSmallFont();

    /* Progress row: segments + percent/speed + action buttons. */
    RenderProgressRow(cModel, tRow, fCapsuleW);

    /* Meta row: size · threads (left). */
    char szMeta[160];
    snprintf(szMeta, sizeof(szMeta), "%s · %d %s",
             FormatSizeBytes(tRow.llFileTotal).c_str(), tRow.nThreads,
             i18n::T("unit.threads"));
    ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
    PushSmallFont();
    ImGui::TextDisabled("%s", szMeta);
    PopSmallFont();
    g_task_row_h = ImGui::GetCursorScreenPos().y - fRowTop;
}

/** @brief Replace a fixed buffer's content with the clipboard text,
 *         truncated to fit at a UTF-8 boundary. */
void PasteIntoBuffer(char* pBuf, size_t nBufSize) {
    const char* pClip = ImGui::GetClipboardText();
    if (pClip == nullptr) {
        return;
    }
    size_t nLen = strlen(pClip);
    if (nLen >= nBufSize) {
        nLen = nBufSize - 1;
        while (nLen > 0 && (((unsigned char)pClip[nLen] & 0xC0) == 0x80)) {
            --nLen;
        }
    }
    memcpy(pBuf, pClip, nLen);
    pBuf[nLen] = '\0';
}

/** @brief Right-click edit menu (paste / copy / cut) for a text input.
 *         Call immediately after the InputText* widget that owns the
 *         buffer, before any other item is submitted. */
void RenderTextEditPopup(const char* pszPopupId, char* pBuf,
                         size_t nBufSize) {
    if (!ImGui::BeginPopupContextItem(pszPopupId)) {
        return;
    }
    const char* pClip = ImGui::GetClipboardText();
    const bool bCanPaste = (pClip != nullptr) && (pClip[0] != '\0');
    const bool bHasText = (pBuf[0] != '\0');
    if (ImGui::MenuItem(i18n::T("context.paste"), nullptr, false,
                        bCanPaste)) {
        PasteIntoBuffer(pBuf, nBufSize);
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem(i18n::T("context.copy"), nullptr, false,
                        bHasText)) {
        ImGui::SetClipboardText(pBuf);
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem(i18n::T("context.cut"), nullptr, false,
                        bHasText)) {
        ImGui::SetClipboardText(pBuf);
        pBuf[0] = '\0';
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* ---- Add-task form (always usable; the queue decouples input from the
 *      running tasks, P5-4) ---- */
void RenderAddForm(CTaskModel& cModel) {
    const float fAvail = ImGui::GetContentRegionAvail().x;
    const float fGap = ImGui::GetStyle().ItemSpacing.x;

    /* Row 1: URL input + detection chip + download button (spec 2.1). */
    const bool bHasUrl = g_url[0] != '\0';
    const bool bIsVideo = bHasUrl ? IsVideoUrl(g_url) : false;
    const std::string strChip = std::string(i18n::T("detected")) + ": " +
                                (bIsVideo ? i18n::T("label.type_video")
                                          : i18n::T("label.type_file"));
    const float fBtnW =
        ImGui::CalcTextSize(i18n::T("button.download")).x + 48.0f;
    const float fUrlW = fAvail - fBtnW - fGap;
    if (g_focus_url_input) {
        ImGui::SetKeyboardFocusHere();
        g_focus_url_input = false;
    }
    ImGui::SetNextItemWidth(fUrlW);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 8));
    if (ImGui::InputTextWithHint(
            "##url", i18n::T("placeholder.url.auto"), g_url, sizeof(g_url),
            ImGuiInputTextFlags_EnterReturnsTrue)) {
        AddTaskFromForm(cModel);
    }
    ImGui::PopStyleVar();
    RenderTextEditPopup("##url_ctx", g_url, sizeof(g_url));
    const ImVec2 vInputMin = ImGui::GetItemRectMin();
    const ImVec2 vInputMax = ImGui::GetItemRectMax();
    const bool bUrlActive =
        ImGui::IsItemActive() || ImGui::IsItemFocused();
    /* Web-style focus ring: blue border while the URL box is active. */
    if (bUrlActive) {
        ImGui::GetWindowDrawList()->AddRect(
            vInputMin, vInputMax, IM_COL32(0x3B, 0x82, 0xF6, 255), 8.0f, 0,
            1.5f);
    }
    /* Detection chip overlaid inside the input's right edge; a solid panel
     * patch behind it covers typed text that would scroll underneath (the
     * desktop equivalent of the web's input right padding). */
    if (bHasUrl) {
        const ImVec2 szChip = ImGui::CalcTextSize(strChip.c_str());
        const float fChipH = szChip.y + 6.0f;
        const float fChipW = szChip.x + 16.0f;
        ImDrawList* pDraw = ImGui::GetWindowDrawList();
        pDraw->AddRectFilled(
            ImVec2(vInputMax.x - fChipW - 18.0f, vInputMin.y + 1.0f),
            ImVec2(vInputMax.x - 1.0f, vInputMax.y - 1.0f),
            IM_COL32(0x21, 0x25, 0x2B, 255), 8.0f,
            ImDrawFlags_RoundCornersRight);
        const ImVec2 vChipPos(
            vInputMax.x - fChipW - 10.0f,
            vInputMin.y + (vInputMax.y - vInputMin.y - fChipH) * 0.5f);
        RenderChip(strChip.c_str(), bIsVideo ? TRUE : FALSE, vChipPos);
    }
    ImGui::SameLine();
    if (RenderDownloadButton(i18n::T("button.download"),
                             ImVec2(fBtnW, 0))) {
        AddTaskFromForm(cModel);
    }

    /* Row 2 (HTML style): 保存到 [path][浏览] left, 线程 [combo] right. */
    const float fThreadsLabelW =
        ImGui::CalcTextSize(i18n::T("label.threads")).x;
    const float fComboW = 56.0f;
    const float fBrowseW = 64.0f;
    const float fPathW = 260.0f;  /* shortened save-path box */
    const float fThreadsW = fThreadsLabelW + fComboW + fGap;
    const float fRowY = ImGui::GetCursorPosY();
    /* Threads group pinned to the right edge (drawn first). */
    ImGui::SetCursorPosX(fAvail - fThreadsW);
    PushSmallFont();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 5));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(IM_COL32(0xA1, 0xA1,
                                                              0xAA, 255)),
                       "%s", i18n::T("label.threads"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fComboW);
    char items[128] = {0};
    int off = 0;
    for (int i = 1; i <= kHardwareMax && off < (int)sizeof(items) - 2; i++) {
        off += snprintf(items + off, sizeof(items) - off, "%d%c", i, '\0');
    }
    int idx = (g_threads >= 1 && g_threads <= kHardwareMax) ? g_threads - 1
                                                            : 0;
    ImGui::BeginDisabled(cModel.ActiveCount() > 0u);
    if (ImGui::Combo("##threads", &idx, items, kHardwareMax)) {
        g_threads = idx + 1;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    PopSmallFont();
    /* Save-to group on the left: label inline, then the path box. */
    ImGui::SetCursorPosY(fRowY);
    ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
    PushSmallFont();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 5));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(IM_COL32(0xA1, 0xA1,
                                                              0xAA, 255)),
                       "%s", i18n::T("label.save_to"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fPathW);
    ImGui::InputTextWithHint(
        "##path", i18n::T("placeholder.path.file"), g_path, sizeof(g_path),
        ImGuiInputTextFlags_None);
    ImGui::PopStyleVar();
    RenderTextEditPopup("##path_ctx", g_path, sizeof(g_path));
    /* Web-style focus ring for the save-path box. */
    if (ImGui::IsItemActive() || ImGui::IsItemFocused()) {
        const ImVec2 vPathMin = ImGui::GetItemRectMin();
        const ImVec2 vPathMax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(
            vPathMin, vPathMax, IM_COL32(0x3B, 0x82, 0xF6, 255), 8.0f, 0,
            1.5f);
    }
#ifdef _WIN32
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 5));
    if (ImGui::Button(i18n::T("button.browse"), ImVec2(fBrowseW, 0))) {
        HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        IFileDialog* pfd = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&pfd));
        if (SUCCEEDED(hr)) {
            DWORD opts = 0;
            pfd->GetOptions(&opts);
            pfd->SetOptions(opts | FOS_PICKFOLDERS);
            pfd->SetTitle(L"Select Save Folder");
            if (SUCCEEDED(pfd->Show(NULL))) {
                IShellItem* psi = nullptr;
                if (SUCCEEDED(pfd->GetResult(&psi))) {
                    PWSTR psz = nullptr;
                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH,
                                                       &psz))) {
                        std::string s = WideToUtf8(psz);
                        snprintf(g_path, sizeof(g_path), "%s", s.c_str());
                        CoTaskMemFree(psz);
                    }
                    psi->Release();
                }
            }
            pfd->Release();
        }
        if (hrCo == S_OK) {
            CoUninitialize();
        }
    }
    ImGui::PopStyleVar();
#else
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 5));
    if (ImGui::Button(i18n::T("button.browse"), ImVec2(fBrowseW, 0))) {
        g_dirbrowse_open = true;
        g_dirbrowse_dir = (g_path[0] != '\0' && PathExists(g_path))
                              ? g_path
                              : std::filesystem::current_path().string();
        std::error_code ec;
        if (!std::filesystem::is_directory(g_dirbrowse_dir, ec) || ec) {
            auto parent = std::filesystem::path(g_dirbrowse_dir).parent_path();
            g_dirbrowse_dir =
                parent.empty() ? std::string("/") : parent.string();
        }
    }
    ImGui::PopStyleVar();
#endif
    PopSmallFont();
}

void AddTaskFromForm(CTaskModel& cModel) {
    std::string url(g_url);
    std::string path(g_path);
    BOOL32 bDecoded = FALSE;

    /* Decode thunder:// links to real URLs (spec 2.1). */
    if (url.rfind("thunder://", 0) == 0) {
        std::string decoded = ThunderDecode(url);
        if (decoded.empty() || !UrlSchemeOk(decoded)) {
            ShowErrorPopup(i18n::T("dialog.error.title"),
                           i18n::T("err.thunder.invalid"));
            return;
        }
        snprintf(g_url, sizeof(g_url), "%s", decoded.c_str());
        url = decoded;
        bDecoded = TRUE;
    }
    if (url.empty()) {
        g_focus_url_input = true;  /* web behavior: focus, no error popup */
        return;
    }
    if (!UrlSchemeOk(url)) {
        ShowErrorPopup(i18n::T("dialog.error.title"),
                       i18n::T("err.url.invalid"));
        return;
    }
    if (path.empty()) {
        ShowErrorPopup(i18n::T("dialog.error.title"),
                       i18n::T("err.path.empty"));
        return;
    }

    /* Auto-classify on the decoded URL (spec 2.1 priority order). */
    if (IsVideoUrl(url)) {
        std::string base = UrlFileName(url);
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) {
            base = base.substr(0, dot);
        }
        if (base.empty()) {
            base = "video";
        }
        const std::string basename =
            JoinPath(path, base + "_" + CurrentTimeStamp());
        if (cModel.AddVideoTask(url, basename, g_threads, 60, FALSE,
                                bDecoded) == 0) {
            ShowErrorPopup(i18n::T("dialog.error.title"), i18n::T("err.busy"));
        } else {
            g_url[0] = '\0';  /* clear the URL after a successful add */
        }
        return;
    }

    if (IsDirectoryPath(path)) {
        std::string name = UrlFileName(url);
        if (name.empty()) {
            name = CurrentTimeStamp() + ".download";
        }
        path = JoinPath(path, name);
    }
    if (PathExists(path)) {
        path = StampName(path);
    }
    if (cModel.AddFileTask(url, path, g_threads, 60, FALSE, bDecoded) == 0) {
        ShowErrorPopup(i18n::T("dialog.error.title"), i18n::T("err.busy"));
    } else {
        g_url[0] = '\0';  /* clear the URL after a successful add */
    }
}

/* ---- Task list (spec 2.2/2.4) ---- */
void RenderTaskList(CTaskModel& cModel,
                    std::vector<CTaskModel::TTaskRow>& vecRowsOut) {
    vecRowsOut = cModel.Rows();
    const std::vector<CTaskModel::TTaskRow>& vecRows = vecRowsOut;

    /* Auto-select the first row so the detail log has a target. */
    bool bSelFound = false;
    for (const CTaskModel::TTaskRow& tRow : vecRows) {
        if (tRow.dwModelId == g_selected_model_id) {
            bSelFound = true;
            break;
        }
    }
    if (!bSelFound && !vecRows.empty()) {
        g_selected_model_id = vecRows[0].dwModelId;
    }

    /* Slim header: clear-finished pinned right, same right inset as the
     * per-task Done pills. */
    const float fGapHdr = ImGui::GetStyle().ItemSpacing.x;
    const float fClearW =
        ImGui::CalcTextSize(i18n::T("label.clear_finished")).x + 16.0f;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - fClearW -
                         fGapHdr);
    if (RenderTextButton(i18n::T("label.clear_finished"),
                         IM_COL32(0xA1, 0xA1, 0xAA, 255),
                         IM_COL32(0x3F, 0x3F, 0x46, 102), ImVec2(0, 0))) {
        cModel.ClearFinished();
    }

    /* Exact height accounting: status bar (32px) always reserved, the log
     * section header (44px) always visible so the toggle exists, the log
     * body gets up to 150px when open (shrinks first), and the task list
     * takes the rest. list + log + status == avail, so the status bar is
     * never clipped by the window bottom. */
    const float fStatusH = 32.0f;
    const float fLogHdrH = 44.0f;
    const float fMinListH = 40.0f;
    const float fLogMaxChildH = 150.0f;
    const float fAvailY = ImGui::GetContentRegionAvail().y;
    float fChildWanted = g_log_open ? fLogMaxChildH : 0.0f;
    float fListWanted =
        fAvailY - fStatusH - fLogHdrH - fChildWanted - 4.0f;
    if (fListWanted < fMinListH) {
        fChildWanted = std::max(
            0.0f, fAvailY - fStatusH - fLogHdrH - fMinListH - 4.0f);
        fListWanted =
            fAvailY - fStatusH - fLogHdrH - fChildWanted - 4.0f;
    }
    g_log_h = fChildWanted;
    const float fListH = std::max(fListWanted, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 102));
    ImGui::BeginChild("##tasklist", ImVec2(0, fListH), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (vecRows.empty()) {
        PushSmallFont();
        const float fEmptyW = ImGui::CalcTextSize(i18n::T("empty.tasks")).x;
        ImGui::SetCursorPos(
            ImVec2((ImGui::GetContentRegionAvail().x - fEmptyW) * 0.5f,
                   36.0f));
        ImGui::TextDisabled("%s", i18n::T("empty.tasks"));
        PopSmallFont();
    } else {
        for (const CTaskModel::TTaskRow& tRow : vecRows) {
            ImGui::PushID(static_cast<int>(tRow.dwModelId));
            RenderTaskRow(cModel, tRow);
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

/* ---- Collapsible log section for the selected task ---- */
void RenderLogSection(CTaskModel& cModel) {
    /* Header is always rendered so the toggle stays visible even when the
     * window is too short for the log body. The bar is drawn manually so
     * it exactly matches the log text box width below: ImGui's framed
     * CollapsingHeader extends half a WindowPadding beyond the content
     * area on both sides, which would make the toggle visibly wider than
     * the log box. */
    const float fBarH = 44.0f;
    const float fBarW = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##log_toggle", ImVec2(fBarW, fBarH));
    const bool bHovered = ImGui::IsItemHovered();
    const bool bClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const ImVec2 vMin = ImGui::GetItemRectMin();
    const ImVec2 vMax = ImGui::GetItemRectMax();
    ImDrawList* pDraw = ImGui::GetWindowDrawList();
    pDraw->AddRectFilled(
        vMin, vMax,
        bHovered ? IM_COL32(0x2A, 0x2F, 0x37, 255)
                 : IM_COL32(0x21, 0x25, 0x2B, 255));
    /* Disclosure arrow (open = down, closed = right). */
    const float cy = (vMin.y + vMax.y) * 0.5f;
    const float cx = vMin.x + 18.0f;
    const ImU32 colArrow = IM_COL32(0xA1, 0xA1, 0xAA, 255);
    if (g_log_open) {
        pDraw->AddTriangleFilled(ImVec2(cx - 3.5f, cy - 2.5f),
                                 ImVec2(cx + 3.5f, cy - 2.5f),
                                 ImVec2(cx, cy + 3.0f), colArrow);
    } else {
        pDraw->AddTriangleFilled(ImVec2(cx - 2.5f, cy - 3.5f),
                                 ImVec2(cx - 2.5f, cy + 3.5f),
                                 ImVec2(cx + 3.0f, cy), colArrow);
    }
    const char* pszLabel = i18n::T("label.log");
    const ImVec2 ts = ImGui::CalcTextSize(pszLabel);
    const float fLift = ts.y * 0.10f; /* CJK glyphs sit low: lift up */
    pDraw->AddText(ImVec2(vMin.x + 36.0f, cy - ts.y * 0.5f - fLift),
                   ImGui::GetColorU32(ImGuiCol_Text), pszLabel);
    if (bClicked) {
        g_log_open = !g_log_open;
    }
    if (g_log_open && g_log_h >= 20.0f) {
        std::vector<std::string> vecLog;
        if (g_selected_model_id != 0) {
            DownloadSnapshot snap;
            if (cModel.TaskDetail(g_selected_model_id, snap, vecLog) ==
                FALSE) {
                g_selected_model_id = 0;
            }
        }
        RenderLog(vecLog, std::max(g_log_h - 32.0f, 8.0f));
    }
}

/* ---- Window status bar (spec 2.4: tasks/slots + weighted progress) ---- */
void RenderStatusBar(const std::vector<CTaskModel::TTaskRow>& vecRows,
                     u32 dwMaxSlots) {
    ImGui::Separator();
    PushSmallFont();
    const u32 dwTotal = (u32)vecRows.size();
    u32 dwActive = 0;
    double dTotalBytes = 0.0;
    double dWeighted = 0.0;
    double dTotalSpeed = 0.0;
    for (const CTaskModel::TTaskRow& tRow : vecRows) {
        const double dSize = (double)tRow.llFileTotal;
        dTotalBytes += dSize;
        dWeighted += dSize * tRow.dPercent;
        if (tRow.emState == emTaskRunning) {
            ++dwActive;
            dTotalSpeed += tRow.dSpeed;
        }
    }
    const double dOverall =
        dTotalBytes > 0.0 ? dWeighted / dTotalBytes : 0.0;
    char szLeft[128];
    snprintf(szLeft, sizeof(szLeft), "%u %s · %u/%u %s", dwTotal,
             i18n::T("tasks.unit"), dwActive, dwMaxSlots, i18n::T("slots"));
    char szRight[160];
    snprintf(szRight, sizeof(szRight), "%s %.0f%% · %s", i18n::T("overall"),
             dOverall, FormatSpeed(dTotalSpeed).c_str());
    const float fGap = ImGui::GetStyle().ItemSpacing.x;
    const float fBarW = 80.0f;
    const float fBarH = 6.0f;
    const ImVec2 ts = ImGui::CalcTextSize(szRight);
    const float fRightW = fBarW + fGap + ts.x;
    /* Same baseline for the left text and the right bar + text. */
    const float fLineY = ImGui::GetCursorPosY();
    ImGui::TextColored(
        ImGui::ColorConvertU32ToFloat4(IM_COL32(0xA1, 0xA1, 0xAA, 255)),
        "%s", szLeft);
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - fRightW);
    ImGui::SetCursorPosY(fLineY +
                         (ImGui::GetTextLineHeight() - fBarH) * 0.5f);
    TotalProgressBar((float)(dOverall / 100.0), fBarW, fBarH);
    ImGui::SameLine();
    ImGui::SetCursorPosY(fLineY);
    ImGui::TextColored(
        ImGui::ColorConvertU32ToFloat4(IM_COL32(0xA1, 0xA1, 0xAA, 255)),
        "%s", szRight);
    PopSmallFont();
}

/* ---- Log area (F7) ---- */
void RenderLog(const std::vector<std::string>& log, float fHeight) {
    ImGui::Separator();
    PushSmallFont();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 2.0f));
    if (ImGui::Button(i18n::T("log.copy"))) {
        std::string all;
        for (const auto& line : log) {
            all += line;
            all += "\n";
        }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::PopStyleVar(2);
    PopSmallFont();
    /* Full width, aligned with the task-list box above. */
    ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
    ImGui::BeginChild("##log", ImVec2(0, fHeight), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const bool bAtBottom =
        ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
    if (bAtBottom) {
        g_log_autoscroll = true;
    }
    for (const auto& line : log) {
        /* No wrapping: a horizontal scrollbar shows when lines are wide. */
        ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetIO().MouseWheel != 0.0f &&
        ImGui::IsWindowHovered()) {
        g_log_autoscroll = false;  /* user scrolled up: stop pinning */
    }
    if (g_log_autoscroll && ImGui::GetScrollMaxY() > 0) {
        ImGui::SetScrollY(ImGui::GetScrollMaxY());
    }
    ImGui::EndChild();
}

/* ---- Mac-style circular window buttons (red=close / yellow=minimize /
 * green=maximize, hover tooltip) ---- */
bool MacCircleButton(float cx, float cy, float d, ImU32 color, ImU32 hover,
                     const char* tip) {
    /* Unique color -> the three button IDs do not collide (fixes the
     * "Program error" complaint). */
    ImGui::PushID((int)color);
    ImGui::SetCursorScreenPos(ImVec2(cx - d * 0.5f, cy - d * 0.5f));
    ImGui::InvisibleButton("##macbtn", ImVec2(d, d));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(ImVec2(cx, cy), d * 0.5f, hovered ? hover : color, 24);
    if (hovered && tip != nullptr) {
        ImGui::SetTooltip("%s", tip);
    }
    return clicked;
}

/* ---- About dropdown menu (spec 2.3: version/website/github/issues/license/
 * tech stack/platforms; links open in the default browser) ---- */
void RenderAboutMenu() {
    PushSmallFont();
    const struct TAboutRow {
        const char* pszKey;
        std::string strValue;
        BOOL32 bLink;
    } kRows[] = {
        {"dialog.about.version", std::string("v") + BURST_VERSION_STRING,
         FALSE},
        {"website", "https://www.burstdownload.com", TRUE},
        {"github", "https://github.com/ErnestAgel/burst-download", TRUE},
        {"issues", "https://github.com/ErnestAgel/burst-download/issues",
         TRUE},
        {"license", "MIT", FALSE},
        {"tech_stack", "C/C++ · libcurl", FALSE},
        {"platforms", "Windows x86_64 · Linux x86_64 · Linux ARM64", FALSE},
    };
    char szHead[64];
    snprintf(szHead, sizeof(szHead), "%s", i18n::T("menu.about"));
    /* Header strip (web: #181C23, full popup width, rounded top). */
    const ImVec2 vHeadMin = ImGui::GetWindowPos();
    const float fHeadH = ImGui::GetTextLineHeight() + 16.0f;
    ImGui::GetWindowDrawList()->AddRectFilled(
        vHeadMin,
        ImVec2(vHeadMin.x + ImGui::GetWindowSize().x, vHeadMin.y + fHeadH),
        IM_COL32(0x18, 0x1C, 0x23, 255), 8.0f,
        ImDrawFlags_RoundCornersTop);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 12.0f);
    ImGui::TextColored(
        ImGui::ColorConvertU32ToFloat4(IM_COL32(0xE4, 0xE4, 0xE7, 255)),
        "%s", szHead);
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::Separator();
    /* One selectable row per entry: label left, value right-aligned
     * (links open in the browser; no nested buttons/tables). */
    const float fTextH = ImGui::GetTextLineHeight();
    for (size_t i = 0; i < sizeof(kRows) / sizeof(kRows[0]); ++i) {
        const TAboutRow& tRow = kRows[i];
        ImGui::PushID(static_cast<int>(i));
        const bool bClicked = ImGui::Selectable("##row", false, 0,
                                                ImVec2(0, 30.0f));
        const ImVec2 vMin = ImGui::GetItemRectMin();
        const ImVec2 vMax = ImGui::GetItemRectMax();
        const float fRowH = vMax.y - vMin.y;
        ImDrawList* pDraw = ImGui::GetWindowDrawList();
        const float fLabelW =
            ImGui::CalcTextSize(i18n::T(tRow.pszKey)).x;
        pDraw->AddText(ImVec2(vMin.x + 12.0f,
                              vMin.y + (fRowH - fTextH) * 0.5f),
                       IM_COL32(0xA1, 0xA1, 0xAA, 255),
                       i18n::T(tRow.pszKey));
        std::string strShow = tRow.strValue;
        const float fMaxValW =
            (vMax.x - vMin.x) - 24.0f - fLabelW - 16.0f;
        while (!strShow.empty() &&
               ImGui::CalcTextSize(strShow.c_str()).x > fMaxValW) {
            strShow.pop_back();
        }
        if (strShow != tRow.strValue) {
            strShow += "...";
        }
        const float fValW = ImGui::CalcTextSize(strShow.c_str()).x;
        const ImU32 uValCol = (tRow.bLink != FALSE)
                                  ? IM_COL32(0x93, 0xC5, 0xFD, 255)
                                  : IM_COL32(0xD4, 0xD4, 0xD8, 255);
        pDraw->AddText(ImVec2(vMax.x - fValW - 12.0f,
                              vMin.y + (fRowH - fTextH) * 0.5f),
                       uValCol, strShow.c_str());
        if (bClicked && (tRow.bLink != FALSE)) {
            OpenUrl(tRow.strValue);
            g_about_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
    }
    PopSmallFont();
}

/* ---- Custom title bar (borderless window): left traffic lights, centered
 * title, right About/language buttons + About dropdown (spec 2.3) ---- */
void RenderTitleBar() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, kTitleBarH),
                             ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImGui::GetColorU32(ImGuiCol_TitleBg));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,
                          ImGui::GetColorU32(ImGuiCol_PopupBg));
    ImGui::Begin("##titlebar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float cy = kTitleBarH * 0.5f;  /* button vertical center */

    /* Left: Mac-style traffic lights (red close / yellow minimize / green
     * maximize), same order as the web mockup. */
    const float fBtnD = 12.0f;
    const float fGap = 9.0f;
    const float fMargin = 14.0f;
    const float rx = fMargin + fBtnD * 0.5f;
    const float yx = rx + fBtnD + fGap;
    const float gx = yx + fBtnD + fGap;
    const ImU32 cRed = IM_COL32(0xFF, 0x5F, 0x57, 255);
    const ImU32 cRedH = IM_COL32(0xFF, 0x7A, 0x74, 255);
    const ImU32 cYellow = IM_COL32(0xFE, 0xBC, 0x2E, 255);
    const ImU32 cYellowH = IM_COL32(0xFF, 0xCB, 0x57, 255);
    const ImU32 cGreen = IM_COL32(0x28, 0xC8, 0x40, 255);
    const ImU32 cGreenH = IM_COL32(0x46, 0xD6, 0x5E, 255);
    if (MacCircleButton(rx, cy, fBtnD, cRed, cRedH,
                        i18n::T("button.close")) &&
        g_window != nullptr) {
        glfwSetWindowShouldClose(g_window, GLFW_TRUE);
    }
    if (MacCircleButton(yx, cy, fBtnD, cYellow, cYellowH,
                        i18n::T("button.minimize")) &&
        g_window != nullptr) {
        glfwIconifyWindow(g_window);
    }
    if (MacCircleButton(gx, cy, fBtnD, cGreen, cGreenH,
                        i18n::T("button.maximize")) &&
        g_window != nullptr) {
        if (glfwGetWindowAttrib(g_window, GLFW_MAXIMIZED) == GLFW_TRUE) {
            glfwRestoreWindow(g_window);
        } else {
            glfwMaximizeWindow(g_window);
        }
    }

    /* Right: About + language toggle buttons (small bordered, web style). */
    const float fAboutW =
        ImGui::CalcTextSize(i18n::T("menu.about")).x + 20.0f;
    const float fLangW =
        ImGui::CalcTextSize(i18n::T("menu.lang_hint")).x + 20.0f;
    const float fBtnH = 22.0f;
    const float fRightX =
        io.DisplaySize.x - fLangW - fAboutW - fGap * 3.0f - fMargin;
    ImGui::SetCursorPos(ImVec2(fRightX, (kTitleBarH - fBtnH) * 0.5f));
    bool bAboutClicked = false;
    if (TitleBarButton(i18n::T("menu.about"), ImVec2(fAboutW, fBtnH))) {
        bAboutClicked = true;
    }
    ImGui::SameLine(0, fGap);
    if (TitleBarButton(i18n::T("menu.lang_hint"), ImVec2(fLangW, fBtnH))) {
        i18n::SetLang(i18n::GetLang() == i18n::Lang::Zh ? i18n::Lang::En
                                                        : i18n::Lang::Zh);
    }

    /* Centered title text (main 18px font; web mockup density). */
    char szTitle[128];
    snprintf(szTitle, sizeof(szTitle), "%s v%s", i18n::T("window.title"),
             BURST_VERSION_STRING);
    const float fTitleW = ImGui::CalcTextSize(szTitle).x;
    const float fTitleH = ImGui::GetTextLineHeight();
    ImGui::SetCursorPos(
        ImVec2((io.DisplaySize.x - fTitleW) * 0.5f,
               (kTitleBarH - fTitleH) * 0.5f - 1.0f));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(IM_COL32(0xA1, 0xA1,
                                                              0xAA, 255)),
                       "%s", szTitle);

    /* Title bar drag: on press, trigger native Windows window dragging
     * (WM_NCLBUTTONDOWN/HTCAPTION); non-Windows falls back to manual
     * position updates. */
    const float fDragX0 = fMargin + fBtnD * 3.0f + fGap * 2.0f + 6.0f;
    const float fDragW = fRightX - fDragX0;
    ImGui::SetCursorPos(ImVec2(fDragX0, 0));
    ImGui::InvisibleButton("##titlebar_drag",
                           ImVec2(fDragW, kTitleBarH));
    if (ImGui::IsItemActivated() && g_window != nullptr) {
#ifdef _WIN32
        HWND hwnd = glfwGetWin32Window(g_window);
        if (hwnd != NULL) {
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
#else
        /* Non-Windows fallback: record the press position and move per
         * frame. */
        static bool s_dragging = false;
        static int s_dragX0 = 0;
        static int s_dragY0 = 0;
        static ImVec2 s_dragMouse0;
        if (!s_dragging) {
            s_dragging = true;
            glfwGetWindowPos(g_window, &s_dragX0, &s_dragY0);
            s_dragMouse0 = io.MousePos;
        }
        if (s_dragging) {
            glfwSetWindowPos(g_window,
                             s_dragX0 + (int)(io.MousePos.x - s_dragMouse0.x),
                             s_dragY0 + (int)(io.MousePos.y - s_dragMouse0.y));
        }
        if (!ImGui::IsItemActive()) {
            s_dragging = false;
        }
#endif
    }

    /* About dropdown (spec 2.3); clicking outside closes it. */
    if (bAboutClicked) {
        g_about_open = true;
        ImGui::OpenPopup("##about_menu");
    }
    if (g_about_open) {
        ImGui::SetNextWindowSize(ImVec2(288.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowPos(
            ImVec2(fRightX + fAboutW + fGap, fBtnH + 2.0f), ImGuiCond_Always,
            ImVec2(1.0f, 0.0f));
        if (ImGui::BeginPopup("##about_menu", ImGuiWindowFlags_NoMove)) {
            RenderAboutMenu();
            ImGui::EndPopup();
        } else {
            g_about_open = false;
        }
    }

    ImGui::End();
    /* Bottom divider (web: border-b rgba(0,0,0,0.4)). */
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(0.0f, kTitleBarH - 1.0f),
        ImVec2(io.DisplaySize.x, kTitleBarH - 1.0f),
        IM_COL32(0, 0, 0, 102), 1.0f);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

/* ---- Window edge/corner resize (a borderless window has no system handles;
 * self-drawn: left/right/bottom edges + bottom-left/bottom-right corners) ----
 * The top 6px of the title bar is reserved for dragging and does not
 * resize (avoids conflicting with the title bar InvisibleButton). */
void RenderResizeGrip() {
    const ImGuiIO& io = ImGui::GetIO();
    const float E = 6.0f;  /* edge band width */
    const float C = 18.0f; /* corner area */
    const float W = io.DisplaySize.x;
    const float H = io.DisplaySize.y;

    enum Dir { None, L, R, B, BL, BR };
    const ImVec2 m = io.MousePos;
    Dir dir = None;
    const bool near_l = m.x <= E;
    const bool near_r = m.x >= W - E;
    const bool near_b = m.y >= H - E && m.y <= H;
    if (m.x <= C && m.y >= H - C) {
        dir = BL;
    } else if (m.x >= W - C && m.y >= H - C) {
        dir = BR;
    } else if (near_l) {
        dir = L;
    } else if (near_r) {
        dir = R;
    } else if (near_b) {
        dir = B;
    }

#ifdef _WIN32
    /* Hover resize cursor (when not pressed). */
    if (dir != None && !io.MouseDown[0]) {
        /* System cursor IDs (IDC_* expand to LPSTR; LoadCursorW needs
         * MAKEINTRESOURCEW values). */
        LPCWSTR cur = MAKEINTRESOURCEW(32512); /* IDC_ARROW */
        switch (dir) {
            case L:
            case R: cur = MAKEINTRESOURCEW(32644); break; /* IDC_SIZEWE */
            case B: cur = MAKEINTRESOURCEW(32645); break; /* IDC_SIZENS */
            case BL:
            case BR: cur = MAKEINTRESOURCEW(32642); break; /* IDC_SIZENWSE */
            default: break;
        }
        SetCursor(LoadCursorW(NULL, cur));
    }
#endif

    /* Drag state. */
    static Dir s_dir = None;
    static int w0 = 0, h0 = 0, x0 = 0, y0 = 0;
    static ImVec2 m0;
    if (dir != None && ImGui::IsMouseClicked(0)) {
        if (g_window != nullptr) {
            glfwGetWindowPos(g_window, &x0, &y0);
            glfwGetWindowSize(g_window, &w0, &h0);
        }
        s_dir = dir;
        m0 = io.MousePos;
    } else if (!io.MouseDown[0]) {
        s_dir = None;
    }
    if (s_dir != None && io.MouseDown[0] && g_window != nullptr) {
        const int dx = (int)(io.MousePos.x - m0.x);
        const int dy = (int)(io.MousePos.y - m0.y);
        int nw = w0, nh = h0, nx = x0, ny = y0;
        switch (s_dir) {
            case R:
            case BR: nw = w0 + dx; break;
            case L:
            case BL: nw = w0 - dx; nx = x0 + dx; break;
            case B: break;
            default: break;
        }
        if (s_dir != L && s_dir != R) {
            nh = h0 + dy; /* bottom edge for B/BL/BR */
        }
        /* Minimum size (glfwSetWindowSizeLimits backs this up; guard the
         * left/top edges here). */
        if (nw < 640) {
            if (s_dir == L || s_dir == BL) nx = x0 + (w0 - 640);
            nw = 640;
        }
        if (nh < 520) {
            nh = 520;
        }
        glfwSetWindowSize(g_window, nw, nh);
        if (s_dir == L || s_dir == BL) {
            glfwSetWindowPos(g_window, nx, ny);
        }
    }

    /* Bottom-right triangle marker (resize visual hint, hover highlight). */
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetWindowPos();
    ImVec2 c = ImVec2(p.x + W - 2, p.y + H - kTitleBarH - 2);
    if (dir == BR) {
        ImU32 col = ImGui::GetColorU32(ImGuiCol_SeparatorActive);
        dl->AddTriangleFilled(ImVec2(c.x - 12, c.y), c,
                              ImVec2(c.x, c.y - 12), col);
    }
}

}  // namespace

void Init(GLFWwindow* window) {
    g_window = window;
}

void SetFonts(ImFont* pFontMain, ImFont* pFontSmall) {
    g_pFontMain = pFontMain;
    g_pFontSmall = pFontSmall;
}

bool Render(CTaskModel& cModel) {
#ifdef _WIN32
    /* Custom title bar (Windows borderless window; Linux uses the system
     * title bar and the controls stay in the web mockup order). */
    RenderTitleBar();
#endif

    /* Main window: fills the client area below the title bar. */
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, kTitleBarH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(io.DisplaySize.x, io.DisplaySize.y - kTitleBarH),
        ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);

    /* Add form + examples + task list + collapsible log + status bar. */
    cModel.OnUiTick();
    std::vector<CTaskModel::TTaskRow> vecRows;
    RenderAddForm(cModel);
    RenderTaskList(cModel, vecRows);
    RenderLogSection(cModel);
    RenderStatusBar(vecRows, cModel.MaxSlots());

#ifdef _WIN32
    /* Resize grip: a borderless window has no system handles; edges and
     * corners are hit-tested here (called inside the main window). */
    RenderResizeGrip();
#endif

    ImGui::End();
    ImGui::PopStyleVar();

    /* Error popup (validation / task failures). */
    dialogs::ShowError(g_error_title, g_error_msg, g_error_guide,
                       g_error_open, g_error_partial_path,
                       &g_error_delete_requested);
    if (g_error_delete_requested) {
        g_error_delete_requested = false;
        if (!g_error_partial_path.empty()) {
            RemoveFile(g_error_partial_path);
            RemoveFile(g_error_partial_path + ".curlbolt.part");
        }
        g_error_partial_path.clear();
    }

#ifndef _WIN32
    /* Linux built-in directory browser (Windows uses the native IFileDialog
     * and has no such state). */
    if (g_dirbrowse_open &&
        DirBrowserRender(g_dirbrowse_dir, g_dirbrowse_open)) {
        snprintf(g_path, sizeof(g_path), "%s", g_dirbrowse_dir.c_str());
    }
#endif

    return true;
}
}  // namespace ui
