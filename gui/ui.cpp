/**
 * @file ui.cpp
 * @brief 主界面渲染实现（见 ui.h）
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

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>  /* IFileDialog 目录选择 */
#else
#include <sys/stat.h>
#endif

namespace ui {

namespace {

/* 自绘标题栏高度（无边框窗口，替代系统标题栏） */
const float kTitleBarH = 36.0f;

/* GLFW 窗口指针（ui::Init 注入，标题栏按钮/拖动/resize 用） */
GLFWwindow* g_window = nullptr;

/* ---- 表单状态（UI 主线程独占，工作线程不触碰） ---- */
/* 本机可用线程上限：min(10, hardware_concurrency())（F4） */
const int kHardwareMax =
    std::min(MaxThread, (int)std::thread::hardware_concurrency());

char g_url[2048] = {0};
char g_path[2048] = {0};
int g_threads = kHardwareMax;   /* 默认 = 本机上限 */
bool g_video_mode = false;

/* 下载控制状态机（F9/F10）：IDLE → RUNNING → PAUSED → IDLE
 *  - 下载中第一次点"取消" = 暂停意图（中止传输，保留缓存文件，可断点续传）
 *  - 暂停后按钮1 = "继续"（续传），按钮2 = "停止"（红色，删除缓存并刷新 UI）
 *  - g_paused=true 表示处于暂停态；g_last_path/g_last_video 供停止时删除缓存用 */
bool g_paused = false;
std::string g_last_path;   /* 最近任务目标路径/基础名（停止时删除缓存） */
bool g_last_video = false; /* 最近任务是否视频模式 */

/* 前向声明（RenderForm 在 OnStartClicked/StartDownload 之前定义） */
void OnStartClicked(DownloadWorker& worker);
void StartDownload(DownloadWorker& worker, const std::string& url,
                   const std::string& path, int threads,
                   bool preserve_snapshot = false);
void StartVideoDownload(DownloadWorker& worker, const std::string& url,
                        const std::string& basename, int threads,
                        bool preserve_snapshot = false);
void StopAndClear(DownloadWorker& worker);

/* 弹窗状态 */
bool g_exists_open = false;
bool g_error_open = false;
bool g_done_open = false;
std::string g_error_title, g_error_msg, g_error_guide;
std::string g_done_path;

/* 上次快照 stage（检测完成/取消/错误的边沿，避免重复弹窗） */
int g_last_stage = STAGE_IDLE;
bool g_started = false;   /* 本会话是否启动过任务（边沿检测使能） */

/* 待启动任务（文件已存在弹窗选择后执行） */
struct Pending {
    bool active = false;
    std::string url, path;
    int threads = 1;
    int exist_choice = 0;  /* 0 等待选择；1 Resume；2 Overwrite；3 Rename；4 Cancel */
} g_pending;

/* 日志自动滚动 */
bool g_log_autoscroll = true;

#ifdef _WIN32
/** UTF-16 → UTF-8 */
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0,
                                NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL,
                        NULL);
    return s;
}

/** UTF-8 → UTF-16 */
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
#endif

