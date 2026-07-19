#pragma once
/*
 * Real miniz generates this from miniz_export.h.in via CMake to add
 * dllexport/dllimport decorations for shared-library builds. We only ever
 * build tinfl as a static ESP-IDF component, so there is nothing to export.
 */
#define MINIZ_EXPORT
