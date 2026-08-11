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
#include <windows.h>
#endif

int main(int argc, char** argv) {
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
