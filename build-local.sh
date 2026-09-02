#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$SCRIPT_DIR"
CACHE_DIR="${HOME}/.cache/zmk-build"
ARTIFACTS_DIR="${WORKSPACE_DIR}/build/artifacts"
USER_UID=$(id -u)
USER_GID=$(id -g)

mkdir -p "${CACHE_DIR}"
mkdir -p "${ARTIFACTS_DIR}"

echo "========================================="
echo "Building ZMK firmware for Kugel-1 (BLE Micro Pro Dedicated)"
echo "Workspace: ${WORKSPACE_DIR}"
echo "Cache:     ${CACHE_DIR}"
echo "Output:    ${ARTIFACTS_DIR}"
echo "========================================="

# Run build inside ZMK Docker container
docker run --rm \
    -v "${WORKSPACE_DIR}:/workspace/zmk-config" \
    -v "${CACHE_DIR}:/workspace/zmk-cache" \
    -w /workspace/zmk-cache \
    -e ZEPHYR_BASE=/workspace/zmk-cache/zephyr \
    -e CMAKE_PREFIX_PATH=/workspace/zmk-cache/zephyr/share/zephyr-package/cmake \
    zmkfirmware/zmk-build-arm:stable \
    bash -c '
set -e

git config --global --add safe.directory "*"

echo "==> Building firmware for Kugel-1 on BLE Micro Pro..."
west build -p -s /workspace/zmk-cache/zmk/app -d /workspace/zmk-config/build/kugel_bmp -b ble_micro_pro/nrf52840 -- \
    -DSHIELD=kugel \
    -DBOARD_ROOT=/workspace/zmk-config \
    -DZMK_CONFIG=/workspace/zmk-config/config \
    -DZMK_EXTRA_MODULES="/workspace/zmk-config;/workspace/zmk-cache/modules/prospector-zmk-module" \
    -DZEPHYR_BASE=/workspace/zmk-cache/zephyr

mkdir -p /workspace/zmk-config/build/artifacts

if [ -f "/workspace/zmk-config/build/kugel_bmp/zephyr/zmk.uf2" ]; then
    cp /workspace/zmk-config/build/kugel_bmp/zephyr/zmk.uf2 /workspace/zmk-config/build/artifacts/kugel_ble_micro_pro.uf2
    echo "==> SUCCESS! Firmware created: build/artifacts/kugel_ble_micro_pro.uf2"
elif [ -f "/workspace/zmk-config/build/kugel_bmp/zephyr/zephyr.uf2" ]; then
    cp /workspace/zmk-config/build/kugel_bmp/zephyr/zephyr.uf2 /workspace/zmk-config/build/artifacts/kugel_ble_micro_pro.uf2
    echo "==> SUCCESS! Firmware created: build/artifacts/kugel_ble_micro_pro.uf2"
fi

if [ -f "/workspace/zmk-config/build/kugel_bmp/zephyr/zephyr.hex" ]; then
    cp /workspace/zmk-config/build/kugel_bmp/zephyr/zephyr.hex /workspace/zmk-config/build/artifacts/kugel_ble_micro_pro.hex
fi

# Fix file permissions
chown -R '"${USER_UID}:${USER_GID}"' /workspace/zmk-config/build /workspace/zmk-cache 2>/dev/null || true
'

echo "========================================="
echo "Build completed successfully!"
ls -lh "${ARTIFACTS_DIR}"
echo "========================================="
