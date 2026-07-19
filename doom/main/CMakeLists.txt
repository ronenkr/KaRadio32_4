idf_component_register(SRCS
                    "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_timer esp_psram nvs_flash bt app_update
                             eratv_common prboom
                    )

component_compile_options(
    -Wno-error=format=
    -Wno-format
    -Wno-error
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ERATV_SPLASH_HEADER=\"${CMAKE_CURRENT_LIST_DIR}/resources/splashimg.h\"
)
