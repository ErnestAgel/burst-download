/**
 * @file main_gui.cpp
 * @brief curlbolt-gui 入口：GLFW 窗口 + ImGui 初始化 + 主循环 + 安全退出流程（§8.4）
 *
 * 退出铁律：置 cancel → join（≤5s）→ ImGui/GLFW 清理 → return；
 * 禁止 detach 或 RUNNING 中 exit（§8.4）。
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "font_data.h"
#include "crashguard.h"
#include "i18n.h"
#include "theme.h"
#include "ui.h"
#include "worker.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/** @brief 致命错误提示（Windows 弹窗 / Linux stderr） */
void ShowFatal(const char* msg) {
#ifdef _WIN32
    MessageBoxA(NULL, msg, "curlbolt-gui", MB_OK | MB_ICONERROR);
#else
    fprintf(stderr, "curlbolt-gui: %s\n", msg);
#endif
}

/** @brief 获取可执行文件所在目录（UTF-8；用于 config.ini 同目录定位） */
std::string ExeDir(const char* argv0) {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4] = {0};
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 4);
    std::wstring w(buf, n);
    size_t slash = w.find_last_of(L"/\\");
    if (slash != std::wstring::npos) {
        w = w.substr(0, slash);
    }
    /* UTF-16 → UTF-8 */
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0,
                                  NULL, NULL);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, NULL,
                        NULL);
    return s;
#else
    (void)argv0;
    std::string exe = "/proc/self/exe";
    char buf[4096];
    ssize_t n = readlink(exe.c_str(), buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string p(buf);
        size_t slash = p.find_last_of('/');
        return (slash != std::string::npos) ? p.substr(0, slash) : ".";
    }
    return ".";
#endif
}

/** @brief 单实例互斥（§8.4）：已有一个实例则提示并尝试激活旧窗口，防双开同时写同一文件 */
bool AcquireSingleInstance() {
#ifdef _WIN32
    HANDLE h = CreateMutexW(NULL, TRUE, L"Global\\curlbolt-gui");
    if (h == NULL) {
        return true;  /* 创建失败不阻塞（无权限等），按单实例处理 */
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* 已有实例：激活其窗口（若可见），并给出清晰提示 */
        HWND existing = FindWindowW(NULL, L"curlbolt-gui");
        if (existing == NULL) {
            /* 窗口标题可能是 i18n 文本（中/英），按进程名枚举兜底 */
            existing = NULL;
            HWND hw = NULL;
            DWORD target_pid = 0;
            /* 不精确匹配：仅提示即可 */
            (void)hw;
            (void)target_pid;
        }
        if (existing != NULL) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        MessageBoxW(NULL,
                    L"curlbolt-gui 已在运行。\n"
                    L"若刚启动即提示此信息，说明已有实例在后台运行：\n"
                    L"  1. 请在任务栏找到并关闭旧的 curlbolt-gui 窗口，或\n"
                    L"  2. 在任务管理器中结束 curlbolt-gui.exe 进程后重试。",
                    L"curlbolt-gui", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    return true;
#else
    (void)0;
    return true;  /* Linux 锁文件 Phase 3 补齐（当前桌面单用户场景风险低） */
#endif
}

}  // namespace

int main(int argc, char** argv) {
    /* 崩溃兜底（§8.2）：真崩溃也写 crash.log + 弹窗，不"裸退" */
    crashguard::Install();

    /* 单实例检测（§8.4） */
    if (!AcquireSingleInstance()) {
        return 1;
    }

    /* 国际化：config.ini 持久化优先，否则跟随系统（§3.3） */
    i18n::Init(ExeDir(argc > 0 ? argv[0] : ""));

    /* ---- GLFW ---- */
    if (!glfwInit()) {
        ShowFatal("GLFW 初始化失败。");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    /* 无边框窗口：系统标题栏由 UI 自绘（最小化/最大化/关闭按钮在自绘标题栏上） */
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(900, 640, i18n::T("window.title"),
                                          NULL, NULL);
    if (window == nullptr) {
        /* R14：OpenGL 3.3+ 不可用（虚拟机/旧驱动）→ 弹窗指引，不崩溃 */
        ShowFatal("无法创建 OpenGL 3.3+ 窗口。\n"
                  "请升级显卡驱动，或在虚拟机中关闭 3D 加速后重试。");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    /* ---- ImGui ---- */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    /* One Dark 主题（用户指定配色） */
    theme::ApplyOneDark();

    /* 嵌入字体（GB2312 全量 + ASCII，中英一套字体，§3.3/§7.3）
     * OversampleH/V 提高位图密度 → 中文渲染更清晰（代价：atlas 体积增大） */
    {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(
            (void*)third_party_fonts_NotoSansSC_subset_ttf,
            (int)third_party_fonts_NotoSansSC_subset_ttf_len, 18.0f, &cfg,
            io.Fonts->GetGlyphRangesChineseFull());
        if (font == nullptr) {
            ShowFatal("字体加载失败。");
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
    }

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true) ||
        !ImGui_ImplOpenGL3_Init("#version 130")) {
        ShowFatal("ImGui 后端初始化失败。");
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    /* ---- 主循环 ---- */
    DownloadWorker worker;
    ui::Init(window);
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        /* 每帧清屏：修复窗口 resize 时旧帧残留导致的拖影 */
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ui::Render(worker);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    /* ---- 退出流程（§8.4 铁律）：置 cancel → join（≤5s）→ 清理 ---- */
    if (worker.IsRunning()) {
        worker.Cancel();
        if (!worker.Join(5)) {
            printf("[gui] worker join timeout, force continue cleanup\n");
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
