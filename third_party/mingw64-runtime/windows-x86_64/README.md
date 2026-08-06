# third_party/mingw64-runtime — MinGW 运行时 DLL

Windows 目标的运行时依赖（MinGW-w64 工具链），构建时经 CMake POST_BUILD 自动复制到可执行文件同目录（与 `third_party/python/windows-x86_64/dll/` 同一处理方式）。

## 文件（windows-x86_64）

| 文件 | 说明 |
|---|---|
| `libgcc_s_seh-1.dll` | GCC 运行时（SEH 异常） |
| `libstdc++-6.dll` | C++ 标准库运行时 |
| `libwinpthread-1.dll` | POSIX 线程运行时（`std::thread`/`std::mutex` 依赖） |

## 来源

- 工具链：MSYS2 MINGW64，gcc/g++ 16.1.0（`D:\msys2\mingw64\bin\`）
- 非静态链接构建（Debug、GUI 冒烟等）运行时依赖；Release CLI 使用 `-static` 全静态链接，不依赖这些 DLL
- 版本随工具链更新，升级 MinGW 后需同步替换并更新下方校验和

## 校验和（md5）

```
1e59785004930fe9bd4851160353e1e7  libgcc_s_seh-1.dll
3a68f6b3de733a28c696b6ea9f4d6663  libstdc++-6.dll
571383a6b1b7b2468c033b91991a6fd6  libwinpthread-1.dll
```
