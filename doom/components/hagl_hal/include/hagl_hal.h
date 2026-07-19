/*
 * Minimal hagl_hal contract for the T-Display S3's parallel/I80 ST7789 panel.
 *
 * This replaces the vendored (SPI-only) hagl_hal component for this board.
 * hagl core (hagl.c/hagl_bitmap.c) only needs two things from a "hagl_hal":
 * the hagl_color_t typedef, and hagl_hal_init(backend) to populate a
 * hagl_backend_t. Everything else (put_pixel/blit/etc.) is generic hagl
 * core code running against a plain RGB565 buffer - see hagl_hal_i80.c.
 */
#ifndef _HAGL_HAL_H
#define _HAGL_HAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <hagl/backend.h>

typedef uint16_t hagl_color_t;

#define DISPLAY_WIDTH  320
#define DISPLAY_HEIGHT 170
#define DISPLAY_DEPTH  16

void hagl_hal_init(hagl_backend_t *backend);

#ifdef __cplusplus
}
#endif
#endif /* _HAGL_HAL_H */
