/**
 * @file crashguard.h
 * @brief 崩溃兜底（§8.2）：未捕获异常/信号/SEH 时写 crash.log + 弹窗，防止"无声崩溃退出"
 *
 * 原则：正常业务错误（网络/HTTP/路径/磁盘等）绝不走到这层；此层只处理真崩溃
 * （野指针、栈溢出、非法指令等），处理器内只做安全操作。
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */
#pragma once

namespace crashguard {

/**
 * @brief 安装崩溃兜底：Windows 注册 SEH 过滤器 + 信号处理器；Linux 注册信号处理器
 * @note 程序启动时调用一次；处理器写 crash.log（与 exe 同目录）并弹窗提示后 _exit(1)
 */
void Install();

}  // namespace crashguard
