#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-vikiroot-build}"
BUILD_TARGET="${BUILD_TARGET:-linux}"

if [[ "${BUILD_TARGET}" == "android" || "${BUILD_TARGET}" == "android-static" ]]; then
  ANDROID_API="${ANDROID_API:-21}"
  SYSROOT="${SYSROOT:-/opt/android-ndk-r16b/platforms/android-${ANDROID_API}/arch-arm64}"
  HEADER_SYSROOT="${HEADER_SYSROOT:-/opt/android-ndk-r16b/sysroot}"
  TARGET_TRIPLE="${TARGET_TRIPLE:-aarch64-linux-android}"
  if [[ "${BUILD_TARGET}" == "android-static" ]]; then
    BASE_CFLAGS="-Os -fno-pie"
    BASE_LDFLAGS="-pthread -s -static -no-pie -Wl,--build-id=sha1"
  else
    BASE_CFLAGS="-Os -fPIE"
    BASE_LDFLAGS="-pthread -s -pie"
  fi
  CFLAGS="${CFLAGS:-${BASE_CFLAGS} --sysroot=${SYSROOT} -D__ANDROID_API__=${ANDROID_API} -isystem ${HEADER_SYSROOT}/usr/include -isystem ${HEADER_SYSROOT}/usr/include/${TARGET_TRIPLE}}"
  LDFLAGS="${LDFLAGS:-${BASE_LDFLAGS} --sysroot=${SYSROOT}}"
elif [[ "${BUILD_TARGET}" == "linux" ]]; then
  TARGET_TRIPLE="${TARGET_TRIPLE:-aarch64-linux-gnu}"
  BASE_CFLAGS="-Os -fno-pie"
  BASE_LDFLAGS="-pthread -s -static -no-pie -Wl,--build-id=sha1"
  CFLAGS="${CFLAGS:-${BASE_CFLAGS}}"
  LDFLAGS="${LDFLAGS:-${BASE_LDFLAGS}}"
else
  echo "Unknown BUILD_TARGET: ${BUILD_TARGET}" >&2
  exit 1
fi

TOOLCHAIN_PREFIX="${TOOLCHAIN_PREFIX:-${TARGET_TRIPLE}}"
CC="${CC:-${TOOLCHAIN_PREFIX}-gcc}"
AS="${AS:-${TOOLCHAIN_PREFIX}-as}"
OC="${OC:-${TOOLCHAIN_PREFIX}-objcopy}"

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <make-target> [make-args...]"
  echo "Example: $0 rmi"
  echo "         $0 clean"
  exit 1
fi

docker build -t "${IMAGE_NAME}" -f Dockerfile .

docker run --rm \
  -u "$(id -u):$(id -g)" \
  -v "$(pwd)":/src \
  -w /src \
  "${IMAGE_NAME}" \
  make \
    CC="${CC}" \
    AS="${AS}" \
    OC="${OC}" \
    CFLAGS="${CFLAGS}" \
    LDFLAGS="${LDFLAGS}" \
    "$@"
