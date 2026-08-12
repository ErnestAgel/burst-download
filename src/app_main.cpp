/**
 * @file app_main.cpp
 * @brief Unified entry point: no args / --gui open the GUI; args run CLI.
 *
 * Windows uses the GUI subsystem: launching without args creates no console
 * window; when run from a terminal with args, the CLI attaches to the parent
 * console (or reuses the existing handles when output is redirected).
 *
 * @author ErnestAgel
 * @date 2026-08-11
 * @license SPDX-License-Identifier: MIT
 */

#include "app.h"
#include "embedded_runtime.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  /* Win10+: SetProcessMitigationPolicy etc. */
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

/* ---- Windows process-level security mitigations (anti DLL injection /
 * hijacking, best-effort, failures ignored) ----
 * 1) SetDefaultDllDirectories: forbid loading DLLs from the current working
 *    directory, preventing DLL search-order hijacking.
 * 2) ImageLoadPolicy: forbid loading remote (SMB/UNC) and low-integrity
 *    images, and prefer System32 (prevents forged system DLLs in the app
 *    directory).
 * Deliberately NOT enabling the "Microsoft-signed DLLs only" policy: it
 * would break legitimate GPU drivers / IMEs / antivirus modules. */
#ifdef _WIN32
static void EnableWindowsSecurityMitigations()
{
    /* 1) Forbid loading DLLs from the CWD (Win8+, anti search-order
     * hijacking). */
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    /* 2) Win10+ process mitigation: ImageLoadPolicy (dynamic load avoids
     * header version differences) - forbid remote and low-integrity images,
     * prefer System32. */
    typedef BOOL(WINAPI* SetMitigationFn)(PROCESS_MITIGATION_POLICY, PVOID,
                                          SIZE_T);
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (hKernel32 != nullptr)
    {
        SetMitigationFn fnSetMitigation = (SetMitigationFn)GetProcAddress(
            hKernel32, "SetProcessMitigationPolicy");
        if (fnSetMitigation != nullptr)
        {
            PROCESS_MITIGATION_IMAGE_LOAD_POLICY tPolicy = {};
            tPolicy.NoRemoteImages = 1;
            tPolicy.NoLowMandatoryLabelImages = 1;
            tPolicy.PreferSystem32Images = 1;
            fnSetMitigation(ProcessImageLoadPolicy, &tPolicy,
                            sizeof(tPolicy));
        }
    }
}
#endif

/**
 * @brief Absolute path of the current executable (UTF-8), or "" on failure.
 *
 * Resolves the real binary location even when launched via PATH or a
 * symlink, so embedded runtime assets are located reliably (issue R11).
 * @return Executable path, or empty string when resolution fails.
 */
static std::string GetExecutablePath()
{
#ifdef _WIN32
    wchar_t wszPath[32768];
    const DWORD dwLen = GetModuleFileNameW(NULL, wszPath, 32768);
    if ((dwLen == 0) || (dwLen >= 32768))
    {
        return "";
    }
    const int nUtf8Len = WideCharToMultiByte(CP_UTF8, 0, wszPath,
                                             (int)dwLen, NULL, 0, NULL, NULL);
    if (nUtf8Len <= 0)
    {
        return "";
    }
    std::string strOut((size_t)nUtf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wszPath, (int)dwLen, &strOut[0],
                        nUtf8Len, NULL, NULL);
    return strOut;
#else
    char szPath[4096];
    const ssize_t nLen = readlink("/proc/self/exe", szPath,
                                  sizeof(szPath) - 1);
    if (nLen <= 0)
    {
        return "";
    }
    szPath[nLen] = '\0';
    return std::string(szPath);
#endif
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    /* Must run before any third-party DLL loads (Python/GLFW/OpenGL). */
    EnableWindowsSecurityMitigations();
#endif
    /* Record the executable path for runtime asset location. */
    const std::string strExe = GetExecutablePath();
    EmbedSetExePath(strExe.empty() ? (argc > 0 ? argv[0] : "") : strExe);
    /* Explicit --gui / -g: open the GUI regardless of other args. */
    const bool bExplicitGui =
        (argc > 1) && ((std::strcmp(argv[1], "--gui") == 0) ||
                       (std::strcmp(argv[1], "-g") == 0));
#ifdef BURST_HAS_GUI
    if (bExplicitGui || (argc <= 1))
    {
        return RunGui(argc, argv);
    }
#else
    if (bExplicitGui)
    {
        std::fprintf(stderr,
                     "this build has no GUI (CLI-only); use the x86_64 "
                     "build for the GUI\n");
        return 1;
    }
#endif

#ifdef _WIN32
    /* CLI: when stdout is redirected, keep the existing handle; otherwise
     * attach to the parent console (or allocate one). */
    {
        const HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        const bool bRedirected =
            (hOut != NULL) && (hOut != INVALID_HANDLE_VALUE) &&
            (GetFileType(hOut) != FILE_TYPE_CHAR);
        if (!bRedirected)
        {
            if (!AttachConsole(ATTACH_PARENT_PROCESS))
            {
                AllocConsole();
            }
            /* Program output is UTF-8: switch the console code page so CJK
             * text renders correctly. */
            SetConsoleOutputCP(CP_UTF8);
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
    }
#endif
    return RunCli(argc, argv);
}
