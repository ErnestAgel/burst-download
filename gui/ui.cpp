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

#include "dialogs.h"
#include "i18n.h"
#include "imgui.h"
#include "toggle.h"
#include "Ccurl.h"  /* MaxThread */

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
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

/* 前向声明（RenderForm 在 OnStartClicked/StartDownload 之前定义） */
void OnStartClicked(DownloadWorker& worker);
void StartDownload(DownloadWorker& worker, const std::string& url,
                   const std::string& path, int threads);

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
    /* MVP 简易分类：信息不足时给通用指引 */
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

    /* 保存路径 + 浏览（Windows 原生 GetSaveFileName，§3） */
    ImGui::SetNextItemWidth(-70.0f);
    ImGui::InputText("##path", g_path, sizeof(g_path),
                     running ? ImGuiInputTextFlags_ReadOnly
                             : ImGuiInputTextFlags_None);
#ifdef _WIN32
    ImGui::SameLine();
    if (ImGui::Button(i18n::T("button.browse"), ImVec2(60, 0)) && !running) {
        OPENFILENAMEW ofn{};
        wchar_t buf[MAX_PATH * 2] = {0};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.lpstrTitle = L"curlbolt";
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
        if (GetSaveFileNameW(&ofn)) {
            std::string s = WideToUtf8(ofn.lpstrFile);
            snprintf(g_path, sizeof(g_path), "%s", s.c_str());
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

    /* 下载 / 取消按钮（F9/F10） */
    ImGui::Separator();
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::BeginDisabled(running || g_pending.active);
    if (ImGui::Button(i18n::T("button.download"),
                      ImVec2(avail * 0.5f - 4.0f, 0))) {
        OnStartClicked(worker);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button(i18n::T("button.cancel"),
                      ImVec2(avail * 0.5f - 4.0f, 0))) {
        worker.Cancel();
    }
    ImGui::EndDisabled();
}

void OnStartClicked(DownloadWorker& worker) {
    std::string url(g_url);
    std::string path(g_path);

    /* URL 预检（R11） */
    if (url.empty() || !UrlSchemeOk(url)) {
        ShowErrorPopup(i18n::T("dialog.error.title"),
                       i18n::T("err.url.invalid"));
        return;
    }
    /* 视频模式：Phase 2 实现，MVP 提示 */
    if (g_video_mode) {
        ShowErrorPopup(i18n::T("dialog.error.title"),
                       i18n::T("err.video.phase2"));
        return;
    }
    if (path.empty()) {
        ShowErrorPopup(i18n::T("dialog.error.title"), i18n::T("err.path.empty"));
        return;
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
                   const std::string& path, int threads) {
    worker.AddLog(std::string("[INFO] URL: ") + url);
    worker.AddLog(std::string("[INFO] 保存到: ") + path);
    g_last_stage = STAGE_IDLE;
    if (!worker.StartFileDownload(url, path, threads, 60)) {
        ShowErrorPopup(i18n::T("dialog.error.title"), i18n::T("err.busy"));
    }
}

/* ---- 进度区渲染（F5/F6/F8） ---- */
void RenderProgress(const DownloadSnapshot& snap) {
    ImGui::Separator();
    ImGui::Text("%s", i18n::T("label.total"));
    /* 总进度条 */
    char buf[128];
    if (snap.totalSpeed > 0) {
        snprintf(buf, sizeof(buf), "%.1f%%  |  %.2f MB/s  |  ETA %s",
                 snap.totalPercent, snap.totalSpeed / (1024.0 * 1024.0),
                 snap.eta.c_str());
    } else {
        snprintf(buf, sizeof(buf), "%.1f%%  |  ETA %s", snap.totalPercent,
                 snap.eta.c_str());
    }
    ImGui::ProgressBar((float)(snap.totalPercent / 100.0),
                       ImVec2(-1.0f, 0), buf);

    /* 每线程进度表（F5） */
    if (!snap.threads.empty()) {
        ImGui::Text("%s", i18n::T("label.thread"));
        ImGui::BeginChild("##threads_list",
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() *
                                         snap.threads.size() +
                                         8.0f),
                          true);
        for (const auto& t : snap.threads) {
            char label[256];
            snprintf(label, sizeof(label), "%s #%d", i18n::T("label.thread"),
                     t.id);
            char overlay[128];
            double mb_done = t.downloaded / (1024.0 * 1024.0);
            double mb_total = t.total / (1024.0 * 1024.0);
            snprintf(overlay, sizeof(overlay), "%.1f/%.1f MB | %.2f MB/s",
                     mb_done, mb_total, t.speed / (1024.0 * 1024.0));
            ImGui::ProgressBar((float)(t.percent / 100.0),
                               ImVec2(-1.0f, 0), overlay);
        }
        ImGui::EndChild();
    }

    /* 阶段状态文本（F8） */
    const char* stage_txt = i18n::T("stage.idle");
    switch (snap.stage) {
        case STAGE_DOWNLOADING: stage_txt = i18n::T("stage.downloading"); break;
        case STAGE_PARSING:     stage_txt = i18n::T("stage.parsing"); break;
        case STAGE_MERGING:     stage_txt = i18n::T("stage.merging"); break;
        case STAGE_DONE:        stage_txt = i18n::T("stage.done"); break;
        case STAGE_CANCELED:    stage_txt = i18n::T("stage.canceled"); break;
        case STAGE_ERROR:       stage_txt = i18n::T("stage.error"); break;
        default: break;
    }
    ImGui::Text("%s: %s", i18n::T("label.status"), stage_txt);
}

/* ---- 日志区（F7） ---- */
void RenderLog(const std::vector<std::string>& log) {
    ImGui::Separator();
    ImGui::Text("%s", i18n::T("label.log"));
    ImGui::BeginChild("##log", ImVec2(0, 160), true);
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
        g_log_autoscroll = true;
    }
    for (const auto& line : log) {
        ImGui::TextWrapped("%s", line.c_str());
    }
    if (g_log_autoscroll && ImGui::GetScrollMaxY() > 0) {
        ImGui::SetScrollY(ImGui::GetScrollMaxY());
    }
    ImGui::EndChild();
}

