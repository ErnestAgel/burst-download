#!/usr/bin/env python3
"""跨平台发布打包：pyc 化运行时 + 内嵌 blob + Windows zip（CI 用）。

逻辑与 scripts/build-release.ps1 的打包段保持一致（本地脚本为兜底，CI 走本文件）：
  - 运行时 pyc 化：compileall -b 生成 .pyc，删除 .py（保留 yt_dlp/version.py）、
    __pycache__、.parser_last_check、.result_*.json；按平台保留/剔除原生扩展。
  - 单文件发布（linux-*）：二进制 + 内嵌运行时 blob（BURSTARC footer，32 字节）。
  - Windows：exe 与 .pyd 导入名 python311.dll -> bd311.dll；zip = exe + 运行 dll。

用法:
  python package_release.py --platform linux-x86_64 --exe build/burst \
      --runtime third_party/python/runtime --out burst-linux-x86_64
  python package_release.py --platform windows-x86_64 --exe build/burst.exe \
      --runtime third_party/python/runtime --build-dir build --out burst-windows-x86_64.zip

注意：必须用 Python 3.11 运行本脚本（compileall 字节码须匹配 3.11 运行时）。
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
MAKE_BLOB = os.path.join(SCRIPT_DIR, "make_runtime_blob.py")
FOOTER_MAGIC = b"BURSTARC"
FOOTER_END = b"BURSTEND"
FOOTER_SIZE = len(FOOTER_MAGIC) + 8 + 8 + len(FOOTER_END)  # 32

OLD_DLL = b"python311.dll"
NEW_DLL = b"bd311.dll"


def _walk_files(root):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            yield os.path.join(dirpath, name)


def convert_to_pyc_only(src, dst, platform):
    """pyc 化运行时。platform: linux-x86_64 / linux-aarch64 / windows-x86_64。"""
    shutil.copytree(src, dst, dirs_exist_ok=True)
    subprocess.run(
        [sys.executable, "-m", "compileall", "-q", "-f", "-b", dst],
        check=True,
    )

    # 删除 .py（保留 yt_dlp/version.py，版本号需可从源码读取）
    for full in _walk_files(dst):
        if full.endswith(".py"):
            rel = os.path.relpath(full, dst).replace("\\", "/")
            if rel != "yt_dlp/version.py":
                os.remove(full)

    # 删除 __pycache__
    for dirpath, dirnames, _filenames in os.walk(dst, topdown=False):
        if os.path.basename(dirpath) == "__pycache__":
            shutil.rmtree(dirpath)
        elif "__pycache__" in dirnames:
            shutil.rmtree(os.path.join(dirpath, "__pycache__"), ignore_errors=True)

    # 平台原生扩展：windows 留 .pyd 去 .so；linux 留 .so 去 .pyd；
    # linux-aarch64 无可用原生扩展，两者都剔除（纯 pyc）。
    drop_exts = [".so" if platform == "windows-x86_64" else ".pyd"]
    if platform == "linux-aarch64":
        drop_exts.append(".so")
    for full in _walk_files(dst):
        if full.endswith(tuple(drop_exts)):
            os.remove(full)

    # yt_dlp 缓存/状态文件不随包
    for full in _walk_files(dst):
        name = os.path.basename(full)
        if name == ".parser_last_check" or (name.startswith(".result_") and name.endswith(".json")):
            os.remove(full)


def patch_import_name(path):
    """二进制内 python311.dll -> bd311.dll（Windows 导入名，等长补零）。"""
    with open(path, "rb") as fh:
        data = fh.read()
    if OLD_DLL not in data:
        return False
    data = data.replace(OLD_DLL, NEW_DLL + b"\x00" * (len(OLD_DLL) - len(NEW_DLL)))
    with open(path, "wb") as fh:
        fh.write(data)
    return True


def _make_blob(assets_dir, blob_path):
    res = subprocess.run(
        [sys.executable, MAKE_BLOB, assets_dir, blob_path],
        capture_output=True,
        text=True,
    )
    if res.returncode != 0:
        raise RuntimeError("make_runtime_blob 失败: %s" % res.stderr.strip())
    lines = [ln for ln in res.stdout.splitlines() if ln.strip()]
    if not lines:
        raise RuntimeError("make_runtime_blob 无输出")
    parts = lines[-1].split()
    if len(parts) != 2:
        raise RuntimeError("make_runtime_blob 输出异常: %s" % res.stdout.strip())
    return int(parts[0], 16), int(parts[1])


def embed_runtime(exe_path, assets_dir):
    """追加运行时 blob + 32 字节 footer，并回读校验。"""
    blob_fd, blob_path = tempfile.mkstemp(prefix="burst-blob-")
    os.close(blob_fd)
    try:
        blob_hash, blob_len = _make_blob(assets_dir, blob_path)
        with open(blob_path, "rb") as fh:
            blob = fh.read()
        with open(exe_path, "ab") as fh:
            fh.write(blob)
            fh.write(FOOTER_MAGIC)
            fh.write(struct.pack("<Q", blob_len))
            fh.write(struct.pack("<Q", blob_hash))
            fh.write(FOOTER_END)
        with open(exe_path, "rb") as fh:
            fh.seek(-FOOTER_SIZE, os.SEEK_END)
            tail = fh.read(FOOTER_SIZE)
        if tail[:8] != FOOTER_MAGIC or tail[-8:] != FOOTER_END:
            raise RuntimeError("运行时 footer 回读校验失败")
    finally:
        os.unlink(blob_path)


def package_linux(exe_path, runtime_dir, out_path, platform):
    tmp_assets = tempfile.mkdtemp(prefix="burst-assets-")
    try:
        convert_to_pyc_only(runtime_dir, tmp_assets, platform)
        shutil.copy2(exe_path, out_path)
        embed_runtime(out_path, tmp_assets)
    finally:
        shutil.rmtree(tmp_assets, ignore_errors=True)


def package_windows_zip(exe_path, build_dir, runtime_dir, out_zip):
    stage = tempfile.mkdtemp(prefix="burst-stage-")
    try:
        assets = os.path.join(stage, "assets")
        convert_to_pyc_only(runtime_dir, assets, "windows-x86_64")

        # .pyd 导入名 python311.dll -> bd311.dll
        dynload = os.path.join(assets, "lib-dynload")
        if os.path.isdir(dynload):
            for name in sorted(os.listdir(dynload)):
                if name.endswith(".pyd"):
                    patch_import_name(os.path.join(dynload, name))

        # exe：改名 + 内嵌运行时
        exe_copy = os.path.join(stage, os.path.basename(exe_path))
        shutil.copy2(exe_path, exe_copy)
        patch_import_name(exe_copy)
        embed_runtime(exe_copy, assets)

        # 运行 dll 平铺；python311.dll -> bd311.dll
        for name in sorted(os.listdir(build_dir)):
            if name.endswith(".dll"):
                shutil.copy2(os.path.join(build_dir, name), os.path.join(stage, name))
        old_dll = os.path.join(stage, "python311.dll")
        if os.path.exists(old_dll):
            os.rename(old_dll, os.path.join(stage, "bd311.dll"))

        shutil.rmtree(assets)

        if os.path.exists(out_zip):
            os.remove(out_zip)
        try:
            compression = zipfile.ZIP_DEFLATED
        except Exception:
            compression = zipfile.ZIP_STORED
        with zipfile.ZipFile(out_zip, "w", compression) as zf:
            for name in sorted(os.listdir(stage)):
                zf.write(os.path.join(stage, name), arcname=name)

        # 校验：必须含 bd311.dll；不得含 python311.dll / 运行时文件
        with zipfile.ZipFile(out_zip) as zf:
            names = zf.namelist()
        if "bd311.dll" not in names:
            raise RuntimeError("zip 缺少 bd311.dll")
        if "python311.dll" in names:
            raise RuntimeError("zip 仍包含 python311.dll")
        bad = [
            n
            for n in names
            if n.startswith("assets/")
            or n.startswith("assets\\")
            or n.endswith((".py", ".so", ".pyd", ".pyc"))
        ]
        if bad:
            raise RuntimeError("zip 不应包含运行时文件: %s" % ", ".join(bad))
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description="burst 发布打包")
    parser.add_argument("--platform", required=True,
                        choices=["linux-x86_64", "linux-aarch64", "windows-x86_64"])
    parser.add_argument("--exe", required=True, help="已构建的 burst 可执行文件")
    parser.add_argument("--runtime", required=True, help="third_party/python/runtime 目录")
    parser.add_argument("--out", required=True, help="输出文件（linux 单文件 / windows zip）")
    parser.add_argument("--build-dir", default=None, help="windows: 含运行 dll 的构建目录")
    args = parser.parse_args()

    exe_path = os.path.abspath(args.exe)
    runtime_dir = os.path.abspath(args.runtime)
    out_path = os.path.abspath(args.out)
    if not os.path.isfile(exe_path):
        parser.error("exe 不存在: %s" % exe_path)
    if not os.path.isdir(runtime_dir):
        parser.error("runtime 目录不存在: %s" % runtime_dir)

    if args.platform == "windows-x86_64":
        if not args.build_dir:
            parser.error("windows 平台需要 --build-dir")
        package_windows_zip(exe_path, os.path.abspath(args.build_dir), runtime_dir, out_path)
    else:
        package_linux(exe_path, runtime_dir, out_path, args.platform)

    print("packaged -> %s (%d bytes)" % (args.out, os.path.getsize(out_path)))


if __name__ == "__main__":
    main()
