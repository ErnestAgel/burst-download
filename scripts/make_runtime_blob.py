#!/usr/bin/env python3
"""构建运行时打包文件（blob）。

用法: make_runtime_blob.py <src_dir> <out_blob>
输出: 最后一行 "hash size"（FNV-1a64 十六进制 + blob 字节数），供打包脚本写入 footer。

blob 格式（与 src/embedded_runtime.cpp 对应）：
  u32 条目数
  逐条: u16 路径长度(UTF-8) | 路径(相对, 正斜杠) | u64 数据长度 | 数据
"""

import os
import struct
import sys


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main() -> None:
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    src, out = sys.argv[1], sys.argv[2]
    if not os.path.isdir(src):
        print("source dir not found: %s" % src)
        sys.exit(1)

    entries = []
    for root, dirs, files in os.walk(src):
        dirs.sort()
        files.sort()
        for name in files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, src).replace("\\", "/")
            with open(full, "rb") as fh:
                data = fh.read()
            entries.append((rel, data))
    entries.sort(key=lambda e: e[0])

    blob = bytearray()
    blob += struct.pack("<I", len(entries))
    for rel, data in entries:
        path = rel.encode("utf-8")
        if len(path) > 0xFFFF:
            print("path too long: %s" % rel)
            sys.exit(1)
        blob += struct.pack("<H", len(path))
        blob += path
        blob += struct.pack("<Q", len(data))
        blob += data

    with open(out, "wb") as fh:
        fh.write(blob)
    print("%016x %d" % (fnv1a64(bytes(blob)), len(blob)))


if __name__ == "__main__":
    main()
