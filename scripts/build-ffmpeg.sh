#!/usr/bin/env bash
# ============================================================================
# 一次性构建 Linux x86_64 最小化 ffmpeg 静态库 → third_party/ffmpeg/
#
# 用途：--video 模式音视频分离流（DASH）下载后的内置合并（remux，-c copy）
# 仅保留合并必需组件，其余全部裁剪：
#   - 库：libavformat + libavcodec + libavutil（静态）
#   - demuxer：mov（MP4/M4A 容器）
#   - muxer：mp4/mov
#   - protocol：file（仅本地文件合并，无需网络）
#   - 关闭：编码器/解码器/滤镜/网络/zlib/iconv 等一切非必需项
#
# Debug 与 Release 共用此静态库（与 third_party/python 的 libpython 同理）。
# 仓库只收录产物（include/ + linux-x86_64/*.a），不收录 ffmpeg 源码。
#
# 用法: bash scripts/build-ffmpeg.sh
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -W 2>/dev/null || pwd)"
FF_VERSION="7.1"
command -v docker >/dev/null 2>&1 || { echo "[错误] 需要 Docker"; exit 1; }

echo "=== 构建 Linux x86_64 最小化 ffmpeg ${FF_VERSION} 静态库 ==="
docker run -i --rm -v "${ROOT}:/src" curl_download_build bash -s <<INNER
set -euo pipefail

apt-get update -qq
apt-get install -y -qq xz-utils nasm pkg-config >/dev/null

cd /tmp
wget -q https://ffmpeg.org/releases/ffmpeg-${FF_VERSION}.tar.xz
tar xf ffmpeg-${FF_VERSION}.tar.xz
cd ffmpeg-${FF_VERSION}

./configure \
  --prefix=/tmp/ff-install \
  --disable-shared --enable-static \
  --disable-programs --disable-doc --disable-debug --enable-small \
  --disable-avdevice --disable-avfilter --disable-postproc \
  --disable-swscale --disable-swresample --disable-network \
  --disable-everything \
  --enable-avformat --enable-avcodec --enable-avutil \
  --enable-demuxer=mov --enable-muxer=mp4,mov \
  --enable-protocol=file \
  --disable-zlib --disable-bzlib --disable-lzma --disable-iconv \
  --disable-xlib --disable-sdl2 --disable-alsa >/dev/null

make -j\$(nproc) >/dev/null
make install >/dev/null

mkdir -p /src/third_party/ffmpeg/linux-x86_64
rm -rf /src/third_party/ffmpeg/include /src/third_party/ffmpeg/linux-x86_64/*
cp -r /tmp/ff-install/include /src/third_party/ffmpeg/include
cp /tmp/ff-install/lib/*.a /src/third_party/ffmpeg/linux-x86_64/

echo "=== 产物清单 ==="
ls -lh /src/third_party/ffmpeg/linux-x86_64/
echo "[OK] third_party/ffmpeg/ 收编完成（${FF_VERSION} 最小化静态库）"
INNER
