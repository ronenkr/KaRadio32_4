#!/usr/bin/env bash
# Flashes both apps plus the hardware-config NVS blob onto a T-Display S3,
# in the order that matters:
#   1. KaRadio32_4's full image (bootloader + partition table + app + otadata)
#      via `idf.py flash` - this is what establishes the shared bootloader/
#      partition table on a fresh chip, and boots into ota_0.
#   2. doom's app binary ONLY, raw-written to ota_1's offset - deliberately
#      not reflashing doom's own bootloader/partition-table output, since
#      both apps must share the one partition table just flashed in step 1.
#   3. The `hardware` NVS config blob (board GPIO/option layout) at its
#      T-Display S3 offset.
#
# Does NOT flash WADs into the littlefs partition - see flash_wads.sh for that.
#
# Usage: tools/flash_all.sh PORT
# Requires: ESP-IDF v5.4.2 environment already sourced, and both apps already
#   built (tools/build_all.sh).
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 PORT" >&2
    exit 1
fi
PORT="$1"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Must match partitions.ttgo_tdisplay_s3.csv.
OTA1_OFFSET=0x210000
HARDWARE_OFFSET=0x622000

doom_bin="${REPO_ROOT}/doom/build/eratv_doom.bin"
if [ ! -f "${doom_bin}" ]; then
    echo "doom/build/eratv_doom.bin not found - run tools/build_all.sh first." >&2
    exit 1
fi

echo "== Flashing KaRadio32_4 (bootloader + partition table + app + otadata) =="
(
    cd "${REPO_ROOT}"
    IDF_TARGET=esp32s3 idf.py -B build-ttgo-t-display-s3 -p "${PORT}" flash
)

echo
echo "== Flashing doom app to ota_1 (${OTA1_OFFSET}) =="
esptool.py --chip esp32s3 --port "${PORT}" write_flash "${OTA1_OFFSET}" "${doom_bin}"

echo
echo "== Flashing hardware config to ${HARDWARE_OFFSET} =="
(
    cd "${REPO_ROOT}/boards"
    NVS_FLASH_OFFSET="${HARDWARE_OFFSET}" ESPTOOL_CHIP=esp32s3 \
        bash ./nvs_partition_generator.sh ttgo_tdisplay_s3.csv
    esptool.py --chip esp32s3 --port "${PORT}" write_flash "${HARDWARE_OFFSET}" \
        build/ttgo_tdisplay_s3.bin
)

echo
echo "Done. Board boots into KaRadio (ota_0); long-press GPIO0 or run"
echo "sys.launchapp to switch to Doom (ota_1). Flash WADs separately with"
echo "tools/flash_wads.sh before trying to actually play."
