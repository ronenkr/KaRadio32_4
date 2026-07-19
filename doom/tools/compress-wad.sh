#!/usr/bin/env bash
# Builds wadcompress if needed, then compresses a WAD into a ZWAD.
#
# Usage: ./compress-wad.sh <input.wad> <output.zwad>
#
# See ../zwad-compressed-wad-porting.md for the format and
# wadcompress.c for the compressor itself.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WADCOMPRESS_BIN="${SCRIPT_DIR}/build/wadcompress"

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <input.wad> <output.zwad>" >&2
  exit 1
fi

INPUT="$1"
OUTPUT="$2"

if [[ -e "${OUTPUT}" ]]; then
  echo "compress-wad.sh: ${OUTPUT} already exists, refusing to overwrite" >&2
  exit 1
fi

if [[ ! -x "${WADCOMPRESS_BIN}" || "${SCRIPT_DIR}/wadcompress.c" -nt "${WADCOMPRESS_BIN}" ]]; then
  mkdir -p "${SCRIPT_DIR}/build"
  echo "compress-wad.sh: building wadcompress"
  cc -O2 -o "${WADCOMPRESS_BIN}" "${SCRIPT_DIR}/wadcompress.c" -lz
fi

exec "${WADCOMPRESS_BIN}" "${INPUT}" "${OUTPUT}"
