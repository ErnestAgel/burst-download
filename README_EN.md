<div align="center">

[🇬🇧 English](README_EN.md) · [🇨🇳 中文](README.md)

# ⚡ curlbolt

**Multi-threaded chunked downloader with video download support**

![C/C++](https://img.shields.io/badge/language-C%2FC%2B%2B-blue?style=for-the-badge)
![libcurl](https://img.shields.io/badge/libcurl-green?style=for-the-badge&logo=curl&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-x86_64%20%7C%20ARM64-orange?style=for-the-badge&logo=linux&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-x86_64-blue?style=for-the-badge&logo=windows&logoColor=white)
![CMake](https://img.shields.io/badge/build-CMake-yellow?style=for-the-badge&logo=cmake&logoColor=white)

[![Stars](https://img.shields.io/github/stars/ErnestAgel/curlbolt?style=flat-square)](https://github.com/ErnestAgel/curlbolt/stargazers)
[![Forks](https://img.shields.io/github/forks/ErnestAgel/curlbolt?style=flat-square)](https://github.com/ErnestAgel/curlbolt/network)
[![Last commit](https://img.shields.io/github/last-commit/ErnestAgel/curlbolt?style=flat-square)](https://github.com/ErnestAgel/curlbolt/commits/main)

> 🎬 **Video download**: one command to download videos from Bilibili / YouTube and other popular sites
> ⚡ **Multi-threading**: HTTP Range chunking with 1–10 threads to saturate bandwidth
> 📦 **Resume support**: continue from where it stopped instead of restarting
> 🖥 **Three-platform builds**: Linux x86_64 / ARM64 / Windows; Linux Release is a static single file with zero deps, Windows Release ships the embedded-Python runtime DLLs (copied to the exe dir by CMake)

</div>

---

## ✨ Features

| | Description |
|---|---|
| 🎬 **Video download** | `--video` mode: pass a video page URL, the built-in parser resolves the media stream (Bilibili / YouTube and more), then downloads with multi-threaded chunking; DASH streams are **auto-merged** into one file (MP4 / WebM containers) |
| ⚡ **Multi-threaded** | `-t` 1–10 threads, HTTP Range chunking, the last chunk absorbs the remainder |
| 📦 **Resume** | Automatically detects an existing local file and resumes; falls back to single-thread when the server lacks Range support |
| ⏱ **Timeout & logging** | `--timeout` / `--no-timeout` control; timeouts and failure details are written to `download.log` |
| 🍪 **Cookie support** | `--cookies-from-browser` reads browser login state (Bilibili 720p+ streams), `--cookie` for manual cookies |
| 🛡 **Referer** | Automatically sends the video page Referer to avoid anti-hotlinking 403s |
| 🖥 **Cross-platform** | Linux x86_64 / Linux aarch64 / Windows; **Debug (dynamic libs) + Release (static single-file) dual builds**; on Windows the embedded-Python runtime DLLs must sit next to the exe (copied automatically by CMake) |

---

## 💡 Why curlbolt?

**Compared with traditional download tools (curl / wget):**

| | curl / wget | curlbolt |
|---|---|---|
| Connections | single-threaded, single connection | 1–10 concurrent connections |
| Bandwidth | limited by TCP slow start / congestion window; often can't saturate high-bandwidth, high-latency links | parallel connections approach the bandwidth ceiling |
| Resume | manual `curl -C -` | automatic detection & resume |
| Video download | ❌ not supported | `--video` auto-resolves stream URLs |
| Logging / timeout | none | timeout interrupt + `download.log` |

**Good fit ✅**

- GitHub Releases, software mirrors, CDNs and other Range-capable static resources
- Large files: ISOs, archives, datasets, model weights
- Direct video stream downloads (Bilibili / YouTube)

**Not a fit ❌**

- **Account-rate-limited** cloud drives (e.g. Baidu Pan): the server throttles per account, so more threads don't increase total throughput
- Servers without Range support (automatically falls back to single-thread)
- Private drives requiring login + dynamic signatures (no direct links)

---

## ⚙️ How It Works

![Multi-thread chunked download](docs/how-it-works.en.svg)

1. **HTTP Range chunking**: send requests like `Range: bytes=0-26214399` to split a large file into N chunks;
2. **Multi-threaded concurrency**: N threads each hold an independent TCP connection and pull their own chunk simultaneously;
3. **Why it's faster**: a single TCP connection is limited by **slow start + congestion window**, so the actual rate often stays below the bandwidth ceiling (especially on high-latency links, e.g. cross-border downloads); parallel connections add up and approach the ceiling;
4. **Prerequisites**: the server must support Range and **not throttle per account** — static file servers / CDNs naturally qualify; account-limited drives like Baidu Pan don't, so more threads change nothing;
5. **Final assembly**: chunks are written to disk and merged into the complete file (the last chunk absorbs the remainder).

---

## 🎬 Video Download

One command for any supported site:

```bash
# Download a Bilibili video (auto-resolve + multi-threaded download)
./curlbolt --video "https://www.bilibili.com/video/BVxxxxxxxx" -o movie

# Bilibili 720p+ (requires being logged in to Bilibili in the browser)
./curlbolt --video "https://www.bilibili.com/video/BVxxxxxxxx" -o movie --cookies-from-browser chrome

# YouTube and other popular video sites
./curlbolt --video "https://www.youtube.com/watch?v=xxxxx" -o clip
```

- Works with Bilibili, YouTube and other popular sites — just paste the video page URL
- DASH streams: video + audio tracks are downloaded and **auto-merged** into one file (in-process, no external tools). The output container follows the video codec: VP9/AV1 → `.mkv`, otherwise → `.mp4`. Temporary tracks are deleted after a successful merge.
- **Overwrite protection**: without `-o`, names come from the URL plus a timestamp (e.g. `10Mb_20260807_123456.dat`, `BVxxxx_20260807_123456_full.mkv`); with explicit `-o`, if the target already exists a timestamp is appended instead of overwriting.

---

## 🚀 Quick Start

```bash
./curlbolt <url> [-o filename] [-t threads] [--timeout sec] [--no-timeout]
./curlbolt --video <video-url> [-o basename] [-t threads] [--timeout sec]
```

```bash
# Download a file (8 threads, 30s no-progress timeout)
./curlbolt https://example.com/file.iso -o file.iso -t 8 --timeout 30

# Download a video
./curlbolt --video "https://www.bilibili.com/video/BVxxxx" -o movie

# Force download without auto-interruption
./curlbolt https://example.com/file.iso -o file.iso --no-timeout

# Show help
./curlbolt -h
```

Live **progress / speed / ETA** output:

```
percent: 42% speed: 3.20 MB/s ETA: 00:05:12
```

---

## 🔨 Build

The project ships prebuilt libcurl libraries, a minimal static FFmpeg and the embedded Python runtime for all three platforms (`third_party/`) — no need to install dev packages.

**Debug (default)**: links dynamic libraries, convenient for gdb

```bash
cmake -B build . && cmake --build build        # Linux
cmake -B build -G "MinGW Makefiles" .          # Windows (MSYS2/mingw64 env, gcc and mingw32-make on PATH)
```

**Release**: links static libraries; on Linux this yields a **single-file executable** (no dynamic dependencies, for distribution). On Windows, since the Python interpreter is embedded, the DLLs under `third_party/python/windows-x86_64/dll/` must sit next to the exe (copied automatically by CMake)

```bash
# Linux (static openssl prepared by the build script)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT=/path/to/openssl \
      -DCURL_STATIC_DEPS="/path/libz.a;/path/libzstd.a" .
cmake --build build
# Windows (native Schannel TLS, no openssl needed)
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build
```

---

## 🖥️ GUI

A graphical front-end (`curlbolt-gui`) over the CLI, currently **Phase 2: File Download + Video Download modes**.

**Build** (`option(BUILD_GUI ON)` by default):

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release .
cmake --build build --target curlbolt-gui      # produces curlbolt-gui.exe on Windows
```

**Run**: launch `curlbolt-gui.exe` on Windows (runtime DLLs are copied next to the exe automatically at build time, including MinGW runtime and embedded Python DLLs).

**Supported**:

- 🎨 **Atom One Dark theme**, frameless window with Mac-style window buttons (minimize/maximize/close)
- ⚡ Multi-threaded segmented download (1~`min(10, cores)` threads selectable, per-thread progress bars)
- 🎬 **Video download** (Bilibili / YouTube etc.): parse → download video/audio tracks (parallel chunks) → auto-merge, with 4-stage status shown live (Parsing / Downloading video track / Downloading audio track / Merging)
- ⏸️ Cancel during download (resume supported); dialogs for done/cancel/error
- 📁 Save to a directory only — filename is derived from the URL automatically; Browse opens a folder picker
- ⚡ **Thunder links** (`thunder://`) decoded automatically
- 🌐 Bilingual UI (switch in Settings menu, remembered via config.ini)
- 💾 Fonts and third-party libs bundled and distributed with the repo; no extra installs

---

## ⚠️ Notes

- Requires server support for **HTTP Range** (static file servers usually support it; falls back to single-thread otherwise);
- **Video mode** works with Bilibili / YouTube and other popular sites; Bilibili 720p+ streams require login state (`--cookies-from-browser chrome`);
- **Resume** compares file size only — delete the local file and re-download if the remote content changed;
- Timeout: interrupts after 60s with no progress by default; `--timeout N` adjusts it, `--no-timeout` disables it.

---

## ⚠️ Disclaimer

This tool is intended only for downloading content **you have the right to obtain** (e.g. personal backups, study & research, public domain or CC-licensed material). Do not use it to download, redistribute or commercially exploit copyrighted content, nor for any unlawful purpose. **Users bear full legal responsibility; the author assumes no liability for any use of this tool.**

本工具仅用于下载**您有权获取**的内容（如个人备份、学习研究、公有领域或 CC 协议素材）。请勿用于下载、传播或商用受版权保护的内容，也不得用于任何违法行为。**使用者应自行承担全部法律责任，作者不对任何使用行为负责。**

---

## 📄 License

This project is licensed under the **MIT License** (Copyright © 2026 ErnestAgel) — free to use, modify, commercialize and redistribute.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
