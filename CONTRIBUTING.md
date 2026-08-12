# Contributing to Burst Download

Thanks for your interest! Burst Download is an open-source, cross-platform download manager. Contributions of any size — bug reports, docs, translations, code — are welcome.

## Quick links

- [README](README.md) — features, usage and build instructions
- [中文 README](README_ZH.md)
- [Issues](https://github.com/ErnestAgel/burst-download/issues) — bugs and feature requests
- [Releases](https://github.com/ErnestAgel/burst-download/releases) — prebuilt binaries

## Reporting bugs & suggesting features

Please use the issue templates:

- **Bug report** — include Burst version (`--version`), platform, build type (Release binary vs. built from source), steps to reproduce, and any relevant logs (`download.log`, `crash.log` or terminal output).
- **Feature request** — describe the problem you're solving, the proposed behavior, and any alternatives you considered.

## Development setup

Prerequisites:

- CMake ≥ 3.10
- Linux: GCC (g++), `make`; for the GUI, X11/OpenGL dev packages (`libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`)
- Windows: MSYS2 / mingw64 with GCC and `mingw32-make` (the project uses GCC-specific flags, MSVC is not supported)

All third-party libraries (libcurl, OpenSSL, FFmpeg, Python 3.11, Dear ImGui, GLFW) ship inside `third_party/` — no development packages needed for the core build.

```bash
# Debug (default; links dynamic libraries for easier debugging)
cmake -B build . && cmake --build build --target burst

# Release (static, single-file binary — what we ship)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=OFF .
cmake --build build --target burst

# Windows (MSYS2/mingw64)
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=OFF .
cmake --build build --target burst
```

## Smoke-testing your change

```bash
./build/burst --version
./build/burst --help
./build/burst https://example.com/some-file.bin -o /tmp/test.bin -t 8 --no-timeout
```

If you touched the video path, also try a short `--video` download (e.g. a Bilibili or YouTube URL you're allowed to download). CI runs a build + `--version`/`--help` smoke test on Linux and Windows for every PR.

## Code style

- C++17, UTF-8 source files, comments in English.
- 4-space indentation, no tabs; keep lines readable (≤ 80 cols where practical).
- Member variables use the `m_` prefix; globals `g_`; statics `s_`; types start with `T`, classes with `C`.
- Use project-provided portable types (`u8/u16/u32/u64`, `s8/s16/s32/s64`) instead of bare `int`/`long` for logic.
- Initialize all locals; never return pointers to automatic storage; check return values.
- Prefer the project's own assertion macro over `assert`.
- Keep functions small (≤ 100 lines, ≤ 6 parameters), nesting ≤ 3, one statement per line.

## Commit & PR conventions

- Use [Conventional Commits](https://www.conventionalcommits.org/) style matching the existing history: `feat:`, `fix:`, `docs:`, `build:`, `refactor:`, `test:`, `chore:`.
- One logical change per PR; keep the diff focused.
- Update the README (both languages) or docs when user-visible behavior changes.
- Make sure CI passes; if you can't run a platform, say so in the PR description.

## Pull request checklist

- [ ] Builds successfully on at least one platform (Linux or Windows)
- [ ] Smoke-tested `--version` and `--help`
- [ ] `download.log` / crash behavior checked if the change touches the download or GUI paths
- [ ] README/docs updated if user-visible behavior changed
- [ ] Commit messages follow Conventional Commits

## License

By contributing, you agree that your contributions are licensed under the same [MIT License](LICENSE) as the project.
