#!/usr/bin/env bash
# Builds a LittleFS filesystem image from a folder of WAD files and flashes
# it to the `littlefs` partition (partitions.ttgo_tdisplay_s3.csv). Kept
# separate from firmware flashing (flash_all.sh) since you'll do this less
# often - once per WAD change, not once per code change.
#
# Uses the same littlefs-python tool the esp_littlefs component's own CMake
# image builder uses (doom/managed_components/joltwallet__littlefs/
# project_include.cmake), just invoked standalone instead of through a full
# idf.py build.
#
# Usage: tools/flash_wads.sh PORT /path/to/wads/
# Requires: python3 with venv support, esptool.py on PATH.
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "Usage: $0 PORT /path/to/wads/" >&2
    exit 1
fi
PORT="$1"
WAD_DIR="$2"

if [ ! -d "${WAD_DIR}" ]; then
    echo "WAD directory not found: ${WAD_DIR}" >&2
    exit 1
fi

# Must match partitions.ttgo_tdisplay_s3.csv's littlefs entry.
LITTLEFS_OFFSET=0x425000
LITTLEFS_SIZE=$((0xBDB000))
NAME_MAX=255
BLOCK_SIZE=4096

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
VENV_DIR="${REPO_ROOT}/tools/.littlefs_venv"
IMAGE_FILE="${REPO_ROOT}/tools/littlefs.bin"

if [ ! -d "${VENV_DIR}" ]; then
    echo "== Setting up littlefs-python venv (one-time) =="
    python3 -m venv "${VENV_DIR}"
    "${VENV_DIR}/bin/pip" install --quiet littlefs-python
fi

echo "== Building LittleFS image from ${WAD_DIR} =="
"${VENV_DIR}/bin/littlefs-python" create "${WAD_DIR}" "${IMAGE_FILE}" \
    -v --fs-size="${LITTLEFS_SIZE}" --name-max="${NAME_MAX}" --block-size="${BLOCK_SIZE}"

echo
echo "== Flashing littlefs image to ${LITTLEFS_OFFSET} =="
esptool.py --chip esp32s3 --port "${PORT}" write_flash "${LITTLEFS_OFFSET}" "${IMAGE_FILE}"

echo
echo "Done. Boot into Doom (long-press GPIO0 or sys.launchapp) to pick up the new WADs."
