# curl_download v1.0.0

基于 libcurl 的多线程分片下载器（支持视频下载）—— 跨平台预编译包

对应源码版本：`v1.0.0`（[GitHub](https://github.com/ErnestAgel/curl_download/tree/v1.0.0)）

## 支持平台

| 平台 | 目录 | 文件 |
|------|------|------|
| Linux x86_64 | `linux-x86_64/` | `curl_download` + `libcurl.so.4` |
| Linux aarch64（ARM64） | `linux-aarch64/` | `curl_download` + `libcurl.so.4` |
| Windows x86_64 | `windows-x86_64/` | `curl_download.exe` + `libcurl-4.dll` |

## 运行方式

**Linux**：

```bash
cd linux-x86_64            # 或 linux-aarch64
chmod +x curl_download
export LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH   # 使用同目录自带的 libcurl.so.4
./curl_download -h
```

**Windows**：`curl_download.exe` 与 `libcurl-4.dll` 保持同目录，命令行运行或双击。

## 功能

- ⚡ 多线程分片下载（`-t` 1~10，HTTP Range）
- 📦 断点续传（自动检测本地文件，从断点继续）
- ⏱ 超时中断与日志（`--timeout N` / `--no-timeout`，详情写入 `download.log`）
- 🎬 视频下载（`--video`，经 yt-dlp 支持 B站/YouTube 等 1000+ 网站）
- 🍪 Cookie 支持（`--cookies-from-browser chrome` 读浏览器登录态 / `--cookie "..."` 手动指定）
- 🛡 防盗链 Referer（B站等视频流）

## 用法示例

```bash
# 普通文件下载（10 线程，30 秒无进展超时）
./curl_download https://example.com/file.iso -o file.iso -t 10 --timeout 30

# 下载 B站视频（默认清晰度）
./curl_download --video "https://www.bilibili.com/video/BVxxxx" -o movie

# 下载 B站高清视频（需浏览器已登录 B站）
./curl_download --video "https://www.bilibili.com/video/BVxxxx" -o movie --cookies-from-browser chrome

# 强制下载不自动中断
./curl_download https://example.com/file.iso -o file.iso --no-timeout
```

## 依赖

- **视频模式**需要系统已安装 [yt-dlp](https://github.com/yt-dlp/yt-dlp)（`pip install yt-dlp` 或官网单文件）
- Linux 包内已含对应架构的 `libcurl.so.4`，运行时建议 `LD_LIBRARY_PATH` 指向包目录；需系统 glibc 及 libcurl 依赖的基础库
- Windows 包自含 `libcurl-4.dll`，无额外依赖

## 校验

```bash
sha256sum linux-x86_64/curl_download linux-aarch64/curl_download linux-x86_64/libcurl.so.4 linux-aarch64/libcurl.so.4
certutil -hashfile windows-x86_64/curl_download.exe SHA256   # Windows
```
