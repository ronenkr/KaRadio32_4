file(GLOB PRBOOM_SOURCES "${CMAKE_CURRENT_LIST_DIR}/*.c")

idf_component_register(
    SRCS ${PRBOOM_SOURCES}
    INCLUDE_DIRS "."
    REQUIRES newlib tinfl
    LDFRAGMENTS "linker.lf"
)

# PrBoom core configuration:
#  - HAVE_CONFIG_H enables config.h (NOTRUECOLOR, NODEHSUPPORT, PRBOOMWAD, ...)
#  - RETRO_GO is intentionally NOT defined so the core falls back to standard
#    vprintf logging and stdio WAD loading (no retro-go dependency).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_CONFIG_H
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -fomit-frame-pointer
)

# The PrBoom sources are vendored C from upstream and trip many modern warnings.
component_compile_options(
    -Wno-error
    -Wno-error=address
    -Wno-misleading-indentation
    -Wno-format-overflow
    -Wno-format
    -Wno-char-subscripts
    -Wno-missing-field-initializers
    -Wno-implicit-fallthrough
    -Wno-maybe-uninitialized
)