/** URL scheme 预检（R11）：http/https */
bool UrlSchemeOk(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

/** Base64 解码（迅雷链接解码用，不含第三方库）
 * 算法：每 4 个 base64 字符 → 3 字节（逐组移位，尾部不足 4 字符按 1~2 字节处理） */
std::string Base64Decode(const std::string& in) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int buf[4] = {0, 0, 0, 0};
    int n = 0;
    for (char c : in) {
        if (c == '=') {
            break;  /* padding：结束 */
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
    /* 尾部不足 4 字符（剩 2 字符 → 1 字节；剩 3 字符 → 2 字节） */
    if (n == 2) {
        out.push_back((char)((buf[0] << 2) | (buf[1] >> 4)));
    } else if (n == 3) {
        out.push_back((char)((buf[0] << 2) | (buf[1] >> 4)));
        out.push_back((char)(((buf[1] & 0xF) << 4) | (buf[2] >> 2)));
    }
    return out;
}

/**
 * @brief 迅雷专用链接解码：thunder:// + Base64("AA" + 真实URL + "ZZ")
 * @return 解码后的真实 URL；非 thunder:// 前缀则原样返回
 */
std::string ThunderDecode(const std::string& url) {
    const char* prefix = "thunder://";
    if (url.rfind(prefix, 0) != 0) {
        return url;
    }
    std::string dec = Base64Decode(url.substr(strlen(prefix)));
    /* 去掉头 2 字节 "AA" 与尾 2 字节 "ZZ" */
    if (dec.size() >= 4) {
        dec = dec.substr(2, dec.size() - 4);
    }
    return dec;
}

/** 目标路径是否存在（F11 触发条件） */
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

/** 删除本地文件（覆盖选择用） */
void RemoveFile(const std::string& path) {
#ifdef _WIN32
    DeleteFileW(Utf8ToWide(path).c_str());
#else
    remove(path.c_str());
#endif
}

/** 当前时间戳（YYYYMMDD_HHMMSS，改名用） */
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

/** 从 URL 推断文件名：去查询串/片段/尾部斜杠，取最后路径段（如 …/node.tar.gz → node.tar.gz） */
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
    return (slash != std::string::npos) ? u.substr(slash + 1) : u;
}

/** 路径是否应视为"目录"：已存在且是目录，或以分隔符结尾 */
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

/** 拼接目录与文件名（补平台分隔符） */
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

/** 时间戳改名：file.zip -> file_20260807_123000.zip（F11 改名语义，与 CLI 一致） */
std::string StampName(const std::string& path) {
    std::string base = path;
    std::string ts = CurrentTimeStamp();
    size_t dot = base.find_last_of('.');
    size_t slash = base.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return base.substr(0, dot) + "_" + ts + base.substr(dot);
    }
    return base + "_" + ts;
}

/** 触发错误弹窗（F12） */
void ShowErrorPopup(const std::string& title, const std::string& msg,
                    const std::string& guide = "") {
    g_error_title = title;
    g_error_msg = msg;
    g_error_guide = guide;
    g_error_open = true;
}

/** 按 §8.3 表格对错误分类并给出指引 */
std::string ErrorGuide(const std::string& err) {
    if (err.find("解析失败") != std::string::npos) {
        return i18n::T("err.guide.parse");
    }
    if (err.find("合并失败") != std::string::npos) {
        return i18n::T("err.guide.merge");
    }
    if (err.find("初始化失败") != std::string::npos) {
        return i18n::T("err.guide.init");
    }
    return i18n::T("err.guide.generic");
}

