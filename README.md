<div align="center">

# ⚡ curlbolt

**多线程分片下载器 · 支持视频下载**  
**Multi-threaded chunked downloader with video download support**

![C/C++](https://img.shields.io/badge/language-C%2FC%2B%2B-blue?style=for-the-badge)
![libcurl](https://img.shields.io/badge/libcurl-green?style=for-the-badge&logo=curl&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-x86_64%20%7C%20ARM64-orange?style=for-the-badge&logo=linux&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-x86_64-blue?style=for-the-badge&logo=windows&logoColor=white)
![CMake](https://img.shields.io/badge/build-CMake-yellow?style=for-the-badge&logo=cmake&logoColor=white)

[![Stars](https://img.shields.io/github/stars/ErnestAgel/curlbolt?style=flat-square)](https://github.com/ErnestAgel/curlbolt/stargazers)
[![Forks](https://img.shields.io/github/forks/ErnestAgel/curlbolt?style=flat-square)](https://github.com/ErnestAgel/curlbolt/network)
[![Last commit](https://img.shields.io/github/last-commit/ErnestAgel/curlbolt?style=flat-square)](https://github.com/ErnestAgel/curlbolt/commits/main)

> 🎬 **视频下载**：一条命令下载 B站 / YouTube 等主流网站的视频，多线程分片下载
> ⚡ **多线程加速**：HTTP Range 分片，1~10 线程并发，榨干带宽
> 📦 **断点续传**：中断后从断点继续，不重头来
> 🖥 **三平台单文件**：Linux x86_64 / ARM64 / Windows，Release 静态编译零依赖

</div>

---

## ✨ 特性 Features

| | 说明 Description |
|---|---|
| 🎬 **视频下载** | `--video` 模式：输入视频网页 URL，自动解析媒体流直链（B站/YouTube 等主流网站），多线程分片下载 |
| ⚡ **多线程并发** | `-t` 1~10 线程，HTTP Range 分片，最后一个分片负责余数 |
| 📦 **断点续传** | 自动检测本地已存在文件并从断点继续；服务器不支持 Range 时自动退化为单线程 |
| ⏱ **超时中断与日志** | `--timeout` / `--no-timeout` 控制；超时中断、失败详情写入 `download.log` |
| 🍪 **Cookie 支持** | `--cookies-from-browser` 读浏览器登录态（B站 720p+ 高清流）、`--cookie` 手动指定 |
| 🛡 **防盗链 Referer** | 自动携带视频页 Referer，B站等视频流防 403 |
| 🖥 **跨平台** | Linux x86_64 / Linux aarch64 / Windows；**Debug（动态库调试）+ Release（静态单文件发布）双构建** |

---

## 🎬 视频下载（亮点 Video Download）

一条命令，任意支持网站：

```bash
# 下载 B站视频（自动解析 + 多线程分片下载）
./curlbolt --video "https://www.bilibili.com/video/BVxxxxxxxx" -o movie

# B站高清 720p+（需要浏览器已登录 B站）
./curlbolt --video "https://www.bilibili.com/video/BVxxxxxxxx" -o movie --cookies-from-browser chrome

# YouTube 等主流视频网站
./curlbolt --video "https://www.youtube.com/watch?v=xxxxx" -o clip
```

- 支持 B站、YouTube 等主流视频网站，视频网页地址直接可用
- 音视频分离流（DASH）自动下载为 `movie.mp4`（视频轨）+ `movie.m4a`（音频轨），用 ffmpeg 一键合并：
  ```bash
  ffmpeg -i movie.mp4 -i movie.m4a -c copy movie_full.mp4
  ```

---

## 🚀 快速开始 Quick Start

```bash
./curlbolt <url> [-o filename] [-t threads] [--timeout sec] [--no-timeout]
./curlbolt --video <video-url> [-o basename] [-t threads] [--timeout sec]
```

```bash
# 下载文件（8 线程，30 秒无进展超时）
./curlbolt https://example.com/file.iso -o file.iso -t 8 --timeout 30

# 下载视频
./curlbolt --video "https://www.bilibili.com/video/BVxxxx" -o movie

# 强制下载不自动中断
./curlbolt https://example.com/file.iso -o file.iso --no-timeout

# 查看帮助
./curlbolt -h
```

终端实时输出**进度 / 速率 / 剩余时间**：

```
percent: 42% speed: 3.20 MB/s ETA: 00:05:12
```

---

## 🔨 构建 Build

项目自带三平台 libcurl 库（`lib/`），无需安装 libcurl 开发包。

**Debug（默认）**：链接动态库，便于 gdb 调试

```bash
cmake -B build . && cmake --build build        # Linux
cmake -B build -G "MinGW Makefiles" .          # Windows
```

**Release**：链接静态库，产出**单文件可执行程序**（无动态依赖，用于发布）

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

## 📁 目录结构 Project Structure

```
curlbolt/
├── CMakeLists.txt        # 构建脚本（Debug 动态 / Release 静态单文件）
├── include/
│   ├── Ccurl.h           # 下载器封装类声明
│   ├── video.h           # 视频直链解析
│   └── curl/             # libcurl 头文件
├── lib/                  # 项目自带的 libcurl 库（三平台 × 动态/静态）
│   ├── linux-x86_64/     # Linux x86_64（libcurl.so / libcurl.a）
│   ├── linux-aarch64/    # Linux ARM64（libcurl.so / libcurl.a）
│   └── windows-x86_64/   # Windows（libcurl-4.dll / libcurl.a）
├── scripts/
│   └── build-static-libs.sh  # 重新构建三平台静态 libcurl 库
├── src/
│   ├── Ccurl.cpp         # 多线程分片下载实现
│   ├── video.cpp         # 视频直链解析实现
│   └── main.cpp          # 命令行入口
└── zsync                 # zsync 二进制
```

---

## ⚠️ 注意事项 Notes

- 需要服务器支持 **HTTP Range**（静态文件服务器通常都支持；不支持时自动退化单线程）；
- **视频模式**支持 B站/YouTube 等主流网站；B站 720p+ 高清流需登录态（`--cookies-from-browser chrome`）；
- **断点续传**仅比较文件大小，远程内容变更时建议删除本地文件重新下载；
- 超时机制：默认 60 秒无进展自动中断，`--timeout N` 调整，`--no-timeout` 禁用。

---

## 📄 License

本项目采用 **MIT License**（Copyright © 2026 ErnestAgel），允许自由使用、修改、商用与分发。  
**This project is licensed under the MIT License.**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
