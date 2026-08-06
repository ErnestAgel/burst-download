#!/usr/bin/env bash
# ============================================================================
# 一次性构建三平台静态 libcurl 库 → third_party/<platform>/libcurl.a
# 供 CMake Release（静态单文件）使用；Debug 仍走现有动态库
#
#   Linux x86_64 : openssl 3.0.13 静态 + curl 7.88.1 静态
#   Windows x86_64: curl 8.12.1 --with-schannel（Windows 原生 TLS，无需 openssl）
#   Linux aarch64: openssl 3.0.13 交叉编译 + curl 7.88.1 交叉静态
#
# 用法: bash scripts/build-static-libs.sh
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -W 2>/dev/null || pwd)"
command -v docker >/dev/null 2>&1 || { echo "[错误] 需要 Docker"; exit 1; }

echo "=== 构建三平台静态 libcurl 库（后台容器内进行） ==="
docker run -i --rm -v "${ROOT}:/src" curl_download_build bash -s <<'INNER'
set -euo pipefail

apt-get update -qq
apt-get install -y -qq wget build-essential zlib1g-dev libzstd-dev perl-modules \
  g++-mingw-w64-x86-64 gcc-aarch64-linux-gnu g++-aarch64-linux-gnu >/dev/null

cd /tmp
wget -q https://curl.se/download/curl-7.88.1.tar.gz
tar xf curl-7.88.1.tar.gz
wget -q https://curl.se/download/curl-8.12.1.tar.gz
tar xf curl-8.12.1.tar.gz
wget -q https://www.openssl.org/source/openssl-3.0.13.tar.gz
tar xf openssl-3.0.13.tar.gz

CURL_OPTS="--disable-shared --enable-static --disable-ldap --disable-ldaps \
  --without-libpsl --without-libidn2 --without-brotli --without-zstd --without-nghttp2 \
  --without-librtmp --without-libssh2"

# ---------- 1) Linux x86_64 ----------
echo "=== [1/3] Linux x86_64 ==="
cd /tmp/openssl-3.0.13
./Configure linux-x86_64 --prefix=/tmp/x86-ssl --libdir=lib no-shared no-tests no-asm >/dev/null
make -j4 >/dev/null && make install_sw >/dev/null
cd /tmp/curl-7.88.1
./configure ${CURL_OPTS} --with-openssl=/tmp/x86-ssl --with-zlib >/dev/null
make -j4 >/dev/null
cp lib/.libs/libcurl.a /src/third_party/linux-x86_64/libcurl.a
echo "[OK] third_party/linux-x86_64/libcurl.a"

# ---------- 2) Windows x86_64（Schannel） ----------
echo "=== [2/3] Windows x86_64 ==="
cd /tmp/curl-8.12.1
./configure --host=x86_64-w64-mingw32 ${CURL_OPTS} \
  --with-schannel --without-zlib --without-openssl >/dev/null
make -j4 >/dev/null
cp lib/.libs/libcurl.a /src/third_party/windows-x86_64/libcurl.a
echo "[OK] third_party/windows-x86_64/libcurl.a"

# ---------- 3) Linux aarch64 ----------
echo "=== [3/3] Linux aarch64 ==="
cd /tmp/openssl-3.0.13
make distclean >/dev/null 2>&1 || true
./Configure linux-aarch64 CC=aarch64-linux-gnu-gcc \
  --prefix=/tmp/arm-ssl --libdir=lib no-shared no-tests no-asm >/dev/null
make -j4 >/dev/null && make install_sw >/dev/null
cd /tmp/curl-7.88.1
make distclean >/dev/null 2>&1 || true
./configure --host=aarch64-linux-gnu ${CURL_OPTS} \
  --with-openssl=/tmp/arm-ssl --with-zlib >/dev/null
make -j4 >/dev/null
cp lib/.libs/libcurl.a /src/third_party/linux-aarch64/libcurl.a
echo "[OK] third_party/linux-aarch64/libcurl.a"
INNER

echo "=== 完成，静态库清单 ==="
ls -la "${ROOT}/third_party/linux-x86_64/libcurl.a" \
       "${ROOT}/third_party/linux-aarch64/libcurl.a" \
       "${ROOT}/third_party/windows-x86_64/libcurl.a"
