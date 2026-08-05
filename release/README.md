# curlbolt v1.0.0

**多线程分片下载器 · 支持视频下载** —— 三平台静态单文件发布

对应源码版本：`v1.0.0`（[GitHub](https://github.com/ErnestAgel/curlbolt/tree/v1.0.0)）

## 平台与文件（每个平台一个文件，静态编译，**无需任何动态库**）

| 平台 | 文件 |
|------|------|
| Linux x86_64 | `curlbolt-linux-x86_64` |
| Linux aarch64（ARM64） | `curlbolt-linux-aarch64` |
| Windows x86_64 | `curlbolt-windows-x86_64.exe` |

## 运行方式

**Linux**：

```bash
chmod +x curlbolt-linux-x86_64        # 或 linux-aarch64
./curlbolt-linux-x86_64 -h
```

**Windows**：直接命令行运行 `curlbolt-windows-x86_64.exe`。

## 功能

- 🎬 **视频下载** `--video`：输入视频网页 URL 直接下载（B站/YouTube 等主流网站），多线程分片
- ⚡ 多线程分片下载（`-t` 1~10，HTTP Range）
- 📦 断点续传（自动检测本地文件，从断点继续）
- ⏱ 超时中断与日志（`--timeout` / `--no-timeout`，详情写入 `download.log`）
- 🍪 Cookie 支持（`--cookies-from-browser chrome` / `--cookie "..."`）
- 🛡 防盗链 Referer（B站等视频流）

## 用法示例

```bash
# 下载文件
./curlbolt https://example.com/file.iso -o file.iso -t 10 --timeout 30

# 下载 B站视频
./curlbolt --video "https://www.bilibili.com/video/BVxxxx" -o movie

# B站高清（浏览器已登录）
./curlbolt --video "https://www.bilibili.com/video/BVxxxx" -o movie --cookies-from-browser chrome
```

## 依赖

- 静态单文件，无 libcurl/openssl 动态库依赖
- 视频模式需系统具备视频解析组件（详见 `curlbolt -h`）

## 校验

```bash
sha256sum curlbolt-linux-x86_64 curlbolt-linux-aarch64
certutil -hashfile curlbolt-windows-x86_64.exe SHA256   # Windows
```
