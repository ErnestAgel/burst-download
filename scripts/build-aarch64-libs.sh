#!/usr/bin/env bash
# ============================================================================
# 一次性构建 Linux aarch64 的 Python 嵌入库 + FFmpeg 静态库（交叉编译）
#
# 在 x86_64 构建镜像内用 aarch64-linux-gnu 交叉工具链编译（比 qemu 原生快数倍）：
#   - host python 3.11.9（x86 原生，供 CPython 交叉构建期代码生成 --with-build-python）
#   - openssl 3.0.13（aarch64，供 _ssl/_hashlib）
#   - CPython 3.11.9（--host=aarch64-linux-gnu，裁剪 44 核心 C 模块静态编入）
#   - ffmpeg 7.1（aarch64，最小化 remux 组件）
#
# 产物收编：
#   third_party/python/linux-aarch64/{libpython3.11.a, include/}   ← include 含 aarch64 configure 生成的 pyconfig.h
#   third_party/ffmpeg/linux-aarch64/{libav*.a, include/}          ← include 含 aarch64 生成的 avconfig.h
#
# 用法: bash scripts/build-aarch64-libs.sh
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -W 2>/dev/null || pwd)"
command -v docker >/dev/null 2>&1 || { echo "[错误] 需要 Docker"; exit 1; }

echo "=== 构建 aarch64 库（交叉编译，预计 30~40 分钟，后台进行） ==="
docker run -i --rm -v "${ROOT}:/src" curl_download_build bash -s <<'INNER'
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
CROSS=aarch64-linux-gnu

echo "=== [0] 工具链与依赖 ==="
${CROSS}-gcc --version | head -1
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq zlib1g-dev libffi-dev perl-modules xz-utils >/dev/null 2>&1 || { echo "[错误] apt 安装失败"; exit 1; }
python3 --version

echo "=== [1/5] host python 3.11.9（x86 原生，交叉构建期使用） ==="
HOSTPY=/src/.build-cache/hostpy
if [ -x ${HOSTPY}/bin/python3.11 ]; then
  echo "[host python 缓存命中]"
else
  cd /tmp
  [ -f Python-3.11.9.tgz ] || wget -q https://www.python.org/ftp/python/3.11.9/Python-3.11.9.tgz
  rm -rf Python-host && mkdir Python-host && tar xf Python-3.11.9.tgz -C Python-host --strip-components=1
  cd Python-host
  ./configure --prefix=${HOSTPY} --disable-shared >/dev/null
  make -j4 >/dev/null && make install >/dev/null
  echo "[host python 编译完成]"
fi
${HOSTPY}/bin/python3.11 --version
echo "[host python OK]"

echo "=== [2/5] openssl 3.0.13（aarch64 交叉） ==="
cd /tmp
[ -f openssl-3.0.13.tar.gz ] || wget -q https://www.openssl.org/source/openssl-3.0.13.tar.gz
rm -rf openssl-3.0.13 && tar xf openssl-3.0.13.tar.gz
cd openssl-3.0.13
./Configure linux-aarch64 --prefix=/tmp/ssl-arm --libdir=lib no-shared no-tests no-asm \
  CC=${CROSS}-gcc AR=${CROSS}-ar RANLIB=${CROSS}-ranlib >/dev/null
make -j4 >/dev/null && make install_sw >/dev/null
echo "[openssl OK]"

echo "=== [3/5] CPython 3.11.9（aarch64 交叉，44 核心模块） ==="
cd /tmp
[ -f Python-3.11.9.tgz ] || wget -q https://www.python.org/ftp/python/3.11.9/Python-3.11.9.tgz
rm -rf Python-arm && mkdir Python-arm && tar xf Python-3.11.9.tgz -C Python-arm --strip-components=1
cd Python-arm
# 交叉编译：getaddrinfo 等检测需运行测试程序（交叉程序无法在本机运行），
# 用 ac_cv 强制声明以通过 configure（CPython 用自定义 cache 变量 ac_cv_getaddrinfo）
export ac_cv_getaddrinfo=yes
export ac_cv_func_getaddrinfo=yes
export ac_cv_func_getnameinfo=yes
export ac_cv_file__dev_ptmx=yes
export ac_cv_file__dev_ptc=no
./configure --host=${CROSS} --build=x86_64-linux-gnu \
  --disable-shared --without-ensurepip --disable-ipv6 \
  --with-openssl=/tmp/ssl-arm \
  --with-build-python=${HOSTPY}/bin/python3.11 >/dev/null
