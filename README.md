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

依赖：**CMake ≥ 3.10**、**gcc/g++**、**libcurl**、**pthread**（Linux）

```bash
cmake .
make
```

构建产物：

| 目标 Target | 说明 Description |
|---|---|
| `curl_download` | C++ 版主程序（`src/*.cpp`） |
| `demo` | 纯 C 版示例（`src/demo.c`） |

> 仓库自带 `include/curl/` 头文件与 `lib/libcurl.so`，即使系统未安装 libcurl 开发包也可直接编译。

---

## 🚀 使用 Usage

运行程序：

```bash
./curl_download
```

下载地址在 `src/main.cpp` 中配置，修改后重新编译即可下载其他文件：

```cpp
ptr->Init("https://releases.ubuntu.com/20.04/ubuntu-20.04.6-live-server-amd64.iso.zsync", "./test");
```

运行后终端会实时输出下载进度：

```
percent: 1%
percent: 2%
...
percent: 100%
```

---

## ⚠️ 注意事项 Notes

- 需要服务器支持 **HTTP Range**（静态文件服务器通常都支持）；
- 仅支持 **Linux**（依赖 `sys/mman.h`、`unistd.h`、`pthread`）；
- 分片数由 `include/Ccurl.h` 中的 `MaxThread` 宏控制，默认为 10（实际起 11 个线程，最后一个线程负责余数部分）。

> **Requires HTTP Range support on the server · Linux only · thread count is controlled by the `MaxThread` macro (default 10).**

---

## 📄 License

未指定许可证。  
**No license specified.**
