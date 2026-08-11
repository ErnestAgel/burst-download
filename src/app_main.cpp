/**
 * @file app_main.cpp
 * @brief 统一入口：无参数/--gui 打开图形界面；带参数走终端 CLI
 *
 * Windows 保持"控制台子系统"（终端 CLI 输出可被管道/变量捕获）；
 * 双击无参数启动时先 FreeConsole() 隐藏弹出的控制台窗口再进入 GUI。
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
#ifdef _WIN32
    if (argc <= 1) {
      FreeConsole();  /* 无参数（资源管理器双击）：隐藏控制台后启动 GUI */
    }
#endif
    return RunGui(argc, argv);
  }
  /* 带参数：终端 CLI */
  return RunCli(argc, argv);
#else
  /* 未编译 GUI 的构建（如 aarch64 CLI-only）：--gui 明确报错，其余走 CLI */
  if (explicit_gui) {
    std::fprintf(stderr,
                 "此构建不包含图形界面（CLI-only），请使用 x86_64 版本获取 GUI\n");
    return 1;
  }
  return RunCli(argc, argv);
#endif
}
