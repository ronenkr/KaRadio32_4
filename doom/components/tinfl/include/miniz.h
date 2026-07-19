#pragma once
/*
 * Stand-in for upstream miniz's umbrella miniz.h. We only vendor the
 * decompression-only "tinfl" subset (miniz_tinfl.c/.h, miniz_common.h) -
 * not the compressor (miniz_tdef), the zip archive reader/writer
 * (miniz_zip), or the zlib-compatible wrapper API - since the ESP32 side of
 * ZWAD only ever needs to inflate, never deflate. See
 * doom/zwad-compressed-wad-porting.md for how this fits together.
 *
 * The real miniz.h auto-detects these three platform properties near its
 * top before including miniz_common.h/miniz_tinfl.h. Since we skip that
 * detection block, they're pinned here for Xtensa/ESP32 instead. Both
 * miniz_tinfl.c (which #include's "miniz.h", resolving to this file) and
 * any other on-device caller must go through this same header so the
 * tinfl_decompressor struct layout (sized by MINIZ_HAS_64BIT_REGISTERS) is
 * identical in every translation unit.
 */
#define MINIZ_LITTLE_ENDIAN 1
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#define MINIZ_HAS_64BIT_REGISTERS 1

#include "miniz_common.h"
#include "miniz_tinfl.h"
