# Vendored decompression-only subset of miniz (richgel999/miniz, MIT
# licensed - see LICENSE-miniz.txt): just tinfl, the tiny inflate
# implementation. Used on-device to decode ZWAD's per-lump zlib streams;
# see doom/zwad-compressed-wad-porting.md.
#
# Callers (and miniz_tinfl.c itself) must always include "miniz.h", not
# miniz_tinfl.h directly - it pins the platform macros that size the
# tinfl_decompressor struct consistently across translation units.
idf_component_register(
    SRCS "src/miniz_tinfl.c"
    INCLUDE_DIRS "include"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

# Vendored upstream C, not our style - keep warnings from breaking the build.
component_compile_options(
    -Wno-error
    -Wno-unused-function
)