/* ---- Mac 风格圆形窗口按钮（红=关闭/黄=最小化/绿=最大化，hover 提示文字） ---- */
bool MacCircleButton(float cx, float cy, float d, ImU32 color, ImU32 hover,
                     const char* tip) {
    ImGui::SetCursorScreenPos(ImVec2(cx - d * 0.5f, cy - d * 0.5f));
    ImGui::InvisibleButton("##macbtn", ImVec2(d, d));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
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

    /* 左侧：设置按钮（与右侧窗口控制分离，符合常规软件逻辑） */
    ImGui::SetCursorPos(ImVec2(8, (kTitleBarH - 26) * 0.5f));
    if (ImGui::Button(u8"⚙", ImVec2(26, 26))) {
        ImGui::OpenPopup("##settings_popup");
    }
    ImGui::SetItemTooltip("%s", i18n::T("menu.settings"));
    if (ImGui::BeginPopup("##settings_popup")) {
        ImGui::Text("%s:", i18n::T("menu.language"));
        int cur = (i18n::GetLang() == i18n::Lang::Zh) ? 0 : 1;
        char lang_items[128];
        snprintf(lang_items, sizeof(lang_items), "%s%c%s%c",
                 i18n::T("lang.zh"), '\0', i18n::T("lang.en"), '\0');
        if (ImGui::Combo("##lang", &cur, lang_items, 2)) {
            i18n::SetLang(cur == 0 ? i18n::Lang::Zh : i18n::Lang::En);
        }
        ImGui::EndPopup();
    }

    /* 标题文本 */
    ImGui::SameLine();
    ImGui::SetCursorPosY((kTitleBarH - ImGui::GetTextLineHeight()) * 0.5f);
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

    /* 标题栏拖动（按住空白区拖动窗口；用按下时位置+鼠标位移，避免漂移） */
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##titlebar_drag",
                           ImVec2(io.DisplaySize.x - btn_d * 3 - gap * 2 -
                                      margin * 2 - 8,
                                  kTitleBarH));
    static bool dragging = false;
    static int drag_x0 = 0, drag_y0 = 0;
    static ImVec2 drag_mouse0;
    if (ImGui::IsItemActive() && !dragging) {
        dragging = true;
        glfwGetWindowPos(g_window, &drag_x0, &drag_y0);
        drag_mouse0 = io.MousePos;
    } else if (!ImGui::IsItemActive() && dragging) {
        dragging = false;
    }
    if (dragging && g_window != nullptr) {
        glfwSetWindowPos(g_window,
                         drag_x0 + (int)(io.MousePos.x - drag_mouse0.x),
                         drag_y0 + (int)(io.MousePos.y - drag_mouse0.y));
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

/* ---- 右下角 resize 手柄（无边框窗口无系统手柄，自绘） ---- */
void RenderResizeGrip() {
    const ImGuiIO& io = ImGui::GetIO();
    const float grip = 16.0f;
    ImGui::SetCursorPos(ImVec2(io.DisplaySize.x - grip,
                               io.DisplaySize.y - kTitleBarH - grip));
    ImGui::InvisibleButton("##resize_grip", ImVec2(grip, grip));
    static bool resizing = false;
    static int rw0 = 0, rh0 = 0;
    static ImVec2 rmouse0;
    if (ImGui::IsItemActive() && !resizing && g_window != nullptr) {
        resizing = true;
        glfwGetWindowSize(g_window, &rw0, &rh0);
        rmouse0 = io.MousePos;
    } else if (!ImGui::IsItemActive() && resizing) {
        resizing = false;
    }
    if (resizing && g_window != nullptr) {
        glfwSetWindowSize(g_window,
                          rw0 + (int)(io.MousePos.x - rmouse0.x),
                          rh0 + (int)(io.MousePos.y - rmouse0.y));
    }
    /* 手柄样式：画一个右下角小三角 */
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetWindowPos();
    ImVec2 c = ImVec2(p.x + io.DisplaySize.x - 2, p.y + io.DisplaySize.y - kTitleBarH - 2);
    ImU32 col = ImGui::GetColorU32(ImGuiCol_SeparatorHovered);
    dl->AddTriangleFilled(ImVec2(c.x - 8, c.y), c, ImVec2(c.x, c.y - 8), col);
}

}  // namespace

