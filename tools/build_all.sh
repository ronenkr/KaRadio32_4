#!/usr/bin/env bash
# Builds both apps that share the T-Display S3's partition table
# (partitions.ttgo_tdisplay_s3.csv): KaRadio32_4 itself (ota_0, 2MB) and the
# Doom port in doom/ (ota_1, 4MB). Reports both binary sizes against their
# partition budgets so an overflow is obvious here rather than at flash time.
#
# Usage: tools/build_all.sh
# Requires: ESP-IDF v5.4.2 environment already sourced
#   (source /path/to/esp-idf/export.sh), or set IDF_EXPORT_SCRIPT below.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if ! command -v idf.py >/dev/null 2>&1; then
    echo "idf.py not found on PATH - source esp-idf/export.sh first." >&2
    exit 1
fi

echo "== Building KaRadio32_4 (ota_0, 2MB budget) =="
(
    cd "${REPO_ROOT}"
    IDF_TARGET=esp32s3 idf.py -B build-ttgo-t-display-s3 \
        -DSDKCONFIG=build-ttgo-t-display-s3/sdkconfig build
)

echo
echo "== Building doom (ota_1, 4MB budget) =="
(
    cd "${REPO_ROOT}/doom"
    IDF_TARGET=esp32s3 idf.py -B build -DSDKCONFIG=build/sdkconfig build
)

karadio_bin="${REPO_ROOT}/build-ttgo-t-display-s3/KaRadio32_4.bin"
doom_bin="${REPO_ROOT}/doom/build/eratv_doom.bin"

karadio_size=$(stat -c%s "${karadio_bin}")
doom_size=$(stat -c%s "${doom_bin}")

# Must match partitions.ttgo_tdisplay_s3.csv - ota_0=0x200000, ota_1=0x400000.
ota0_budget=$((0x200000))
ota1_budget=$((0x400000))

echo
echo "== Size summary =="
printf "KaRadio32_4.bin: %8d bytes / %8d (ota_0)  %3d%% free\n" \
    "${karadio_size}" "${ota0_budget}" $(( (ota0_budget - karadio_size) * 100 / ota0_budget ))
printf "eratv_doom.bin:  %8d bytes / %8d (ota_1)  %3d%% free\n" \
    "${doom_size}" "${ota1_budget}" $(( (ota1_budget - doom_size) * 100 / ota1_budget ))

if (( karadio_size >= ota0_budget )); then
    echo "ERROR: KaRadio32_4.bin exceeds the ota_0 partition." >&2
    exit 1
fi
if (( doom_size >= ota1_budget )); then
    echo "ERROR: eratv_doom.bin exceeds the ota_1 partition." >&2
    exit 1
fi

echo
echo "Both apps built. Run tools/flash_all.sh PORT to flash them."
