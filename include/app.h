/**
 * @file app.h
 * @brief 统一程序入口：burst 同时是 CLI 与 GUI（无参数/--gui 打开图形界面，带参数走终端 CLI）
 *
 * @author ErnestAgel
 * @date 2026-08-11
 * @license SPDX-License-Identifier: MIT
 */

#ifndef APP_H
#define APP_H

/**
 * @brief CLI 入口（src/main.cpp）：带参数时由终端执行，输出走 stdout/stderr
 */
int RunCli(int argc, char** argv);

/**
 * @brief GUI 入口（gui/main_gui.cpp）：GLFW + ImGui 图形界面
 */
int RunGui(int argc, char** argv);

#endif  // APP_H
