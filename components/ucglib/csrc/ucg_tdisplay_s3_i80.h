/*
 * T-Display S3 8-bit Intel 8080 transport for Ucglib.
 *
 * The standard LilyGO T-Display S3 uses an ST7789V panel on an 8-bit
 * parallel bus.  This is intentionally separate from the SPI Ucglib HAL.
 */
#ifndef UCG_TDISPLAY_S3_I80_H_
#define UCG_TDISPLAY_S3_I80_H_

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "ucg.h"

typedef struct {
    gpio_num_t power;
    gpio_num_t backlight;
    gpio_num_t reset;
    gpio_num_t cs;
    gpio_num_t dc;
    gpio_num_t wr;
    gpio_num_t rd;
    gpio_num_t data[8];
} ucg_tdisplay_s3_i80_pins_t;

bool ucg_tdisplay_s3_i80_configure(const ucg_tdisplay_s3_i80_pins_t *pins);
uint16_t *ucg_tdisplay_s3_i80_draw_buffer(void);
bool ucg_tdisplay_s3_i80_submit_buffer(int x, int y, int width, int height);
bool ucg_tdisplay_s3_i80_fill_rect(int x, int y, int width, int height,
                                   const uint8_t color[3]);

int16_t ucg_com_tdisplay_s3_i80(ucg_t *ucg, int16_t msg, uint16_t arg,
                                uint8_t *data);

#endif /* UCG_TDISPLAY_S3_I80_H_ */
