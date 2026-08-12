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
#include "toggle.h"
#include "Ccurl.h"  /* MaxThread */
#include "pathutil.h"
#include "version.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>  /* IFileDialog folder picker */
#else
#include <sys/stat.h>
#include <filesystem>  /* Linux directory browser initial directory */
#include "dirbrowser.h"
#endif

namespace ui {

namespace {

/* Custom title bar height (borderless window, replaces the system title
 * bar). */
#ifdef _WIN32
const float kTitleBarH = 36.0f; /* Windows custom title bar height */
#else
const float kTitleBarH = 0.0f;  /* Linux uses the system title bar; the
                                 * content area starts at the window top */
#endif

/* GLFW window pointer (injected by ui::Init; used by the title bar buttons,
 * drag and resize). */
GLFWwindow* g_window = nullptr;

/* ---- Form state (UI thread only; worker threads never touch) ---- */
/* Machine thread cap: adapts 4~8 to the CPU core count (see
 * BurstMaxThreads). */
const int kHardwareMax = BurstMaxThreads();

char g_url[2048] = {0};
char g_path[2048] = {0};
int g_threads = BurstDefaultThreads();   /* default = sensible 2~4 */
bool g_video_mode = false;

/* Download control state machine (F9/F10): IDLE -> RUNNING -> PAUSED -> IDLE
 *  - the first Cancel while downloading = pause intent (abort the transfer,
 *    keep the cache file for resume)
 *  - when paused, button 1 = Resume (resume), button 2 = Stop (red, deletes
 *    the cache and refreshes the UI)
 *  - g_paused=true means paused; g_last_path/g_last_video are used by Stop
 *    to delete the cache */
bool g_paused = false;
std::string g_last_path;   /* last task target path/base name (Stop deletes
                            * its cache) */
bool g_last_video = false; /* whether the last task was video mode */

/* Forward declarations (RenderForm is defined before OnStartClicked /
 * StartDownload). */
void OnStartClicked(DownloadWorker& worker);
void StartDownload(DownloadWorker& worker, const std::string& url,
                   const std::string& path, int threads,
                   bool preserve_snapshot = false);
void StartVideoDownload(DownloadWorker& worker, const std::string& url,
                        const std::string& basename, int threads,
                        bool preserve_snapshot = false);
void StopAndClear(DownloadWorker& worker);

/* Popup state */
bool g_exists_open = false;
bool g_error_open = false;
bool g_done_open = false;
std::string g_error_title, g_error_msg, g_error_guide;
std::string g_error_partial_path;
bool g_error_delete_requested = false;
std::string g_done_path;
bool g_about_open = false;

#ifndef _WIN32
/* Linux built-in directory browser state (Windows uses the native
 * IFileDialog, see RenderForm). */
bool g_dirbrowse_open = false;
std::string g_dirbrowse_dir;
#endif

/* Last snapshot stage (edge detection for done/canceled/error, avoids
 * duplicate popups). */
int g_last_stage = STAGE_IDLE;
bool g_started = false;   /* whether a task started this session (enables
                           * edge detection) */

/* Pending task (executed after the file-exists choice). */
struct Pending {
    bool active = false;
    std::string url, path;
    int threads = 1;
    int exist_choice = 0;  /* 0 waiting; 1 Resume; 2 Overwrite; 3 Rename;
                            * 4 Cancel */
} g_pending;

/* Log auto-scroll */
bool g_log_autoscroll = true;

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

/* ---- Form rendering ---- */
void RenderForm(DownloadWorker& worker) {
    bool running = worker.IsRunning();

    /* Mode toggle (F1): the URL placeholder follows the mode. */
    bool toggled = ToggleMode(i18n::T("mode.file"), i18n::T("mode.video"),
                              g_video_mode);

    /* URL input (placeholder follows the mode). */
    ImGui::SetNextItemWidth(-1.0f);
    if (toggled) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextWithHint(
        "##url", g_video_mode ? i18n::T("placeholder.url.video")
                              : i18n::T("placeholder.url.file"),
        g_url, sizeof(g_url),
        running ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);

    /* Save path + browse (Windows native GetSaveFileName).
     * Video-mode path semantics = save directory (the output name is derived
     * from the URL plus a timestamp). */
    ImGui::SetNextItemWidth(-70.0f);
    ImGui::InputTextWithHint(
        "##path", g_video_mode ? i18n::T("placeholder.path.video")
                               : i18n::T("placeholder.path.file"),
        g_path, sizeof(g_path),
        running ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
#ifdef _WIN32
    ImGui::SameLine();
    if (ImGui::Button(i18n::T("button.browse"), ImVec2(60, 0)) && !running) {
        /* Folder picker (IFileDialog FOS_PICKFOLDERS): returns a directory;
         * the file name is appended from the URL. */
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
#else
    ImGui::SameLine();
    if (ImGui::Button(i18n::T("button.browse"), ImVec2(60, 0)) && !running) {
        /* Linux built-in directory browser (zero external deps, see
         * dirbrowser.h). */
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
#endif

    /* Threads: combo 1..kHardwareMax (the cap adapts 4~8 to CPU cores). */
    ImGui::Text("%s:", i18n::T("label.threads"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    char items[128] = {0};
    int off = 0;
    for (int i = 1; i <= kHardwareMax && off < (int)sizeof(items) - 2; i++) {
        off += snprintf(items + off, sizeof(items) - off, "%d%c", i, '\0');
    }
    int idx = (g_threads >= 1 && g_threads <= kHardwareMax) ? g_threads - 1 : 0;
    if (ImGui::Combo("##threads", &idx, items, kHardwareMax)) {
        g_threads = idx + 1;
    }
    ImGui::SameLine();
    {
        char buf[128];
        snprintf(buf, sizeof(buf), i18n::T("hint.threads"), kHardwareMax);
        ImGui::TextDisabled("%s", buf);
    }

    /* Download / cancel buttons (F9/F10) + pause/resume/stop state machine:
     *   IDLE:    [Start Download]        [Cancel(disabled)]
     *   RUNNING: [Downloading...(disabled)] [Cancel = pause intent]
     *   PAUSED:  [Resume]  [Stop(red, deletes cache, refreshes)] */
    ImGui::Separator();
    float avail = ImGui::GetContentRegionAvail().x;
    if (g_paused) {
        /* Paused: resume / stop. */
        if (ImGui::Button(i18n::T("button.resume"),
                          ImVec2(avail * 0.5f - 4.0f, 0))) {
            OnStartClicked(worker); /* resume branch (resume when paused) */
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0xC0, 0x3A, 0x3A, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              IM_COL32(0xD9, 0x4A, 0x4A, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              IM_COL32(0xA0, 0x2E, 0x2E, 255));
        if (ImGui::Button(i18n::T("button.stop"),
                          ImVec2(avail * 0.5f - 4.0f, 0))) {
            StopAndClear(worker);
        }
        ImGui::PopStyleColor(3);
    } else {
        ImGui::BeginDisabled(running || g_pending.active);
        if (ImGui::Button(running ? i18n::T("button.downloading")
                                  : i18n::T("button.download"),
                          ImVec2(avail * 0.5f - 4.0f, 0))) {
            OnStartClicked(worker);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!running);
        if (ImGui::Button(i18n::T("button.cancel"),
                          ImVec2(avail * 0.5f - 4.0f, 0))) {
            worker.Cancel();  /* first cancel = pause intent (cache kept) */
            /* Issue R10: do not optimistically enter the paused state; the
             * stage edge decides pause (download cancel) vs stop (parse/
             * merge cancel) once the worker actually stops. */
            worker.AddLog("[INFO] cancel requested, stopping...");
        }
        ImGui::EndDisabled();
    }
}

void OnStartClicked(DownloadWorker& worker) {
    std::string url(g_url);
    std::string path(g_path);

    /* Clicked while paused -> resume: skip the existence check and the video
     * rename; video mode must reuse the original basename (g_last_path) so
     * the same file is resumed. */
    if (g_paused) {
        if (url.empty() || !UrlSchemeOk(url)) {
            ShowErrorPopup(i18n::T("dialog.error.title"),
                           i18n::T("err.url.invalid"));
            return;
        }
        if (g_last_video) {
            StartVideoDownload(worker, url, g_last_path, g_threads,
                               true /* resume: keep progress snapshot */);
        } else {
            if (path.empty()) {
                ShowErrorPopup(i18n::T("dialog.error.title"),
                               i18n::T("err.path.empty"));
                return;
            }
            /* Directory -> append the file name (same name when the URL is
             * unchanged; local residue resumes). */
            if (IsDirectoryPath(path)) {
                std::string name = UrlFileName(url);
                if (name.empty()) {
                    name = CurrentTimeStamp() + ".download";
                }
                path = JoinPath(path, name);
            }
            StartDownload(worker, url, path, g_threads,
                          true /* resume: keep progress snapshot */);
        }
        g_paused = false;
        return;
    }

    /* Decode thunder:// links to real URLs. */
    if (url.rfind("thunder://", 0) == 0) {
        std::string decoded = ThunderDecode(url);
        if (decoded.empty() || !UrlSchemeOk(decoded)) {
            ShowErrorPopup(i18n::T("dialog.error.title"),
                           i18n::T("err.thunder.invalid"));
            return;
        }
        worker.AddLog("[INFO] detected a thunder link, decoded to: " +
                      decoded);
        snprintf(g_url, sizeof(g_url), "%s", decoded.c_str());
        url = decoded;
    }

    /* URL pre-check (http/https). */
    if (url.empty() || !UrlSchemeOk(url)) {
        ShowErrorPopup(i18n::T("dialog.error.title"),
                       i18n::T("err.url.invalid"));
        return;
    }
    /* Video mode: the path is a save directory; the output base name is
     * directory + URL name + timestamp (timestamp naming prevents
     * overwrites, same semantics as the CLI without -o). */
    if (g_video_mode) {
        if (path.empty()) {
            ShowErrorPopup(i18n::T("dialog.error.title"),
                           i18n::T("err.path.empty"));
            return;
        }
        std::string base = UrlFileName(url);
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) {
            base = base.substr(0, dot);  /* base name without extension */
        }
        if (base.empty()) {
            base = "video";
        }
        std::string basename =
            JoinPath(path, base + "_" + CurrentTimeStamp());
        StartVideoDownload(worker, url, basename, g_threads);
        return;
    }
    if (path.empty()) {
        ShowErrorPopup(i18n::T("dialog.error.title"),
                       i18n::T("err.path.empty"));
        return;
    }

    /* When the save path is a directory, append the URL-derived file name
     * (the user only needs to pick a folder). */
    if (IsDirectoryPath(path)) {
        std::string name = UrlFileName(url);
        if (name.empty()) {
            name = CurrentTimeStamp() + ".download";  /* timestamp fallback */
        }
        path = JoinPath(path, name);
    }

    /* File exists (F11): show the four-choice dialog, then start. */
    if (PathExists(path)) {
        g_pending.active = true;
        g_pending.url = url;
        g_pending.path = path;
        g_pending.threads = g_threads;
        g_pending.exist_choice = 0;
        g_exists_open = true;
        return;
    }
    StartDownload(worker, url, path, g_threads);
}

void StartDownload(DownloadWorker& worker, const std::string& url,
                   const std::string& path, int threads,
                   bool preserve_snapshot) {
    worker.AddLog(std::string("[INFO] URL: ") + url);
    worker.AddLog(std::string("[INFO] saving to: ") + path);
    if (preserve_snapshot) {
        worker.AddLog("[INFO] resuming from the existing download progress");
    }
    g_last_path = path;
    g_last_video = false;
    g_last_stage = STAGE_IDLE;
    if (!worker.StartFileDownload(url, path, threads, 60,
                                  preserve_snapshot)) {
        ShowErrorPopup(i18n::T("dialog.error.title"), i18n::T("err.busy"));
    }
}

void StartVideoDownload(DownloadWorker& worker, const std::string& url,
                        const std::string& basename, int threads,
                        bool preserve_snapshot) {
    worker.AddLog(std::string("[INFO] video URL: ") + url);
    worker.AddLog(std::string("[INFO] output base name: ") + basename);
    if (preserve_snapshot) {
        worker.AddLog("[INFO] resuming from the existing download progress");
    }
    g_last_path = basename;
    g_last_video = true;
    g_last_stage = STAGE_IDLE;
    if (!worker.StartVideoDownload(url, basename, threads, 60,
                                   preserve_snapshot)) {
        ShowErrorPopup(i18n::T("dialog.error.title"), i18n::T("err.busy"));
    }
}

/** Delete download cache files (used by Stop): file mode = the target file;
 *  video mode = the basename's audio/video tracks and merged output. */
void RemoveDownloadArtifacts(const std::string& base, bool video) {
    if (base.empty()) {
        return;
    }
    if (video) {
        const char* exts[] = {".mp4", ".m4a", ".mkv", ".webm"};
        for (const char* e : exts) {
            RemoveFile(base + e);
            RemoveFile(base + "_full" + e);
            RemoveFile(base + e + ".curlbolt.part");
        }
    } else {
        RemoveFile(base);
        RemoveFile(base + ".curlbolt.part");
    }
}

/** Stop the task (red button): delete cache files, reset the worker and
 *  refresh the UI to its initial state. */
void StopAndClear(DownloadWorker& worker) {
    worker.AddLog("[INFO] task stopped, deleted cache files: " + g_last_path);
    RemoveDownloadArtifacts(g_last_path, g_last_video);
    /* Reset worker cache and snapshot/log (worker is idle now). */
    worker.Reset();
    /* Refresh the UI to its initial state: clear form, progress and pause. */
    g_paused = false;
    g_last_path.clear();
    g_last_video = false;
    g_started = false;
    g_last_stage = STAGE_IDLE;
    g_url[0] = '\0';
    g_path[0] = '\0';
    g_threads = kHardwareMax;
    worker.AddLog("[INFO] task cleared, ready to start a new download");
}

/* ---- Progress area rendering (F5/F6/F8) ---- */
void RenderProgress(const DownloadSnapshot& snap) {
    ImGui::Separator();
    ImGui::Text("%s", i18n::T("label.total"));
    /* Total progress bar (F6): 3D cylinder style - track/fill are
     * capsule-shaped (radius = height/2) with a vertical gradient (top bright
     * -> bottom dark, simulated with AddRectFilledMultiColor).  Chunk effect:
     * full fill in green, dark separators between chunks, dark track for
     * unwritten areas.  The fill corners hug the track edges exactly (first
     * chunk left-rounded, last chunk right-rounded, single chunk fully
     * rounded). */
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float bar_w = ImGui::GetContentRegionAvail().x;
        const float bar_h = 30.0f; /* 1.5x wide, fits per-chunk text */
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float radius = bar_h * 0.5f; /* capsule corner = cylinder end */
        /* Cylinder: uniform color + top highlight + bottom shadow (layered
         * draws, the gradient API has no rounded corners). */
        const ImU32 bg_mid = IM_COL32(0x3A, 0x3A, 0x3A, 255);
        const ImU32 gn_mid = IM_COL32(0x5E, 0xA8, 0x4E, 255); /* green fill */
        const ImU32 gn_hi  = IM_COL32(255, 255, 255, 40);     /* highlight */
        const ImU32 gn_lo  = IM_COL32(0, 0, 0, 52);           /* shadow */
        const ImU32 sep = IM_COL32(0x0A, 0x0A, 0x0A, 230);    /* separator */
        const ImU32 border = IM_COL32(0x6A, 0x6A, 0x6A, 255);
        /* Track (capsule + highlight/shadow). */
        draw->AddRectFilled(pos, ImVec2(pos.x + bar_w, pos.y + bar_h),
                            bg_mid, radius);
        draw->AddRectFilled(pos, ImVec2(pos.x + bar_w, pos.y + bar_h * 0.32f),
                            IM_COL32(255, 255, 255, 16), radius,
                            ImDrawFlags_RoundCornersAll);
        draw->AddRectFilled(ImVec2(pos.x, pos.y + bar_h * 0.8f),
                            ImVec2(pos.x + bar_w, pos.y + bar_h),
                            IM_COL32(0, 0, 0, 40), radius,
                            ImDrawFlags_RoundCornersAll);
        if (!snap.threads.empty() && snap.threads[0].file_total > 0) {
            const double ft = (double)snap.threads[0].file_total;
            for (int i = 0; i < (int)snap.threads.size(); i++) {
                const auto& t = snap.threads[i];
                float s = (float)((double)t.file_start / ft);
                float e = (float)((double)t.total / ft);
                float cur = (float)((double)t.downloaded / ft);
                if (s > 1.0f) s = 1.0f;
                if (e > 1.0f) e = 1.0f;
                if (cur > e) cur = e;
                /* Chunk completion (for in-chunk text + hover tooltip). */
                double seg_done = (double)(t.downloaded - t.file_start);
                double seg_total = (double)(t.total - t.file_start);
                int seg_pct = (seg_total > 0)
                                  ? (int)(seg_done / seg_total * 100.0)
                                  : 0;
                if (seg_pct < 0) seg_pct = 0;
                if (seg_pct > 100) seg_pct = 100;
                /* Hover: show this chunk's progress + speed. */
                {
                    const ImVec2 m = ImGui::GetIO().MousePos;
                    if (m.x >= pos.x + bar_w * s && m.x <= pos.x + bar_w * e &&
                        m.y >= pos.y && m.y <= pos.y + bar_h) {
                        char tip[160];
                        snprintf(tip, sizeof(tip),
                                 "%s #%d | %d%% | %.2f MB/s",
                                 i18n::T("label.thread"), t.id, seg_pct,
                                 t.speed / (1024.0 * 1024.0));
                        ImGui::SetTooltip("%s", tip);
                    }
                }
                if (t.downloaded <= t.file_start) {
                    continue; /* not started: keep the dark track */
                }
                /* Corner rounding only for the physical first/last chunk
                 * (first left-rounded, last right-rounded, single chunk fully
                 * rounded); middle chunks use RoundCornersNone for sharp
                 * corners - flags=0 in ImGui means all four corners rounded!
                 * (Previously middle chunks with flags=0 became capsule
                 * shaped and left track gaps between them.) */
                const int nseg = (int)snap.threads.size();
                ImDrawFlags flags = ImDrawFlags_RoundCornersNone;
                if (nseg == 1) {
                    flags = ImDrawFlags_RoundCornersAll;
                } else if (i == 0) {
                    flags = ImDrawFlags_RoundCornersLeft;
                } else if (i == nseg - 1) {
                    flags = ImDrawFlags_RoundCornersRight;
                }
                /* Green fill (solid sharp-corner bar; the highlight/shadow
                 * is drawn as one capsule below). */
                draw->AddRectFilled(
                    ImVec2(pos.x + bar_w * s, pos.y),
                    ImVec2(pos.x + bar_w * cur, pos.y + bar_h), gn_mid,
                    radius, flags);
                /* In-chunk text: chunk percent (shown when the cell is wide
                 * enough). */
                if ((e - s) * bar_w > 44.0f) {
                    char seg_txt[16];
                    snprintf(seg_txt, sizeof(seg_txt), "%d%%", seg_pct);
                    ImVec2 ts = ImGui::CalcTextSize(seg_txt);
                    ImVec2 tp(pos.x + bar_w * (s + e) * 0.5f - ts.x * 0.5f,
                              pos.y + (bar_h - ts.y) * 0.5f);
                    draw->AddText(tp, IM_COL32(255, 255, 255, 235), seg_txt);
                }
                /* Downloaded boundary inside a chunk (dark thin line,
                 * distinct from the battery-cell separators). */
                if (cur > s && cur < e - 0.001f) {
                    draw->AddLine(ImVec2(pos.x + bar_w * cur, pos.y + 2.0f),
                                  ImVec2(pos.x + bar_w * cur,
                                         pos.y + bar_h - 2.0f),
                                  IM_COL32(0, 0, 0, 90), 1.0f);
                }
            }
        } else if (snap.totalPercent > 0.0) {
            /* No chunk data (parse/merge stage): fill by total progress
             * (left-rounded). */
            float cur = (float)(snap.totalPercent / 100.0);
            if (cur > 1.0f) cur = 1.0f;
            ImDrawFlags f2 = cur >= 0.999f ? ImDrawFlags_RoundCornersAll
                                            : ImDrawFlags_RoundCornersLeft;
            draw->AddRectFilled(pos, ImVec2(pos.x + bar_w * cur, pos.y + bar_h),
                                gn_mid, radius, f2);
        }
        /* Overall cylinder highlight/shadow (one capsule across all chunks,
         * matching the end-face arcs; replaces per-chunk highlights). */
        draw->AddRectFilled(
            pos, ImVec2(pos.x + bar_w, pos.y + bar_h * 0.35f), gn_hi,
            radius, ImDrawFlags_RoundCornersAll);
        draw->AddRectFilled(
            ImVec2(pos.x, pos.y + bar_h * 0.72f),
            ImVec2(pos.x + bar_w, pos.y + bar_h), gn_lo, radius,
            ImDrawFlags_RoundCornersAll);
        /* Battery-cell separators: 5px dark vertical bands (2-3x wider,
         * user request), drawn last so the fill never covers them; they show
         * even with no download data. */
        if (!snap.threads.empty() && snap.threads[0].file_total > 0) {
            const double ft = (double)snap.threads[0].file_total;
            for (const auto& t : snap.threads) {
                float s = (float)((double)t.file_start / ft);
                if (s > 0.002f && s < 0.998f) {
                    draw->AddRectFilled(
                        ImVec2(pos.x + bar_w * s - 2.5f, pos.y + 1.0f),
                        ImVec2(pos.x + bar_w * s + 2.5f,
                               pos.y + bar_h - 1.0f),
                        sep);
                }
            }
        }
        draw->AddRect(pos, ImVec2(pos.x + bar_w, pos.y + bar_h), border,
                      radius);
        ImGui::Dummy(ImVec2(bar_w, bar_h));
    }

    /* Bottom-right of the total bar: overall percent + speed. */
    {
        char info[128];
        if (snap.totalSpeed > 0) {
            snprintf(info, sizeof(info), "%.1f%%  |  %.2f MB/s",
                     snap.totalPercent,
                     snap.totalSpeed / (1024.0 * 1024.0));
        } else {
            snprintf(info, sizeof(info), "%.1f%%", snap.totalPercent);
        }
        float txt_w = ImGui::CalcTextSize(info).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             ImGui::GetContentRegionAvail().x - txt_w);
        ImGui::Text("%s", info);
    }

    const char* stage_txt = i18n::T("stage.idle");
    switch (snap.stage) {
        case STAGE_DOWNLOADING: stage_txt = i18n::T("stage.downloading"); break;
        case STAGE_PARSING:     stage_txt = i18n::T("stage.parsing"); break;
        case STAGE_VIDEO_DL:    stage_txt = i18n::T("stage.video"); break;
        case STAGE_AUDIO_DL:    stage_txt = i18n::T("stage.audio"); break;
        case STAGE_MERGING:     stage_txt = i18n::T("stage.merging"); break;
        case STAGE_DONE:        stage_txt = i18n::T("stage.done"); break;
        case STAGE_CANCELED:
            /* Paused (first cancel) shows "Paused", otherwise "Canceled". */
            stage_txt = g_paused ? i18n::T("stage.paused")
                                 : i18n::T("stage.canceled");
            break;
        case STAGE_ERROR:       stage_txt = i18n::T("stage.error"); break;
        default: break;
    }
    ImGui::Text("%s: %s", i18n::T("label.status"), stage_txt);
}

/* ---- Log area (F7) ---- */
void RenderLog(const std::vector<std::string>& log) {
    ImGui::Separator();
    ImGui::Text("%s", i18n::T("label.log"));
    ImGui::SameLine();
    if (ImGui::SmallButton(i18n::T("log.copy"))) {
        std::string all;
        for (const auto& line : log) {
            all += line;
            all += "\n";
        }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::BeginChild("##log", ImVec2(0, 160), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
        g_log_autoscroll = true;
    }
    for (const auto& line : log) {
        /* No wrapping: a horizontal scrollbar shows when lines are wide. */
        ImGui::TextUnformatted(line.c_str());
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

/* ---- Custom title bar (borderless window: settings + title + Mac-style
 * buttons + drag) ---- */
void RenderTitleBar() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, kTitleBarH),
                             ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
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

    /* Title text (the settings entry moved to the main window's Settings
     * menu bar, see Render()). */
    ImGui::SetCursorPos(
        ImVec2(12, (kTitleBarH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::Text("%s", i18n::T("window.title"));

    /* Right side: Mac-style three-color buttons (red=close at the far right,
     * green=maximize, yellow=minimize). */
    const float btn_d = 14.0f;
    const float gap = 12.0f;
    const float margin = 18.0f; /* distance from the right edge */
    /* Red (close) */
    float rx = io.DisplaySize.x - margin - btn_d * 0.5f;
    /* Green (maximize) */
    float gx = rx - btn_d - gap;
    /* Yellow (minimize) */
    float yx = gx - btn_d - gap;
    const ImU32 cRed = IM_COL32(0xE0, 0x6C, 0x75, 255);
    const ImU32 cRedH = IM_COL32(0xF0, 0x8A, 0x92, 255);
    const ImU32 cYellow = IM_COL32(0xE5, 0xC0, 0x7B, 255);
    const ImU32 cYellowH = IM_COL32(0xF2, 0xD3, 0x9A, 255);
    const ImU32 cGreen = IM_COL32(0x98, 0xC3, 0x79, 255);
    const ImU32 cGreenH = IM_COL32(0xB2, 0xD5, 0x97, 255);

    if (MacCircleButton(yx, cy, btn_d, cYellow, cYellowH,
                        i18n::T("button.minimize")) &&
        g_window != nullptr) {
        glfwIconifyWindow(g_window);
    }
    if (MacCircleButton(gx, cy, btn_d, cGreen, cGreenH,
                        i18n::T("button.maximize")) &&
        g_window != nullptr) {
        if (glfwGetWindowAttrib(g_window, GLFW_MAXIMIZED) == GLFW_TRUE) {
            glfwRestoreWindow(g_window);
        } else {
            glfwMaximizeWindow(g_window);
        }
    }
    if (MacCircleButton(rx, cy, btn_d, cRed, cRedH, i18n::T("button.close")) &&
        g_window != nullptr) {
        glfwSetWindowShouldClose(g_window, GLFW_TRUE);
    }

    /* Title bar drag: on press, trigger native Windows window dragging
     * (WM_NCLBUTTONDOWN/HTCAPTION) so the system moves the window smoothly;
     * non-Windows falls back to manual position updates. */
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##titlebar_drag",
                           ImVec2(io.DisplaySize.x - btn_d * 3 - gap * 2 -
                                      margin * 2 - 8,
                                  kTitleBarH));
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
        static bool dragging = false;
        static int drag_x0 = 0, drag_y0 = 0;
        static ImVec2 drag_mouse0;
        if (!dragging) {
            dragging = true;
            glfwGetWindowPos(g_window, &drag_x0, &drag_y0);
            drag_mouse0 = io.MousePos;
        }
        if (dragging) {
            glfwSetWindowPos(g_window,
                             drag_x0 + (int)(io.MousePos.x - drag_mouse0.x),
                             drag_y0 + (int)(io.MousePos.y - drag_mouse0.y));
        }
        if (!ImGui::IsItemActive()) {
            dragging = false;
        }
#endif
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
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
        if (nh < 480) {
            nh = 480;
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
    ImU32 col = ImGui::GetColorU32(dir == BR ? ImGuiCol_SeparatorActive
                                             : ImGuiCol_SeparatorHovered);
    dl->AddTriangleFilled(ImVec2(c.x - 12, c.y), c, ImVec2(c.x, c.y - 12), col);
}

}  // namespace

void Init(GLFWwindow* window) {
    g_window = window;
}

bool Render(DownloadWorker& worker) {
#ifdef _WIN32
    /* Custom title bar (Windows borderless window; Linux uses the system
     * title bar). */
    RenderTitleBar();
#endif

    /* Main window: fills the client area below the title bar; includes a
     * Settings menu bar (language switching). */
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, kTitleBarH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(io.DisplaySize.x, io.DisplaySize.y - kTitleBarH),
        ImGuiCond_Always);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings);

    /* Settings menu bar: the language entry shows the TARGET language hint
     * (a Chinese UI -> "language"; an English UI -> "中文", see
     * menu.lang_hint); opening it shows the regular language choices. */
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(i18n::T("menu.lang_hint"))) {
            bool zh = (i18n::GetLang() == i18n::Lang::Zh);
            bool en = !zh;
            if (ImGui::MenuItem(i18n::T("lang.zh"), nullptr, zh)) {
                i18n::SetLang(i18n::Lang::Zh);
            }
            if (ImGui::MenuItem(i18n::T("lang.en"), nullptr, en)) {
                i18n::SetLang(i18n::Lang::En);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(i18n::T("menu.about"))) {
            g_about_open = true;
        }
        ImGui::EndMenuBar();
    }

    RenderForm(worker);

    /* Snapshot read (once per frame, copied under lock). */
    DownloadSnapshot snap;
    int stage = worker.GetSnapshot(snap);

    /* Stage edge -> popups (done F13 / error F12) and pause confirmation.
     * CANCELED = user pause (cache kept, resume/stop available);
     * DONE/ERROR exit the paused state. */
    if (g_started && stage != g_last_stage) {
        const int prev_stage = g_last_stage;
        g_last_stage = stage;
        if (stage == STAGE_DONE) {
            g_done_path = snap.status;  /* completion path is in the log */
            g_done_open = true;
            g_paused = false;
        } else if (stage == STAGE_ERROR) {
            ShowErrorPopup(i18n::T("dialog.error.title"), snap.error,
                           ErrorGuide(snap.error), g_last_path);
            g_paused = false;
        } else if (stage == STAGE_CANCELED) {
            /* Issue R10: a cancel during download is a real pause (cache
             * kept, resume possible); a cancel during parse/merge simply
             * stops the task. */
            if ((prev_stage == STAGE_DOWNLOADING) ||
                (prev_stage == STAGE_VIDEO_DL) ||
                (prev_stage == STAGE_AUDIO_DL)) {
                g_paused = true;
                worker.AddLog("[INFO] paused (cache kept): click Resume to "
                              "continue or Stop to delete the cache");
            } else {
                g_paused = false;
                worker.AddLog("[INFO] task stopped");
            }
        }
    }
    g_started = worker.IsRunning() || g_started;

    RenderProgress(snap);
    RenderLog(snap.log);

#ifdef _WIN32
    /* Bottom-right resize grip (Windows borderless window; draw before the
     * window End). */
    RenderResizeGrip();
#endif

    ImGui::End();

    /* File-exists four-choice (F11) handling. */
    if (g_pending.active && g_exists_open) {
        dialogs::ExistsChoice c = dialogs::ShowFileExists(g_pending.path,
                                                          g_exists_open);
        if (c != dialogs::ExistsChoice::None) {
            switch (c) {
                case dialogs::ExistsChoice::Resume:
                    StartDownload(worker, g_pending.url, g_pending.path,
                                  g_pending.threads);
                    break;
                case dialogs::ExistsChoice::Overwrite:
                    RemoveFile(g_pending.path);
                    worker.AddLog("[INFO] old file deleted (overwrite): " +
                                  g_pending.path);
                    StartDownload(worker, g_pending.url, g_pending.path,
                                  g_pending.threads);
                    break;
                case dialogs::ExistsChoice::Rename: {
                    std::string newpath = StampName(g_pending.path);
                    worker.AddLog("[INFO] renamed to: " + newpath);
                    StartDownload(worker, g_pending.url, newpath,
                                  g_pending.threads);
                    break;
                }
                default:  /* Cancel */
                    worker.AddLog("[INFO] canceled (file-exists dialog)");
                    break;
            }
            g_pending.active = false;
        }
    }

    /* Error popup (F12). */
    dialogs::ShowError(g_error_title, g_error_msg, g_error_guide,
                       g_error_open, g_error_partial_path,
                       &g_error_delete_requested);
    if (g_error_delete_requested) {
        g_error_delete_requested = false;
        if (!g_error_partial_path.empty()) {
            RemoveDownloadArtifacts(g_error_partial_path, g_last_video);
            worker.AddLog("[INFO] partial file deleted: " +
                          g_error_partial_path);
        }
        g_error_partial_path.clear();
    }

    /* Done popup (F13). */
    dialogs::ShowDone(g_done_path, g_done_open);

    /* About popup. */
    dialogs::ShowAbout(BURST_VERSION_STRING, g_about_open);

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
