<div align="center">

# ⚡ curl_download

**基于 libcurl 的多线程分片下载工具**  
**Multi-threaded chunked downloader built on libcurl**

![C/C++](https://img.shields.io/badge/language-C%2FC%2B%2B-blue?style=for-the-badge)
![libcurl](https://img.shields.io/badge/libcurl-easy-green?style=for-the-badge&logo=curl&logoColor=white)
![Linux](https://img.shields.io/badge/platform-Linux-orange?style=for-the-badge&logo=linux&logoColor=white)
![CMake](https://img.shields.io/badge/build-CMake-yellow?style=for-the-badge&logo=cmake&logoColor=white)
![pthread](https://img.shields.io/badge/threading-pthread-lightgrey?style=for-the-badge)

[![Stars](https://img.shields.io/github/stars/ErnestAgel/curl_download?style=flat-square)](https://github.com/ErnestAgel/curl_download/stargazers)
[![Forks](https://img.shields.io/github/forks/ErnestAgel/curl_download?style=flat-square)](https://github.com/ErnestAgel/curl_download/network)
[![Last commit](https://img.shields.io/github/last-commit/ErnestAgel/curl_download?style=flat-square)](https://github.com/ErnestAgel/curl_download/commits/main)

</div>

---

## ✨ 特性 Features

| | 说明 Description |
|---|---|
| ⚡ **多线程并发下载** | 11 个线程并行拉取文件的不同字节区间，充分利用带宽 |
| 📐 **HTTP Range 分片** | 每个线程通过 `Range: bytes=start-end` 只下载属于自己的片段 |
| 🧠 **mmap 内存映射落盘** | 文件通过 `mmap` 映射到内存，分片数据直接写入对应偏移，免去额外缓冲拷贝 |
| 📊 **实时进度显示** | 进度回调汇总所有线程的下载量，打印整体下载百分比 |
| 📏 **自动探测文件大小** | 下载前通过 `HEAD`（`NOBODY`）请求获取 `Content-Length` |
| 🔧 **双语言实现** | C++ 封装类 `Ccurl` + 纯 C 参考实现 `demo.c` |

> **Multi-threaded concurrent download · HTTP Range chunks · mmap zero-copy write · live progress report · automatic file-size probing · C++ & C implementations**

---

## 🧩 工作原理 How It Works

```
  探测文件大小 (HEAD / Content-Length)
            │
            ▼
┌────────────┬────────────┬────────────┬──────────────────┐
│ Thread 0   │ Thread 1   │    ...     │ Thread 10        │
│ Range 0~N  │ Range N+1~M│            │ Range ...~EOF    │
└────────────┴────────────┴────────────┴──────────────────┘
      │             │             │             │
      └─────────────┴──────┬──────┴─────────────┘
                           ▼
            mmap 映射的文件（每线程写入各自偏移）
```

1. `get_Download_FileSize()` 发送 `HEAD` 请求，探测文件总大小；
2. 创建本地文件并用 `mmap` 映射，按 `MaxThread` 均分字节区间；
3. 每个线程持有一个 `st_EasyList`，通过 `CURLOPT_RANGE` 发起自己的分片请求；
4. 回调 `File_Write` 将收到的数据 `memcpy` 到 mmap 的对应偏移；
5. `progressFunc` 汇总各线程下载量，打印整体百分比。

> **Probe size via HEAD → split the file into ranges → each thread fetches its own range → write into the mmap'd file → aggregate progress.**

---

## 📁 目录结构 Project Structure

```
curl_download/
├── CMakeLists.txt        # 构建脚本（生成 curl_download 与 demo）
├── include/
│   ├── Ccurl.h           # Ccurl 类声明
│   └── curl/             # libcurl 头文件
├── lib/
│   └── libcurl.so*       # libcurl 动态库
├── src/
│   ├── Ccurl.cpp         # C++ 封装实现
│   ├── main.cpp          # 程序入口（演示下载）
│   └── demo.c            # 纯 C 参考实现
└── zsync                 # zsync 二进制
```

---

## 🔨 构建 Build

**Linux（x86_64 / aarch64）**——自动选择项目自带 `lib/` 下的对应架构库，无需安装 libcurl：

```bash
cmake .
make
```

**Windows（MinGW / MSVC）**——优先链接项目自带 `lib/windows-x86_64/` 的 mingw 版 libcurl，缺失时回退系统 curl（vcpkg / 官方安装包）：

```bash
cmake -G "MinGW Makefiles" .
mingw32-make
```

构建产物：

| 目标 Target | 说明 Description |
|---|---|
| `curl_download` | 主程序（`src/*.cpp`），跨平台 |
| `demo` | Linux 专属纯 C 参考实现（依赖 mmap/pthread，Windows 下不构建） |

> 项目自带 `include/curl/` 头文件与三种架构的 libcurl 库（`lib/linux-x86_64/`、`lib/linux-aarch64/`、`lib/windows-x86_64/`），无需安装 libcurl 开发包即可编译。
> Windows 构建时 CMake 会自动把 `libcurl-4.dll` 复制到可执行文件同目录（运行时依赖）。

---

## 🚀 使用 Usage

命令行传参运行（无需修改代码）：

```bash
./curl_download <url> [-o filename] [-t threads] [--timeout sec] [--no-timeout] [-h]
```

示例：

```bash
# 下载到默认文件名 ./test
./curl_download https://example.com/file.iso

# 指定文件名与线程数
./curl_download https://example.com/file.iso -o file.iso -t 8

# 设置 30 秒无进展超时自动中断
./curl_download https://example.com/file.iso -o file.iso --timeout 30

# 强制下载，不自动中断
./curl_download https://example.com/file.iso -o file.iso --no-timeout

# 查看帮助
./curl_download -h
```

终端实时输出下载进度（百分比 / 速率 / 剩余时间）：

```
percent: 24% speed: 3.20 MB/s ETA: 00:05:12
...
percent: 100%
```

**超时机制**：默认下载无进展（低于 1 字节/秒）持续 **60 秒**自动中断并记录日志；`--timeout N` 自定义秒数（0 表示不限）；`--no-timeout` 强制下载不自动中断。

**日志**：超时中断、分片失败、任务完成等事件会写入同目录的 `download.log`（含时间戳、URL、分片范围、错误信息）。

---

## ⚠️ 注意事项 Notes

- 需要服务器支持 **HTTP Range**（静态文件服务器通常都支持；不支持时会自动退化为单线程整段下载）；
- 跨平台：**Linux（x86_64 / aarch64）与 Windows**；
- 线程数用 `-t` 参数控制，范围 1~10，默认 10，最后一个分片负责余数部分；
- **断点续传**：自动检测本地已存在文件大小并从断点继续；文件已完整时直接跳过；服务器不支持 Range 时会丢弃旧文件重新下载；
- **超时机制**：默认 60 秒无进展自动中断，可用 `--timeout N` 调整或用 `--no-timeout` 禁用（详见上方"使用"章节）。

> **Requires HTTP Range support (auto fallback to single stream) · Cross-platform (Linux x86_64 / aarch64, Windows) · threads via `-t` (1-10) · resume supported · timeout via `--timeout` / `--no-timeout`.**

---

## 📄 License

未指定许可证。  
**No license specified.**
