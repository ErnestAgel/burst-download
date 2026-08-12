/**
 * @file main_gui.cpp
 * @brief GUI entry (RunGui): GLFW window + ImGui init + main loop + safe
 *        exit flow.
 *
 * Exit rule: cancel -> join (bounded, then final join) -> ImGui/GLFW
 * cleanup -> return; detach or exit-while-running are forbidden.
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <unistd.h> /* Linux: readlink resolves /proc/self/exe */
#endif

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "font_data.h"
#include "app.h"
#include "crashguard.h"
#include "embed_python.h"
#include "i18n.h"
#include "theme.h"
#include "ui.h"
#include "taskmodel.h"

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

namespace {

/** @brief Fatal error prompt (Windows message box / Linux stderr). */
void ShowFatal(const char* msg) {
#ifdef _WIN32
    MessageBoxA(NULL, msg, "burst-gui", MB_OK | MB_ICONERROR);
#else
    fprintf(stderr, "burst-gui: %s\n", msg);
#endif
}

/** @brief Executable directory (UTF-8; used to locate config.ini). */
std::string ExeDir(const char* argv0) {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4] = {0};
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 4);
    std::wstring w(buf, n);
    size_t slash = w.find_last_of(L"/\\");
    if (slash != std::wstring::npos) {
        w = w.substr(0, slash);
    }
    /* UTF-16 -> UTF-8. */
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

/** @brief Single-instance guard: prevents two instances writing the same
 *         files; prompts and tries to activate the old window. */
bool AcquireSingleInstance() {
#ifdef _WIN32
    HANDLE h = CreateMutexW(NULL, TRUE, L"Global\\burst-gui");
    if (h == NULL) {
        return true;  /* creation failure (no rights etc.) is not blocking */
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* An instance already runs: try to activate its window. */
        HWND existing = FindWindowW(NULL, L"Burst Download");
        if (existing != NULL) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        MessageBoxW(NULL,
                    L"burst-gui is already running.\n"
                    L"If this appears right after launch, an old instance is "
                    L"running in the background:\n"
                    L"  1. find and close the old burst-gui window in the "
                    L"taskbar, or\n"
                    L"  2. end the burst-gui.exe process in Task Manager and "
                    L"retry.",
                    L"burst-gui", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    return true;
#else
    (void)0;
    return true;  /* Linux lock file planned (Phase 3; low risk on a single
                   * user desktop) */
#endif
}

}  // namespace

int RunGui(int argc, char** argv) {
    /* Crash guard: real crashes write crash.log + show a prompt. */
    crashguard::Install();

#ifdef _WIN32
    /* High-DPI awareness: prevents the borderless window from being DPI
     * scaled on first show (visible width "resize" jitter). */
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32 != nullptr) {
            typedef BOOL(WINAPI* SetDpiAwareFn)(DPI_AWARENESS_CONTEXT);
            SetDpiAwareFn fn = (SetDpiAwareFn)GetProcAddress(
                u32, "SetProcessDpiAwarenessContext");
            if (fn != nullptr) {
                fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            }
        }
    }
#endif

    /* Single-instance guard. */
    if (!AcquireSingleInstance()) {
        return 1;
    }

    /* i18n: config.ini persistence first, otherwise the system language. */
    i18n::Init(ExeDir(argc > 0 ? argv[0] : ""));

    /* Initialize the embedded Python runtime (idempotent; used for video
     * parsing).  Locate order: assets/ next to the exe -> temp cache ->
     * env/compile-time macro.  Init failure does not block the GUI (file
     * mode still works); video tasks retry and report clearly. */
    EmbedPythonInit();

    /* ---- GLFW ---- */
    if (!glfwInit()) {
        ShowFatal("GLFW initialization failed.");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef _WIN32
    /* Windows: borderless with a custom title bar / resize grip (ui.cpp
     * RenderTitleBar / RenderResizeGrip). */
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
#else
    /* Linux: system title bar and borders (window manager provides
     * drag/resize). */
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
#endif
    GLFWwindow* window = glfwCreateWindow(900, 640, i18n::T("window.title"),
                                          NULL, NULL);
    if (window == nullptr) {
        /* OpenGL 3.3+ unavailable (VM / old driver): prompt, do not crash. */
        ShowFatal("Failed to create an OpenGL 3.3+ window.\n"
                  "Upgrade your GPU driver, or disable 3D acceleration in "
                  "the virtual machine.");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    /* Minimum window size: prevents the resize grip from shrinking the
     * window unusably. */
    glfwSetWindowSizeLimits(window, 640, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
    /* Focus the borderless window at startup: avoids the first click being
     * consumed just to activate the window ("needs two clicks"). */
    glfwFocusWindow(window);
#ifdef _WIN32
    if (HWND hwnd = glfwGetWin32Window(window)) {
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
    }
#endif

    /* ---- ImGui ---- */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    /* One Dark theme (user-specified palette). */
    theme::ApplyOneDark();

    /* Embedded font (GB2312 full + ASCII, one font for both languages).
     * OversampleH/V raise the bitmap density for crisper CJK rendering
     * (atlas size grows as a tradeoff). */
    {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.OversampleH = 3;
        cfg.OversampleV = 3;
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(
            (void*)third_party_fonts_NotoSansSC_subset_ttf,
            (int)third_party_fonts_NotoSansSC_subset_ttf_len, 18.0f, &cfg,
            io.Fonts->GetGlyphRangesChineseFull());
        if (font == nullptr) {
            ShowFatal("Font loading failed.");
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
    }

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true) ||
        !ImGui_ImplOpenGL3_Init("#version 130")) {
        ShowFatal("ImGui backend initialization failed.");
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    /* ---- Main loop ---- */
    CTaskModel cModel;
    ui::Init(window);
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        /* Clear every frame: prevents ghosting after window resizes. */
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ui::Render(cModel);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    /* ---- Exit flow: cancel all tasks, then wait (cancel checkpoints and
     *      timeouts make the wait bounded, R1/R12) ---- */
    cModel.CancelAll();
    cModel.WaitAll();

    /* The embedded Python interpreter is intentionally NOT finalized:
     * Py_FinalizeEx can hang for a long time after yt_dlp loads many
     * extension modules (reported as "program unresponsive after download"),
     * and the CLI does not finalize either (the OS reclaims at exit; the
     * Python docs also allow skipping finalize for embedders).  If a
     * shutdown is ever needed, it must run after all GLFW/ImGui cleanup and
     * tolerate the hang risk. */

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
