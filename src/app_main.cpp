/**
 * @file app_main.cpp
 * @brief 统一入口：无参数/--gui 打开图形界面；带参数走终端 CLI
 *
 * Windows 为 GUI 子系统：双击无参数启动不创建控制台窗口；
 * 终端带参数运行时附加到父进程控制台输出（已重定向到文件/管道时沿用现有句柄）。
 *
 * @author ErnestAgel
 * @date 2026-08-11
 * @license SPDX-License-Identifier: MIT
 */

#include "app.h"
#include "embedded_runtime.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 /* Win10+：SetProcessMitigationPolicy 等 API 需要 */
#endif
#include <windows.h>
#endif

/* ---- Windows 进程级安全缓解（防 DLL 注入/劫持，best-effort，失败忽略） ----
 * 1) SetDefaultDllDirectories：禁止从当前工作目录(CWD)加载 DLL，
 *    防"下载目录放一个同名恶意 dll"的 DLL 搜索顺序劫持；
 * 2) ImageLoadPolicy：禁止加载远程(SMB/UNC)来源与低完整性级别(Low IL)
 *    的镜像，并优先 System32（防应用目录伪造系统 dll）。
 * 刻意不启用"仅微软签名 DLL"策略：会误伤显卡驱动/输入法/杀软等
 * 合法第三方模块。 */
#ifdef _WIN32
static void EnableWindowsSecurityMitigations() {
  /* 1) 禁止从当前工作目录(CWD)加载 DLL（Win8+，防 DLL 搜索顺序劫持） */
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

  /* 2) Win10+ 进程缓解：ImageLoadPolicy（动态加载规避头文件版本差异）——
   *    禁止加载远程(SMB/UNC)与低完整性级别(Low IL)镜像，并优先 System32。 */
  typedef BOOL(WINAPI* SetMitigationFn)(PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  if (k32 != nullptr) {
    auto fn = (SetMitigationFn)GetProcAddress(k32, "SetProcessMitigationPolicy");
    if (fn != nullptr) {
      PROCESS_MITIGATION_IMAGE_LOAD_POLICY img{};
      img.NoRemoteImages = 1;
      img.NoLowMandatoryLabelImages = 1;
      img.PreferSystem32Images = 1;
      fn(ProcessImageLoadPolicy, &img, sizeof(img));
    }
  }
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  /* 必须在任何第三方 DLL 加载（Python/GLFW/OpenGL）之前调用 */
  EnableWindowsSecurityMitigations();
#endif
  /* 记录可执行文件路径，供运行时定位使用 */
  EmbedSetExePath(argc > 0 ? argv[0] : "");
  /* 显式 --gui / -g：无论是否带其它参数都打开图形界面 */
  const bool explicit_gui =
      argc > 1 && (std::strcmp(argv[1], "--gui") == 0 ||
                   std::strcmp(argv[1], "-g") == 0);
#ifdef BURST_HAS_GUI
  if (explicit_gui || argc <= 1) {
    return RunGui(argc, argv);
  }
#else
  if (explicit_gui) {
    std::fprintf(stderr,
                 "此构建不包含图形界面（CLI-only），请使用 x86_64 版本获取 GUI\n");
    return 1;
  }
#endif

#ifdef _WIN32
  /* CLI：stdout 已是文件/管道（重定向）则直接沿用；否则附加父进程控制台（无则分配） */
  {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    bool redirected = h != NULL && h != INVALID_HANDLE_VALUE &&
                      GetFileType(h) != FILE_TYPE_CHAR;
    if (!redirected) {
      if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
      }
      /* 程序输出为 UTF-8：把控制台代码页切到 UTF-8，避免中文乱码 */
      SetConsoleOutputCP(CP_UTF8);
      freopen("CONOUT$", "w", stdout);
      freopen("CONOUT$", "w", stderr);
    }
  }
#endif
  return RunCli(argc, argv);
}
