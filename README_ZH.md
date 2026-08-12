<div align="center">

[🇨🇳 中文](README_ZH.md) · [🇬🇧 English](README.md)

# ⚡ Burst Download

**多线程分片下载器 · 支持视频下载**  
**Multi-threaded chunked downloader with video download support**

![C/C++](https://img.shields.io/badge/language-C%2FC%2B%2B-blue?style=for-the-badge)
![libcurl](https://img.shields.io/badge/libcurl-green?style=for-the-badge&logo=curl&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-x86_64%20%7C%20ARM64-orange?style=for-the-badge&logo=linux&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-x86_64-blue?style=for-the-badge&logo=windows&logoColor=white)
![CMake](https://img.shields.io/badge/build-CMake-yellow?style=for-the-badge&logo=cmake&logoColor=white)

[![Stars](https://img.shields.io/github/stars/ErnestAgel/burst-download?style=flat-square)](https://github.com/ErnestAgel/burst-download/stargazers)
[![Forks](https://img.shields.io/github/forks/ErnestAgel/burst-download?style=flat-square)](https://github.com/ErnestAgel/burst-download/network)
[![Last commit](https://img.shields.io/github/last-commit/ErnestAgel/burst-download?style=flat-square)](https://github.com/ErnestAgel/burst-download/commits/main)

> 🎬 **视频下载**：一条命令下载 B站 / YouTube 等主流网站的视频，多线程分片下载
> ⚡ **多线程加速**：HTTP Range 分片，1~8 线程并发（默认按 CPU 核数自适应），榨干带宽
> 📦 **断点续传**：中断后从断点继续，不重头来
> 🖥 **三平台构建**：Linux x86_64 / ARM64 / Windows，Release 单文件发布

</div>

---

## ✨ 特性 Features

| | 说明 Description |
|---|---|
| 🎬 **视频下载** | `--video` 模式：输入视频网页 URL，自动解析媒体流直链（B站/YouTube 等主流网站），多线程分片下载；音视频分离流（DASH）下载后**自动合并**为单文件（MP4 / WebM 多格式容器） |
| 🔄 **解析器更新** | 仅在**解析失败**时自动更新 yt-dlp 并重试一次，仍失败报真实错误；`--update-parser` 保留手动更新 |
| ⚡ **多线程并发** | `-t` 1~8 线程（默认按 CPU 核数 2~4 自适应），HTTP Range 分片，最后一个分片负责余数 |
| 📚 **多任务与批量** | GUI 任务列表 + 4 并发槽位（下载中可继续添加）；CLI `-j N` 多 URL 前台批量 |
| ⚡ **全 lane 引擎（P8-4）** | curl_multi 非阻塞多路复用：每任务可占满全部连接，多任务共享固定 lane 数异步推进 |
| 📦 **断点续传** | 自动检测本地已存在文件并从断点继续；服务器不支持 Range 时自动退化为单线程 |
| ⏱ **超时中断与日志** | `--timeout` / `--no-timeout` 控制；超时中断、失败详情写入 `download.log` |
| 🍪 **Cookie 支持** | `--cookies-from-browser` 读浏览器登录态（B站 720p+ 高清流）、`--cookie` 手动指定 |
| 🛡 **防盗链 Referer** | 自动携带视频页 Referer，B站等视频流防 403 |
| 🖥 **跨平台** | Linux x86_64 / Linux aarch64 / Windows；**Debug（动态库调试）+ Release（静态单文件发布）双构建**；Windows Release 需将运行 dll 与 exe 同目录（构建时自动复制） |

---

## 💡 为什么用 Burst Download？Why Burst Download?

**对比传统下载工具（curl / wget）：**

| | curl / wget | burst |
|---|---|---|
| 连接数 | 单线程、单连接 | 1~8 个并发连接 |
| 带宽利用 | 单连接受 TCP 慢启动/拥塞窗口限制，高带宽高延迟网络常吃不饱 | 多连接并行，逼近带宽上限 |
| 断点续传 | `curl -C -` 需手动指定 | 自动检测本地文件，断点续传 |
| 视频下载 | ❌ 不支持 | `--video` 自动解析直链下载 |
| 日志/超时 | 无 | 超时中断 + `download.log` |

**适用场景 ✅**

- GitHub Releases、软件源、镜像站、CDN 等支持 Range 的静态资源
- 大文件：ISO、压缩包、数据集、模型权重
- 视频网站直链下载（B站 / YouTube）

**不适用场景 ❌**

- 百度网盘等**账号级限速**网盘：服务端按账号限速，多线程总量不变
- 不支持 Range 的服务器（自动退化为单线程）
- 需登录 + 动态签名的私有网盘（无直链）

---

## ⚙️ 加速原理 How It Works

![多线程分片下载原理](docs/how-it-works.svg)

1. **HTTP Range 分片**：向服务器发送 `Range: bytes=0-26214399` 这类请求，把大文件切成 N 段；
2. **多线程并发**：N 个线程各持一条独立 TCP 连接，同时拉取自己的分片；
3. **为什么能加速**：单条 TCP 连接受**慢启动 + 拥塞窗口**限制，实际速率往往达不到带宽上限（高延迟网络尤其明显，如跨国下载）；多连接并行叠加，就能逼近带宽上限；
4. **前提条件**：服务器支持 Range 且**不设账号级限速**——静态文件服务器 / CDN 天然满足；百度网盘这类按账号限速的不满足，开再多线程总量也不变；
5. **收尾拼接**：各分片落盘后合并为完整文件（最后一个分片负责余数）。

---

## 🎬 视频下载（亮点 Video Download）

一条命令，任意支持网站：

```bash
# 下载 B站视频（自动解析 + 多线程分片下载）
./burst --video "https://www.bilibili.com/video/BVxxxxxxxx" -o movie

# B站高清 720p+（需要浏览器已登录 B站）
./burst --video "https://www.bilibili.com/video/BVxxxxxxxx" -o movie --cookies-from-browser chrome

# YouTube 等主流视频网站
./burst --video "https://www.youtube.com/watch?v=xxxxx" -o clip
```

- 支持 B站、YouTube 等主流视频网站，视频网页地址直接可用
- 音视频分离流（DASH）自动下载视频轨 + 音频轨，并**自动合并**为单文件（内置合并引擎，全程进程内，无需外部工具）：输出容器按视频编码自动选择——VP9/AV1 视频轨 → `.mkv`，其余 → `.mp4`；合并成功后自动删除音视频中间文件
- **文件命名防覆盖**：未指定 `-o` 时按 URL 推断 + 时间戳命名（如 `10Mb_20260807_123456.dat`、`BVxxxx_20260807_123456_full.mkv`）；显式 `-o` 指定的目标已存在时自动追加时间戳避让，不会覆盖已有文件

---

## 🚀 快速开始 Quick Start

```bash
./burst <url> [-o filename] [-t threads] [--timeout sec] [--no-timeout]
./burst --video <video-url> [-o basename] [-t threads] [--timeout sec]
./burst --update-parser
./burst --version
```

```bash
# 下载文件（8 线程，30 秒无进展超时）
./burst https://example.com/file.iso -o file.iso -t 8 --timeout 30

# 批量下载 3 个文件，2 个并发
./burst https://a/file1.iso https://b/file2.iso https://c/file3.iso -j 2

# 下载视频
./burst --video "https://www.bilibili.com/video/BVxxxx" -o movie

# 强制下载不自动中断
./burst https://example.com/file.iso -o file.iso --no-timeout

# 在线更新视频解析组件（网站改版导致解析失效时自愈，无需重新编译）
./burst --update-parser

# 查看帮助
./burst -h

# 查看版本
./burst --version
```

终端实时输出**进度 / 速率 / 剩余时间**：

```
percent: 42% speed: 3.20 MB/s ETA: 00:05:12
```

---

## 🔨 构建 Build

项目自带三平台 libcurl 库、最小化 FFmpeg 静态库与 Python 运行时（`third_party/`），无需安装开发包。

**Debug（默认）**：链接动态库，便于 gdb 调试

```bash
cmake -B build . && cmake --build build        # Linux
cmake -B build -G "MinGW Makefiles" .          # Windows（MSYS2/mingw64 环境，gcc 与 mingw32-make 需在 PATH）
```

**Release**：链接静态库，产出**单文件程序**。Windows 需将 `third_party/python/windows-x86_64/dll/` 下的运行 dll 与 exe 同目录（CMake 构建时自动复制），解压即用。Linux：curl/openssl/python/ffmpeg 均为仓库静态库，glibc 动态链接，运行时依赖桌面环境的 `libGL`/`libX11`；产物为单文件。视频解析组件随程序分发，开箱即用。

```bash
# Linux（openssl 静态库由构建脚本准备）
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT=/path/to/openssl \
      -DCURL_STATIC_DEPS="/path/libz.a;/path/libzstd.a" .
cmake --build build
# Windows（Schannel 原生 TLS，无需 openssl）
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build
```

---

## 🖥️ 图形界面 GUI

![Burst Download GUI（Windows）](docs/GUI.png)

图形界面提供**文件下载 + 视频下载**操作，支持 **Windows x86_64 / Linux x86_64**（Linux aarch64 构建不含图形界面）。

**运行**：

```bash
./burst                 # 打开图形界面（GUI）
./burst --gui           # 显式指定打开图形界面
./burst <url> ...       # 终端 CLI 下载
```

Windows：双击 `burst.exe` 打开 GUI；Linux：`./burst` 无参数打开 GUI，依赖桌面环境自带的 `libGL`/`libX11`。

**构建**（`option(BUILD_GUI ON)` 默认开启）：

```bash
# Windows（MSYS2/mingw64）
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release .
cmake --build build --target burst      # 产出 burst.exe

# Linux（需 X11 开发包：libgl1-mesa-dev libx11-dev libxrandr-dev
#   libxinerama-dev libxcursor-dev libxi-dev）
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build --target burst      # 产出 burst
```

**已支持**：

- 🎨 **Atom One Dark 暗色主题**；Windows：无边框窗口 + Mac 风格按钮（最小化/最大化/关闭）；Linux：系统标题栏
- ⚡ 多线程分片下载（1~8 线程可选，默认随 CPU 核数自适应）
- 🎬 **视频下载**（B站/YouTube 等）：解析 → 下载视频轨/音频轨（分片并行）→ 自动合并，四阶段状态实时显示（解析中/下载视频轨/下载音频轨/合并中）
- 🗂️ **任务列表 + 每任务控制**：**停止** = 取消并**保留断点**（`.curlbolt.part` 分片级元数据，继续时仅续传未完成分片）；**继续** = 重新入队续传；**删除** = 未完成任务硬删（取消 + 删产物 + 移除行）；**移除** = 清理已完成/失败行；URL 添加成功后自动清空
- 📚 **多任务队列**：下载中可继续添加任务；4 个并发槽位共享一个引擎（每任务保留完整分片连接数在途）
- 📊 **3D 圆柱体总进度条**（电池格分片效果）：完成格绿色、当前格增长、格线 5px、格内显示分片完成度%，hover 显示分片速度；右下角显示总体百分比 + 总速度
- 📁 保存路径填目录即可（文件名自动取自 URL）；"浏览…"按钮：Windows 原生目录对话框 / Linux 内置目录浏览器（零依赖）
- ⚡ 支持 **迅雷专用链接**（`thunder://` 自动解码）
- 🌐 中英双语界面（菜单栏显示目标语言提示：中文界面 → `language`，英文界面 → `中文`）
- 🪟 Windows：无边框窗口全边缘 resize（左右下边 + 四角）+ 最小 640×480 + DPI 感知；Linux：系统标题栏（可拖动/缩放）
- 💾 内嵌字体与第三方库，随仓库分发，无需额外安装

---

## ⚠️ 注意事项 Notes

- 需要服务器支持 **HTTP Range**（静态文件服务器通常都支持；不支持时自动退化单线程）；
- **视频模式**支持 B站/YouTube 等主流网站；B站 720p+ 高清流需登录态（`--cookies-from-browser chrome`）；
- **断点续传**基于 `.curlbolt.part` 分片级元数据（含 ETag/Last-Modified 校验，远程内容变更自动全量重下）；
- 超时机制：默认 60 秒无进展自动中断，`--timeout N` 调整，`--no-timeout` 禁用。

---

## 🔐 发布签名与安全校验 Release Signing

Windows 版未做 Authenticode 商业签名（开源项目未购买付费证书），首次运行可能弹出 SmartScreen「Windows 已保护你的电脑」提示，点击 **更多信息 → 仍要运行** 即可放行。

每个发布文件附带 GPG 签名（`.sig` 文件）与校验清单 `SHA256SUMS.txt`，可验证真实性与完整性：

```bash
gpg --import burst-public-key.asc          # 导入发布公钥（每个 Release 也附带）
gpg --verify burst-windows-x86_64.zip.sig burst-windows-x86_64.zip  # 验签
sha256sum -c SHA256SUMS.txt                # 校验哈希
```

发布签名公钥指纹：`1C5F F3B1 7A21 6A32 1AE1 7566 4A8E 6102 2774 824C`

---

## ⚠️ 免责声明 Disclaimer

本工具仅用于下载**您有权获取**的内容（如个人备份、学习研究、公有领域或 CC 协议素材）。请勿用于下载、传播或商用受版权保护的内容，也不得用于任何违法行为。**使用者应自行承担全部法律责任，作者不对任何使用行为负责。**

This tool is intended only for downloading content **you have the right to obtain** (e.g. personal backups, study & research, public domain or CC-licensed material). Do not use it to download, redistribute or commercially exploit copyrighted content, nor for any unlawful purpose. **Users bear full legal responsibility; the author assumes no liability for any use of this tool.**

---

## 📄 License

本项目采用 **MIT License**（Copyright © 2026 ErnestAgel），允许自由使用、修改、商用与分发。  
**This project is licensed under the MIT License.**

内置 **FFmpeg**（[LGPL v2.1+](https://www.ffmpeg.org/legal.html)）静态库，仅用于音视频封装合并（remux），未作修改；LGPL 许可要求下，本仓库随附全部源码与链接说明。

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
