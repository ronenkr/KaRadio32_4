# hagl_hal backend for the T-Display S3's parallel/I80 ST7789 panel.
# Component must be named exactly "hagl_hal" - the vendored "hagl" (core)
# component's own CMakeLists.txt REQUIRES a component by that literal name.
idf_component_register(
    SRCS "src/hagl_hal_i80.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd esp_driver_gpio esp_driver_ledc hagl
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
