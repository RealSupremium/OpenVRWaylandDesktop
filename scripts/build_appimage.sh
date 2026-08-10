#!/usr/bin/env bash
set -euo pipefail

# Ensure working directory is the repository root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT_DIR}"

BUILD_DIR="${ROOT_DIR}/build"
APPDIR="${ROOT_DIR}/AppDir"
OUTPUT_DIR="${ROOT_DIR}/out"

mkdir -p "${ROOT_DIR}/tools"
cd "${ROOT_DIR}/tools"

LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_PLUGIN_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/continuous/linuxdeploy-plugin-appimage-x86_64.AppImage"

if [ ! -f "linuxdeploy-x86_64.AppImage" ]; then
    echo "Downloading linuxdeploy..."
    curl -fsSL -o linuxdeploy-x86_64.AppImage "${LINUXDEPLOY_URL}"
    chmod +x linuxdeploy-x86_64.AppImage
fi

if [ ! -f "linuxdeploy-plugin-appimage-x86_64.AppImage" ]; then
    echo "Downloading linuxdeploy-plugin-appimage..."
    curl -fsSL -o linuxdeploy-plugin-appimage-x86_64.AppImage "${LINUXDEPLOY_PLUGIN_URL}"
    chmod +x linuxdeploy-plugin-appimage-x86_64.AppImage
fi

cd "${ROOT_DIR}"

export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:/usr/local/lib/x86_64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}"

cmake -B "${BUILD_DIR}" -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -G Ninja

cmake --build "${BUILD_DIR}" --config Release --parallel "$(nproc)"

rm -rf "${APPDIR}"
DESTDIR="${APPDIR}" cmake --install "${BUILD_DIR}"

mkdir -p "${OUTPUT_DIR}"

# In Docker / CI without FUSE, APPIMAGE_EXTRACT_AND_RUN=1 allows running AppImages
export APPIMAGE_EXTRACT_AND_RUN=1

LINUXDEPLOY="${ROOT_DIR}/tools/linuxdeploy-x86_64.AppImage"

"${LINUXDEPLOY}" \
    --appdir "${APPDIR}" \
    --desktop-file "${APPDIR}/usr/share/applications/openvr_desktop_overlay.desktop" \
    --icon-file "${APPDIR}/usr/share/icons/hicolor/512x512/apps/openvr_desktop_overlay.png" \
    --executable "${APPDIR}/usr/bin/openvr_desktop_overlay" \
    --output appimage

# Move generated AppImage to out/
mv *.AppImage "${OUTPUT_DIR}/" 2>/dev/null || true

ls -lh "${OUTPUT_DIR}"/*.AppImage
