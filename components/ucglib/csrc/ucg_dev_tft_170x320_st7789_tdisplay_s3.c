/*
 * ST7789V 170x320 device callback for the LilyGO T-Display S3.
 *
 * Ucglib retains the panel's native 170x320 coordinate space.  KaRadio's
 * normal Rotate90/Rotate270 setup turns it into the expected 320x170 UI.
 */
#include "ucg.h"
#include "ucg_tdisplay_s3_i80.h"

#define TDISPLAY_S3_WIDTH 170
#define TDISPLAY_S3_HEIGHT 320

static uint16_t rgb_to_565(const uint8_t color[3])
{
    return ((uint16_t)(color[0] & 0xf8) << 8) |
           ((uint16_t)(color[1] & 0xfc) << 3) |
           ((uint16_t)color[2] >> 3);
}

static bool line_box(const ucg_t *ucg, int *x, int *y, int *width,
                     int *height, bool *reverse)
{
    *x = ucg->arg.pixel.pos.x;
    *y = ucg->arg.pixel.pos.y;
    *width = 1;
    *height = 1;
    *reverse = false;

    switch (ucg->arg.dir) {
    case 0:
        *width = ucg->arg.len;
        break;
    case 1:
        *height = ucg->arg.len;
        break;
    case 2:
        *width = ucg->arg.len;
        *x -= *width - 1;
        *reverse = true;
        break;
    case 3:
        *height = ucg->arg.len;
        *y -= *height - 1;
        *reverse = true;
        break;
    default:
        return false;
    }

    return true;
}

static void draw_pixel(ucg_t *ucg)
{
    (void)ucg_tdisplay_s3_i80_fill_rect(
        ucg->arg.pixel.pos.x, ucg->arg.pixel.pos.y, 1, 1,
        ucg->arg.pixel.rgb.color);
}

static void draw_constant_line(ucg_t *ucg)
{
    int x;
    int y;
    int width;
    int height;
    bool reverse;

    if (ucg_clip_l90fx(ucg) == 0 ||
        !line_box(ucg, &x, &y, &width, &height, &reverse)) {
        return;
    }

    (void)reverse;
    (void)ucg_tdisplay_s3_i80_fill_rect(x, y, width, height,
                                         ucg->arg.pixel.rgb.color);
}

static ucg_int_t draw_gradient_line(ucg_t *ucg)
{
    int x;
    int y;
    int width;
    int height;
    bool reverse;
    uint16_t *buffer;

    if (ucg->arg.len <= 1) {
        ucg->arg.pixel.rgb = ucg->arg.rgb[0];
        draw_constant_line(ucg);
        return 1;
    }

    for (uint8_t component = 0; component < 3; ++component) {
        ucg_ccs_init(ucg->arg.ccs_line + component,
                     ucg->arg.rgb[0].color[component],
                     ucg->arg.rgb[1].color[component], ucg->arg.len);
    }

    if (ucg_clip_l90se(ucg) == 0 ||
        !line_box(ucg, &x, &y, &width, &height, &reverse)) {
        return 0;
    }

    buffer = ucg_tdisplay_s3_i80_draw_buffer();
    if (buffer == NULL) {
        return 0;
    }

    for (ucg_int_t index = 0; index < ucg->arg.len; ++index) {
        uint8_t color[3] = {
            ucg->arg.ccs_line[0].current,
            ucg->arg.ccs_line[1].current,
            ucg->arg.ccs_line[2].current,
        };
        const ucg_int_t buffer_index = reverse ? ucg->arg.len - index - 1 : index;
        buffer[buffer_index] = rgb_to_565(color);

        for (uint8_t component = 0; component < 3; ++component) {
            ucg_ccs_step(ucg->arg.ccs_line + component);
        }
    }

    return ucg_tdisplay_s3_i80_submit_buffer(x, y, width, height) ? 1 : 0;
}

ucg_int_t ucg_dev_st7789_18x170x320_tdisplay_s3(ucg_t *ucg, ucg_int_t msg,
                                                 void *data)
{
    switch (msg) {
    case UCG_MSG_DEV_POWER_UP:
        return ucg_com_PowerUp(ucg, 100, 66);

    case UCG_MSG_DEV_POWER_DOWN:
        ucg_com_PowerDown(ucg);
        return 1;

    case UCG_MSG_GET_DIMENSION:
        ((ucg_wh_t *)data)->w = TDISPLAY_S3_WIDTH;
        ((ucg_wh_t *)data)->h = TDISPLAY_S3_HEIGHT;
        return 1;

    case UCG_MSG_DRAW_PIXEL:
        if (ucg_clip_is_pixel_visible(ucg) != 0) {
            draw_pixel(ucg);
        }
        return 1;

    case UCG_MSG_DRAW_L90FX:
        draw_constant_line(ucg);
        return 1;

    default:
        return ucg_dev_default_cb(ucg, msg, data);
    }
}

ucg_int_t ucg_ext_st7789_tdisplay_s3(ucg_t *ucg, ucg_int_t msg, void *data)
{
    (void)data;

    if (msg == UCG_MSG_DRAW_L90SE) {
        return draw_gradient_line(ucg);
    }

    return ucg_ext_none(ucg, msg, data);
}
