/**
 * @file crashguard.cpp
 * @brief 崩溃兜底实现（§8.2）
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */

#include "crashguard.h"

#include <cstdio>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#include <signal.h>
#else
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace crashguard {

namespace {

/** @brief 追加一行到 crash.log（与 exe 同目录；处理器内尽量使用底层安全 API） */
void WriteCrashLog(const char* kind, unsigned long code) {
    char buf[512];
    time_t now = time(nullptr);
    struct tm* tm_now = localtime(&now);
    int n = 0;
    if (tm_now != nullptr) {
        n = snprintf(buf, sizeof(buf),
                     "[%04d-%02d-%02d %02d:%02d:%02d] CRASH kind=%s code=0x%lx\n",
                     tm_now->tm_year + 1900, tm_now->tm_mon + 1,
                     tm_now->tm_mday, tm_now->tm_hour, tm_now->tm_min,
                     tm_now->tm_sec, kind, code);
    } else {
        n = snprintf(buf, sizeof(buf), "CRASH kind=%s code=0x%lx\n", kind, code);
    }
#ifdef _WIN32
    HANDLE h = CreateFileA("crash.log", FILE_APPEND_DATA, FILE_SHARE_READ,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, buf, (DWORD)(n > 0 ? n : 0), &written, NULL);
        CloseHandle(h);
    }
#else
    int fd = open("crash.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        if (n > 0) write(fd, buf, (size_t)n);
        close(fd);
    }
#endif
}

/** @brief 弹窗提示（Windows）；Linux 无 GUI 弹窗能力，日志即指引 */
void ShowAlert() {
#ifdef _WIN32
    MessageBoxA(NULL,
                "发生意外错误，已记录 crash.log，请重新启动程序。",
                "burst-gui", MB_OK | MB_ICONERROR);
#else
    /* 无终端/无弹窗，crash.log 即指引 */
#endif
}

/** @brief 统一崩溃处理：写日志 → 弹窗 → 退出（不再返回） */
[[noreturn]] void HandleCrash(const char* kind, unsigned long code) {
    WriteCrashLog(kind, code);
    ShowAlert();
    _exit(1);
}

#ifdef _WIN32
/** @brief 未处理异常过滤器（SEH） */
LONG WINAPI SehFilter(EXCEPTION_POINTERS* ep) {
    unsigned long code = ep != nullptr ? ep->ExceptionRecord->ExceptionCode : 0;
    HandleCrash("SEH", code);
}

void SignalHandler(int sig) {
    HandleCrash("signal", (unsigned long)sig);
}
#else
void SignalHandler(int sig) {
    HandleCrash("signal", (unsigned long)sig);
}
#endif

}  // namespace

void Install() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(SehFilter);
    signal(SIGSEGV, SignalHandler);
    signal(SIGABRT, SignalHandler);
    signal(SIGFPE, SignalHandler);
    signal(SIGILL, SignalHandler);
#else
    signal(SIGSEGV, SignalHandler);
    signal(SIGABRT, SignalHandler);
    signal(SIGFPE, SignalHandler);
    signal(SIGILL, SignalHandler);
#endif
}

}  // namespace crashguard
