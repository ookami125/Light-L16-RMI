#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_BUILD_DIR="${ROOT_DIR}/client/build"
SERVER_BUILD_DIR="${ROOT_DIR}/build"
STAGING_DIR="${ROOT_DIR}/release"
ZIP_PATH="${ROOT_DIR}/release.zip"

build_client() {
  cmake "${ROOT_DIR}/client" -B "${CLIENT_BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
  cmake --build "${CLIENT_BUILD_DIR}" -j
}

build_server() {
  "${ROOT_DIR}/docker_build_server.sh" rmi
}

build_client
build_server

rm -rf "${STAGING_DIR}"
mkdir -p "${STAGING_DIR}"

if [ ! -x "${CLIENT_BUILD_DIR}/rmi_client" ]; then
  echo "Client binary not found at ${CLIENT_BUILD_DIR}/rmi_client" >&2
  exit 1
fi
if [ ! -f "${SERVER_BUILD_DIR}/rmi" ]; then
  echo "Server binary not found at ${SERVER_BUILD_DIR}/rmi" >&2
  exit 1
fi

cp "${CLIENT_BUILD_DIR}/rmi_client" "${STAGING_DIR}/"
cp "${SERVER_BUILD_DIR}/rmi" "${STAGING_DIR}/"
if [ -f "${ROOT_DIR}/rmi.config" ]; then
  cp "${ROOT_DIR}/rmi.config" "${STAGING_DIR}/"
else
  echo "Warning: ${ROOT_DIR}/rmi.config not found; skipping config." >&2
fi

if [ -d "${CLIENT_BUILD_DIR}/scripts" ]; then
  cp -R "${CLIENT_BUILD_DIR}/scripts" "${STAGING_DIR}/"
else
  echo "Warning: ${CLIENT_BUILD_DIR}/scripts not found; skipping scripts." >&2
fi

rm -f "${ZIP_PATH}"
(
  cd "${STAGING_DIR}"
  zip -r "${ZIP_PATH}" .
)
rm -rf "${STAGING_DIR}"