void Init(GLFWwindow* window) {
    g_window = window;
}

bool Render(DownloadWorker& worker) {
    /* 自绘标题栏（无边框窗口） */
    RenderTitleBar();

    /* 主窗口：从标题栏下方铺满客户区 */
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, kTitleBarH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(io.DisplaySize.x, io.DisplaySize.y - kTitleBarH),
        ImGuiCond_Always);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);

    RenderForm(worker);

    /* 快照读取（每帧一次，锁内拷贝） */
    DownloadSnapshot snap;
    int stage = worker.GetSnapshot(snap);

    /* 阶段边沿 → 弹窗（完成 F13 / 错误 F12 / 取消提示） */
    if (g_started && stage != g_last_stage) {
        g_last_stage = stage;
        if (stage == STAGE_DONE) {
            g_done_path = snap.status;  /* 完成路径在日志里，此处仅提示 */
            g_done_open = true;
        } else if (stage == STAGE_ERROR) {
            ShowErrorPopup(i18n::T("dialog.error.title"), snap.error,
                           ErrorGuide(snap.error));
        }
    }
    g_started = worker.IsRunning() || g_started;

    RenderProgress(snap);
    RenderLog(snap.log);
    ImGui::End();

    /* 右下角 resize 手柄（无边框窗口） */
    RenderResizeGrip();

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