python3 - <<'PY'
import re
path = 'Modules/Setup'
lines = open(path).read().splitlines()
mods = {'_struct','_json','_ssl','_hashlib','_socket','binascii','zlib',
        'math','_datetime','_codecs','_collections','_heapq','_io','_pickle','_random',
        '_sre','array','select','unicodedata','_statistics','time','_posixsubprocess',
        '_functools','_operator','_thread','_signal','_tracemalloc','_weakref','_contextvars',
        '_queue','_multibytecodec','_codecs_cn','_codecs_hk','_codecs_iso2022','_codecs_jp',
        '_codecs_kr','_codecs_tw'}
# 注：_bz2/_lzma 依赖交叉系统库（libbz2/liblzma 无 aarch64 版），yt-dlp 不需要，裁剪掉
out = []
for ln in lines:
    m = re.match(r'^#\s*([A-Za-z_0-9]+)\s', ln)
    if m and m.group(1) in mods:
        ln = re.sub(r'\s+-shared\b', '', ln)  # 静态编入
        if m.group(1) == 'zlib':
            ln = re.sub(r'-lz\s*$', '/src/third_party/linux-aarch64/static/libz.a', ln)
        ln = ln.lstrip('#').lstrip()
    out.append(ln)
open(path, 'w').write('\n'.join(out) + '\n')
print(f"模块启用: {sum(1 for ln in out if re.match(r'^[A-Za-z_0-9]+\s+\S+\.c', ln))}")
PY
make -j4 >/dev/null
${CROSS}-strip --strip-debug libpython3.11.a   # 仅去调试符号，保留全局符号（--strip-all 会破坏静态链接）
file libpython3.11.a
echo "[libpython OK] $(ls -la libpython3.11.a | awk '{print $5}')B"

echo "=== [4/5] ffmpeg 7.1（aarch64 交叉，最小化） ==="
cd /tmp
[ -f ffmpeg-7.1.tar.xz ] || wget -q https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz
rm -rf ffmpeg-7.1 && tar xf ffmpeg-7.1.tar.xz
cd ffmpeg-7.1
./configure \
  --prefix=/tmp/ff-install \
  --enable-cross-compile --target-os=linux --arch=aarch64 \
  --cc=${CROSS}-gcc --host-cc=gcc \
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
make -j4 >/dev/null
make install >/dev/null
echo "[ffmpeg OK]"

echo "=== [5/5] 收编产物 ==="
mkdir -p /src/third_party/python/linux-aarch64
rm -rf /src/third_party/python/linux-aarch64/*
cp /tmp/Python-arm/libpython3.11.a /src/third_party/python/linux-aarch64/
cp -r /tmp/Python-arm/Include /src/third_party/python/linux-aarch64/include
cp /tmp/Python-arm/pyconfig.h /src/third_party/python/linux-aarch64/include/

mkdir -p /src/third_party/ffmpeg/linux-aarch64
rm -rf /src/third_party/ffmpeg/linux-aarch64/*
cp /tmp/ff-install/lib/*.a /src/third_party/ffmpeg/linux-aarch64/
cp -r /tmp/ff-install/include /src/third_party/ffmpeg/linux-aarch64/include

echo "=== 产物清单 ==="
ls -lh /src/third_party/python/linux-aarch64/ /src/third_party/ffmpeg/linux-aarch64/
echo "[OK] aarch64 收编完成"
INNER
