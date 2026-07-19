# Trimmed, board-specific version of eratv_common: only what doom/main/main.cpp
# actually uses on this hardware. BluetoothJoystick/FrameBuffer/AudioPlayer/
# FileSystem are all header-only (see include/eratv/) - no sources of our
# own to compile. The I80 video backend lives in the separate "hagl_hal"
# component (name required by hagl core's own REQUIRES).
idf_component_register(
    INCLUDE_DIRS "include" "include/eratv" "include/eratv/player"
    REQUIRES esp_timer nvs_flash bt driver esp_driver_i2s
             hagl hagl_hal joltwallet__littlefs
)