/* ---- 表单渲染 ---- */
void RenderForm(DownloadWorker& worker) {
    bool running = worker.IsRunning();

    /* 模式拨动开关（F1）：切换时 URL 占位联动 */
    bool toggled = ToggleMode(i18n::T("mode.file"), i18n::T("mode.video"),
                              g_video_mode);

    /* URL 输入（占位提示随模式联动） */
    ImGui::SetNextItemWidth(-1.0f);
    if (toggled) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextWithHint(
        "##url", g_video_mode ? i18n::T("placeholder.url.video")
                              : i18n::T("placeholder.url.file"),
        g_url, sizeof(g_url),
        running ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);

    /* 保存路径 + 浏览（Windows 原生 GetSaveFileName，§3）
     * 视频模式路径语义 = 保存目录（输出名由 URL 自动推断 + 时间戳防覆盖） */
    ImGui::SetNextItemWidth(-70.0f);
    ImGui::InputTextWithHint(
        "##path", g_video_mode ? i18n::T("placeholder.path.video")
                               : i18n::T("placeholder.path.file"),
        g_path, sizeof(g_path),
        running ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
#ifdef _WIN32
    ImGui::SameLine();
    if (ImGui::Button(i18n::T("button.browse"), ImVec2(60, 0)) && !running) {
        /* 目录选择对话框（IFileDialog FOS_PICKFOLDERS）：返回目录路径，文件名由 URL 自动拼接 */
        HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        IFileDialog* pfd = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&pfd));
        if (SUCCEEDED(hr)) {
            DWORD opts = 0;
            pfd->GetOptions(&opts);
            pfd->SetOptions(opts | FOS_PICKFOLDERS);
            pfd->SetTitle(L"选择保存目录");
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
#endif

    /* 线程数：下拉框选择 1..kHardwareMax（F4，上限 = min(10, 核数)） */
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

    /* 下载 / 取消按钮（F9/F10）+ 暂停/继续/停止状态机：
     *   IDLE:    [开始下载]         [取消(禁用)]
     *   RUNNING: [下载中…(禁用)]    [取消 = 暂停意图]
     *   PAUSED:  [继续(断点续传)]   [停止(红色, 删除缓存, 刷新初始)] */
    ImGui::Separator();
    float avail = ImGui::GetContentRegionAvail().x;
    if (g_paused) {
        /* 暂停态：继续 / 停止 */
        if (ImGui::Button(i18n::T("button.resume"),
                          ImVec2(avail * 0.5f - 4.0f, 0))) {
            OnStartClicked(worker); /* 继续分支（g_paused=true 时走续传） */
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
            worker.Cancel();  /* 第一次取消 = 暂停意图（保留缓存，可续传） */
            g_paused = true;  /* 乐观置位：按钮即切换为 继续/停止 */
            worker.AddLog("[INFO] 已请求暂停：可点击『继续』断点续传，"
                          "或『停止』删除缓存");
        }
        ImGui::EndDisabled();
    }
}

void OnStartClicked(DownloadWorker& worker) {
    std::string url(g_url);
    std::string path(g_path);

    /* 暂停态点击 → 继续（断点续传）：跳过已存在检测与视频重命名，
     * 视频模式必须沿用原 basename（g_last_path）才能续传原文件 */
    if (g_paused) {
        if (url.empty() || !UrlSchemeOk(url)) {
            ShowErrorPopup(i18n::T("dialog.error.title"),
                           i18n::T("err.url.invalid"));
            return;
        }
        if (g_last_video) {
            StartVideoDownload(worker, url, g_last_path, g_threads,
                               true /* 继续：保留进度快照 */);
        } else {
            if (path.empty()) {
                ShowErrorPopup(i18n::T("dialog.error.title"),
                               i18n::T("err.path.empty"));
                return;
            }
            /* 目录 → 拼文件名（URL 未变则同名，命中本地残留 → 断点续传） */
            if (IsDirectoryPath(path)) {
                std::string name = UrlFileName(url);
                if (name.empty()) {
                    name = CurrentTimeStamp() + ".download";
                }
                path = JoinPath(path, name);
            }
            StartDownload(worker, url, path, g_threads,
                          true /* 继续：保留进度快照 */);
        }
        g_paused = false;
        return;
    }

    /* 迅雷专用链接（thunder://）解码为真实 URL */
    if (url.rfind("thunder://", 0) == 0) {
        std::string decoded = ThunderDecode(url);
        if (decoded.empty() || !UrlSchemeOk(decoded)) {
            ShowErrorPopup(i18n::T("dialog.error.title"),
                           i18n::T("err.thunder.invalid"));
            return;
        }
        worker.AddLog("[INFO] 检测到迅雷链接，已解码为: " + decoded);
        snprintf(g_url, sizeof(g_url), "%s", decoded.c_str());
        url = decoded;
    }

    /* URL 预检（R11） */
    if (url.empty() || !UrlSchemeOk(url)) {
        ShowErrorPopup(i18n::T("dialog.error.title"),
                       i18n::T("err.url.invalid"));
        return;
    }
    /* 视频模式（Phase 2）：路径 = 保存目录，输出基础名 = 目录 + URL 名 + 时间戳
     * （时间戳命名天然防覆盖，与 CLI 未指定 -o 语义一致，无需 F11 四选一） */
    if (g_video_mode) {
        if (path.empty()) {
            ShowErrorPopup(i18n::T("dialog.error.title"),
                           i18n::T("err.path.empty"));
            return;
        }
        std::string base = UrlFileName(url); /* 去查询串/取最后路径段 */
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) {
            base = base.substr(0, dot); /* 基础名不带扩展名 */
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
        ShowErrorPopup(i18n::T("dialog.error.title"), i18n::T("err.path.empty"));
        return;
    }

    /* 保存路径若为目录：自动拼 URL 文件名+后缀（用户只需填目录，§4 增强） */
    if (IsDirectoryPath(path)) {
        std::string name = UrlFileName(url);
        if (name.empty()) {
            name = CurrentTimeStamp() + ".download";  /* URL 无文件名时用时间戳兜底 */
        }
        path = JoinPath(path, name);
    }

    /* 文件已存在（F11）：弹四选一，选择后启动 */
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
    worker.AddLog(std::string("[INFO] 保存到: ") + path);
    if (preserve_snapshot) {
        worker.AddLog("[INFO] 断点续传：从已下载进度继续");
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
    worker.AddLog(std::string("[INFO] 视频 URL: ") + url);
    worker.AddLog(std::string("[INFO] 输出基础名: ") + basename);
    if (preserve_snapshot) {
        worker.AddLog("[INFO] 断点续传：从已下载进度继续");
    }
    g_last_path = basename;
    g_last_video = true;
    g_last_stage = STAGE_IDLE;
    if (!worker.StartVideoDownload(url, basename, threads, 60,
                                   preserve_snapshot)) {
        ShowErrorPopup(i18n::T("dialog.error.title"), i18n::T("err.busy"));
    }
}

/** 删除下载缓存文件（停止任务用）：
 * 文件模式 = 目标文件；视频模式 = basename 的音视频轨与合并产物 */
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

/** 停止任务（红色按钮）：删除缓存文件、清理线程缓存、刷新 UI 至初始状态 */
void StopAndClear(DownloadWorker& worker) {
    worker.AddLog("[INFO] 已停止任务，删除缓存文件: " + g_last_path);
    RemoveDownloadArtifacts(g_last_path, g_last_video);
    /* 清理线程缓存与快照/日志（worker 已 idle） */
    worker.Reset();
    /* 刷新 UI 至初始状态：清空表单、进度、暂停态 */
    g_paused = false;
    g_last_path.clear();
    g_last_video = false;
    g_started = false;
    g_last_stage = STAGE_IDLE;
    g_url[0] = '\0';
    g_path[0] = '\0';
    g_threads = kHardwareMax;
    worker.AddLog("[INFO] 已清空任务，可重新开始下载");
}

/* ---- 进度区渲染（F5/F6/F8） ---- */
void RenderProgress(const DownloadSnapshot& snap) {
    ImGui::Separator();
    ImGui::Text("%s", i18n::T("label.total"));
    /* 总进度条（F6）：3D 圆柱体风格 —— 底槽/填充均为胶囊形（radius=高/2）+ 垂直渐变
     * （AddRectFilledMultiColor 顶部亮→底部暗，模拟圆柱曲面），分片效果：
     * 全部填充为绿色（用户需求，不再用蓝色），段间深色分隔线，未下载区留暗色底槽。
     * 填充圆角与底槽边缘完全贴合（首段左圆、末段右圆、单段全圆）。 */
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float bar_w = ImGui::GetContentRegionAvail().x;
        const float bar_h = 30.0f; /* 1.5 倍宽，容纳段内文字（用户需求） */
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float radius = bar_h * 0.5f; /* 胶囊圆角 = 圆柱端面 */
        /* 圆柱体：统一色 + 顶部高光(亮) + 底部阴影(暗)（1.93 渐变 API 无圆角，改用分层绘制） */
        const ImU32 bg_mid = IM_COL32(0x3A, 0x3A, 0x3A, 255);
        const ImU32 gn_mid = IM_COL32(0x5E, 0xA8, 0x4E, 255); /* 绿色填充（不再用蓝色） */
        const ImU32 gn_hi  = IM_COL32(255, 255, 255, 40);    /* 填充高光 */
        const ImU32 gn_lo  = IM_COL32(0, 0, 0, 52);          /* 填充阴影 */
        const ImU32 sep = IM_COL32(0x0A, 0x0A, 0x0A, 230);   /* 电池格分隔线（明显） */
        const ImU32 border = IM_COL32(0x6A, 0x6A, 0x6A, 255);
        /* 底槽（胶囊 + 高光/阴影） */
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
                /* 分片完成度（段内文字用 + hover 提示用） */
                double seg_done = (double)(t.downloaded - t.file_start);
                double seg_total = (double)(t.total - t.file_start);
                int seg_pct = (seg_total > 0)
                                  ? (int)(seg_done / seg_total * 100.0)
                                  : 0;
                if (seg_pct < 0) seg_pct = 0;
                if (seg_pct > 100) seg_pct = 100;
                /* hover：显示该分片 进度 + 速度 */
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
                    continue; /* 未开始分片：留暗色底槽（电池未充电格） */
                }
                /* 圆角只给物理首尾分片（首左圆/尾右圆/单片全圆），中间分片用
                 * RoundCornersNone 明确直角 —— flags=0 在 ImGui 中是"四角全圆"！
                 * （此前中间分片 flags=0 → 每片都是胶囊圆角，圆角间留出底槽间隙） */
                const int nseg = (int)snap.threads.size();
                ImDrawFlags flags = ImDrawFlags_RoundCornersNone;
                if (nseg == 1) {
                    flags = ImDrawFlags_RoundCornersAll;
                } else if (i == 0) {
                    flags = ImDrawFlags_RoundCornersLeft;
                } else if (i == nseg - 1) {
                    flags = ImDrawFlags_RoundCornersRight;
                }
                /* 绿色填充（纯色直角方柱；高光/阴影改为整体胶囊绘制，见下方） */
                draw->AddRectFilled(
                    ImVec2(pos.x + bar_w * s, pos.y),
                    ImVec2(pos.x + bar_w * cur, pos.y + bar_h), gn_mid,
                    radius, flags);
                /* 段内文字：分片完成度%（格宽足够时显示，电池格充电进度） */
                if ((e - s) * bar_w > 44.0f) {
                    char seg_txt[16];
                    snprintf(seg_txt, sizeof(seg_txt), "%d%%", seg_pct);
                    ImVec2 ts = ImGui::CalcTextSize(seg_txt);
                    ImVec2 tp(pos.x + bar_w * (s + e) * 0.5f - ts.x * 0.5f,
                              pos.y + (bar_h - ts.y) * 0.5f);
                    draw->AddText(tp, IM_COL32(255, 255, 255, 235), seg_txt);
                }
                /* 分片内已下载边界（深色细线，区别于电池格分隔线） */
                if (cur > s && cur < e - 0.001f) {
                    draw->AddLine(ImVec2(pos.x + bar_w * cur, pos.y + 2.0f),
                                  ImVec2(pos.x + bar_w * cur,
                                         pos.y + bar_h - 2.0f),
                                  IM_COL32(0, 0, 0, 90), 1.0f);
                }
            }
        } else if (snap.totalPercent > 0.0) {
            /* 无分片数据（解析/合并阶段）：按总进度绿色渐变填充（左圆右直角） */
            float cur = (float)(snap.totalPercent / 100.0);
            if (cur > 1.0f) cur = 1.0f;
            ImDrawFlags f2 = cur >= 0.999f ? ImDrawFlags_RoundCornersAll
                                            : ImDrawFlags_RoundCornersLeft;
            draw->AddRectFilled(pos, ImVec2(pos.x + bar_w * cur, pos.y + bar_h),
                                gn_mid, radius, f2);
        }
        /* 整体圆柱高光/阴影（胶囊形贯穿所有分片，贴合整体端面圆弧；
         * 替代分片级独立高光 → 不再出现"每分片圆角高光长方体"） */
        draw->AddRectFilled(
            pos, ImVec2(pos.x + bar_w, pos.y + bar_h * 0.35f), gn_hi,
            radius, ImDrawFlags_RoundCornersAll);
        draw->AddRectFilled(
            ImVec2(pos.x, pos.y + bar_h * 0.72f),
            ImVec2(pos.x + bar_w, pos.y + bar_h), gn_lo, radius,
            ImDrawFlags_RoundCornersAll);
        /* 电池格分隔线：5px 深色竖带（2-3 倍加宽，用户需求），最后画 →
         * 不被填充覆盖、格线始终清晰；无下载数据时格线也已显示（启动即加载好） */
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

    /* 总进度条右下方：总体百分比 + 总体下载速度（用户需求） */
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
            /* 暂停态（第一次取消）显示"已暂停"，否则"已取消" */
            stage_txt = g_paused ? i18n::T("stage.paused")
                                 : i18n::T("stage.canceled");
            break;
        case STAGE_ERROR:       stage_txt = i18n::T("stage.error"); break;
        default: break;
    }
    ImGui::Text("%s: %s", i18n::T("label.status"), stage_txt);
}

