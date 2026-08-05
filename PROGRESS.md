# PROGRESS.md

## 目标
- 配置 VSCode（C/C++ + Doxygen 扩展与设置）✅ 已提交
- 清理旧注释 + 规范 Doxygen 注释 ✅ 已提交
- README 美化（中英双语 + 徽章）✅ 已提交
- 功能开发：CLI / 断点续传 / 速度 ETA / 失败重试 + 修 bug ✅ 已实现（待提交）
- **跨平台改造 + 项目自带库：Docker 编译 x86_64 libcurl + CMake 平台检测（Windows/Linux x86_64/Linux aarch64）+ Windows 条件编译 ✅ 已实现（待提交）**

## 已完成
- [x] README/Doxygen/VSCode 配置提交推送（3910084 / efc222e）
- [x] 功能开发：CLI、断点续传（含进度基数修复）、Range 检测+单线程退化、3 次重试、速度/ETA、int64/curl_off_t 大文件、11 个 bug 修复；三轮 review 最终 pass
- [x] **Docker 验证（WSL/x86 环境）**：
  - 首次用系统 libcurl 验证：编译 ✓、-h ✓、10MB 下载完整 ✓、断点续传 ✓、速度/ETA ✓
  - 发现并修复：`-h` 作为首参被当 URL 的 bug；续传进度基数不计入的显示 bug
- [x] **跨平台改造**：
  - Docker 内编译 curl-7.88.1 源码（--with-openssl）产出 x86_64 libcurl.so.4.8.0（680KB）→ `lib/linux-x86_64/`
  - 现有 ARM64 库迁移 → `lib/linux-aarch64/`（git rename）
  - CMakeLists 平台检测：`WIN32` → `find_package(CURL)`；Linux 按 `CMAKE_SYSTEM_PROCESSOR` 匹配 aarch64/其他 选项目库 + rpath；Windows 不链 pthread、不构建 demo
  - Ccurl.h/cpp Windows 条件编译：CreateThread/WaitForSingleObject 替代 pthread、CreateFileMappingA/MapViewOfFile 替代 mmap、Sleep 替代 sleep、`_stat64` 替代 stat、threadid 用 %p 打印
  - 进度互斥锁改 `std::mutex`（跨平台，修复 pthread_mutex 在 Windows 不存在）
- [x] **双平台验证**：
  - Linux x86_64：项目自带库编译 ✓，`ldd` 确认 `libcurl.so.4 => lib/linux-x86_64/libcurl.so.4` ✓（核心诉求达成），https 10MB 下载完整 ✓，demo 同样链接项目库 ✓
  - Windows：mingw 交叉 `-fsyntax-only` 检查 Ccurl.cpp/main.cpp 语法正确 ✓（修复 HANDLE→long 丢精度编译错误）
- [x] **Windows 端完整验证**：
  - mingw 交叉编译 curl-7.88.1（--host=x86_64-w64-mingw32）→ `lib/windows-x86_64/`（libcurl-4.dll PE32+ + libcurl.dll.a import lib，依赖仅系统 dll + libwinpthread）
  - CMake Windows 分支：优先项目库，回退 find_package(CURL)；POST_BUILD 自动复制 libcurl-4.dll 到 exe 目录
  - 交叉编译 curl_download.exe 链接项目 Windows 库成功（objdump 确认 DLL Name: libcurl-4.dll）
  - **wine 实际运行验证：`-h` 正常 + http 10MB 真实下载完整（10485760 字节，多线程分片 + 进度显示）**
  - 发现并修复：mingw import lib 记录的 DLL 名为 libcurl-4.dll（带版本号），项目文件重命名对齐
- [x] **最终检查**：模拟 pull 后干净编译（三平台成果物齐全 + cmake/make 通过 + ldd 项目库 + 10MB 下载冒烟测试）
- [x] README 构建章节更新为三平台说明
- [x] **CLI 传参 + 超时中断 + 日志机制**（本轮）：
  - CLI：`--timeout N`（下载无进展 N 秒自动中断，默认 60）/ `--no-timeout`（强制不中断）；help 同步更新
  - 超时：分片下载与探测请求（HEAD 探测/Range 检测）均受低速超时保护（LOW_SPEED_LIMIT=1 + LOW_SPEED_TIME，CURLOPT_NOSIGNAL 多线程安全）
  - 日志：新增 `AppendLog()`（线程安全、带时间戳）写入 `download.log`——超时中断、分片重试耗尽、探测失败、任务完成/失败均记录
  - README 使用章节重写为 CLI 传参示例（消除"编译进代码"旧说明），注意事项更新（跨平台/超时/续传）
  - Docker 验证：help ✓、传参下载 10MB ✓、挂起服务器 `--timeout 3` 约 6.5s 超时中断 ✓、log 记录 `curl error: Timeout was reached` ✓、成功/失败日志 ✓
- [x] 新增 `.gitignore`（忽略 build/、*.o）

## 进行中
- 等待用户确认后提交推送（未提交：CMakeLists.txt、PROGRESS.md、Ccurl.h、Ccurl.cpp、main.cpp、.gitignore、lib/ 重组 + 新库）

## 关键决策
- `-t` 语义 = 实际线程数（末分片承担余数）
- Range 不支持时静默退化单线程并丢弃断点（O_TRUNC 重下）
- 断点续传仅校验大小不校验远程内容（ETag）——**已知限制**，后续增强
- 项目库按架构分子目录：`lib/linux-x86_64/`、`lib/linux-aarch64/`，Windows 用系统 curl（find_package）
- 三个 .so 文件均为实体文件（非符号链接），兼容 Windows clone（core.symlinks=false）
- libcurl 版本统一 7.88.1（与用户原 ARM64 库同版本，soname libcurl.so.4）
- 本机无 gcc/g++：验证全部在 Docker 容器（ubuntu + 编译工具）完成

## 下一步
1. 用户确认后提交推送（建议 commit：`feat: cross-platform build with bundled libcurl (x86_64/aarch64/windows)`）
2. 后续增强候选：ETag 内容校验、下载队列、zsync 增量更新、Windows 真机编译验证（需 vcpkg/预编译 curl）
