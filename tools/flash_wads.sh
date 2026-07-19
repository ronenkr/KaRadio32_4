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
LITTLEFS_PARTITION_SIZE=$((0xBDB000))
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

# Build the image just large enough to hold WAD_DIR's actual content, NOT the
# full partition size. littlefs-python's own free-block accounting is broken
# for any image built with baked-in headroom over its actual content - even a
# handful of unused blocks get reported (and, critically, treated) as fully
# allocated once mounted, so any later write on the device - our own
# prboom.cfg/doom_settings.bin/save files - fails with ENOSPC despite the
# partition being mostly empty. Confirmed independently via littlefs-python's
# own fs_stat() on a bare-metal test image, so this isn't just a misleading
# stat: real writes fail too. The device grows this tight image up to the
# full partition size itself on mount instead (FileSystem.hpp's
# grow_on_mount=true), through the real C littlefs library, which does not
# have this defect.
CONTENT_BYTES=$(du -sb "${WAD_DIR}" | cut -f1)
MARGIN_BYTES=$((32 * 1024))  # littlefs superblock/directory-entry overhead
LITTLEFS_SIZE=$(( ( (CONTENT_BYTES + MARGIN_BYTES + BLOCK_SIZE - 1) / BLOCK_SIZE ) * BLOCK_SIZE ))

if (( LITTLEFS_SIZE > LITTLEFS_PARTITION_SIZE )); then
    echo "WAD_DIR content (${CONTENT_BYTES} bytes) does not fit in the littlefs partition (${LITTLEFS_PARTITION_SIZE} bytes)." >&2
    exit 1
fi

echo "== Building LittleFS image from ${WAD_DIR} (${LITTLEFS_SIZE} bytes, grows to ${LITTLEFS_PARTITION_SIZE} on device) =="
"${VENV_DIR}/bin/littlefs-python" create "${WAD_DIR}" "${IMAGE_FILE}" \
    -v --fs-size="${LITTLEFS_SIZE}" --name-max="${NAME_MAX}" --block-size="${BLOCK_SIZE}"

echo
echo "== Erasing littlefs partition (${LITTLEFS_PARTITION_SIZE} bytes at ${LITTLEFS_OFFSET}) =="
# Clean 0xFF space beyond the (smaller) image is required for grow_on_mount
# to work correctly - otherwise it could grow into stale data left over from
# a previous, differently-sized image.
esptool.py --chip esp32s3 --port "${PORT}" erase_region "${LITTLEFS_OFFSET}" "${LITTLEFS_PARTITION_SIZE}"

echo
echo "== Flashing littlefs image to ${LITTLEFS_OFFSET} =="
esptool.py --chip esp32s3 --port "${PORT}" write_flash "${LITTLEFS_OFFSET}" "${IMAGE_FILE}"

echo
echo "Done. Boot into Doom (long-press GPIO0 or sys.launchapp) to pick up the new WADs."