/* ---- 日志区（F7） ---- */
void RenderLog(const std::vector<std::string>& log) {
    ImGui::Separator();
    ImGui::Text("%s", i18n::T("label.log"));
    ImGui::BeginChild("##log", ImVec2(0, 160), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
        g_log_autoscroll = true;
    }
    for (const auto& line : log) {
        ImGui::TextUnformatted(line.c_str()); /* 不换行：内容超宽时显示水平滚动条 */
    }
    if (g_log_autoscroll && ImGui::GetScrollMaxY() > 0) {
        ImGui::SetScrollY(ImGui::GetScrollMaxY());
    }
    ImGui::EndChild();
}

/* ---- Mac 风格圆形窗口按钮（红=关闭/黄=最小化/绿=最大化，hover 提示文字） ---- */
bool MacCircleButton(float cx, float cy, float d, ImU32 color, ImU32 hover,
                     const char* tip) {
    ImGui::PushID((int)color); /* 颜色唯一 → 三个按钮 ID 不冲突（修复 Program error） */
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

/* ---- 自绘标题栏（无边框窗口：设置 + 标题 + Mac 风格窗口按钮 + 拖动） ---- */
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

    const float cy = kTitleBarH * 0.5f;  /* 按钮垂直中心 */

    /* 标题文本（设置入口已移至主窗口"设置"菜单栏，见 Render()） */
    ImGui::SetCursorPos(ImVec2(12, (kTitleBarH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::Text("%s", i18n::T("window.title"));

    /* 右侧：Mac 风格三色圆钮（红=关闭 最右，绿=最大化，黄=最小化） */
    const float btn_d = 14.0f;
    const float gap = 12.0f;
    const float margin = 18.0f; /* 距右缘 */
    /* 红（关闭） */
    float rx = io.DisplaySize.x - margin - btn_d * 0.5f;
    /* 绿（最大化） */
    float gx = rx - btn_d - gap;
    /* 黄（最小化） */
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

    /* 标题栏拖动：按下时触发 Windows 原生窗口拖动（WM_NCLBUTTONDOWN/HTCAPTION），
     * 由系统平滑移动窗口 → 无重影、无渲染干扰；非 Windows 回退为位置计算 */
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
        /* 非 Windows 回退：记录按下位置逐帧移动 */
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

/* ---- 窗口边缘/四角 resize（无边框窗口无系统手柄，自绘：左右下边 + 左下/右下角） ----
 * 标题栏顶部 6px 留给"拖动移动"，不参与 resize（避免与标题栏 InvisibleButton 冲突） */
void RenderResizeGrip() {
    const ImGuiIO& io = ImGui::GetIO();
    const float E = 6.0f;  /* 边缘带宽 */
    const float C = 18.0f; /* 四角区域 */
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
    /* hover 缩放光标（未按下时） */
    if (dir != None && !io.MouseDown[0]) {
        /* 系统光标 ID（IDC_* 展开为 LPSTR，LoadCursorW 需 MAKEINTRESOURCEW 数值） */
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

    /* 拖动状态 */
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
            nh = h0 + dy; /* B/BL/BR 下边 */
        }
        /* 最小尺寸（glfwSetWindowSizeLimits 已兜底，这里防左/上角越界） */
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

    /* 右下角三角标记（resize 视觉提示，hover 高亮） */
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
    /* 自绘标题栏（无边框窗口） */
    RenderTitleBar();

    /* 主窗口：从标题栏下方铺满客户区；自带"设置"菜单栏（语言切换，常规软件逻辑） */
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

    /* 设置菜单栏：语言切换入口显示"目标语言"提示
     * （中文界面 → "language"；英文界面 → "中文"，见 menu.lang_hint），
     * 点开后是常规语言选择项 */
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
        ImGui::EndMenuBar();
    }

    RenderForm(worker);

    /* 快照读取（每帧一次，锁内拷贝） */
    DownloadSnapshot snap;
    int stage = worker.GetSnapshot(snap);

    /* 阶段边沿 → 弹窗（完成 F13 / 错误 F12）与暂停态确认
     * CANCELED = 用户暂停（缓存保留，可继续/停止）；DONE/ERROR 退出暂停态 */
    if (g_started && stage != g_last_stage) {
        g_last_stage = stage;
        if (stage == STAGE_DONE) {
            g_done_path = snap.status;  /* 完成路径在日志里，此处仅提示 */
            g_done_open = true;
            g_paused = false;
        } else if (stage == STAGE_ERROR) {
            ShowErrorPopup(i18n::T("dialog.error.title"), snap.error,
                           ErrorGuide(snap.error));
            g_paused = false;
        } else if (stage == STAGE_CANCELED) {
            g_paused = true;
            worker.AddLog("[INFO] 已暂停（缓存保留）：点击『继续』断点续传，"
                          "或『停止』删除缓存");
        }
    }
    g_started = worker.IsRunning() || g_started;

    RenderProgress(snap);
    RenderLog(snap.log);

    /* 右下角 resize 手柄（无边框窗口，须在窗口 End 前绘制） */
    RenderResizeGrip();

    ImGui::End();

    /* 文件已存在四选一（F11）处理 */
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
                    worker.AddLog("[INFO] 已删除旧文件（覆盖）: " +
                                  g_pending.path);
                    StartDownload(worker, g_pending.url, g_pending.path,
                                  g_pending.threads);
                    break;
                case dialogs::ExistsChoice::Rename: {
                    std::string newpath = StampName(g_pending.path);
                    worker.AddLog("[INFO] 已改名: " + newpath);
                    StartDownload(worker, g_pending.url, newpath,
                                  g_pending.threads);
                    break;
                }
                default:  /* Cancel */
                    worker.AddLog("[INFO] 已取消（文件已存在弹窗）");
                    break;
            }
            g_pending.active = false;
        }
    }

    /* 错误弹窗（F12） */
    dialogs::ShowError(g_error_title, g_error_msg, g_error_guide,
                       g_error_open);

    /* 完成弹窗（F13） */
    dialogs::ShowDone(g_done_path, g_done_open);

    return true;
}

}  // namespace ui
