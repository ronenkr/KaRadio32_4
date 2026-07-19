/*
 * hagl_hal backend for the T-Display S3's parallel/I80 8-bit-bus ST7789
 * panel, used in place of the vendored hagl_hal (which only supports 4-wire
 * SPI panels - the wrong bus for this board).
 *
 * Pin config and esp_lcd setup sequence mirror
 * KaRadio32_4/components/ucglib/csrc/ucg_tdisplay_s3_i80.c (the driver
 * KaRadio itself uses for this exact panel), with two deliberate
 * differences for a game's full-frame-per-tick rendering instead of small
 * dirty-region UI redraws:
 *   - the DMA draw buffer is the WHOLE frame (320x170x2 bytes), not one
 *     line, so a single esp_lcd_panel_draw_bitmap() call per frame suffices
 *   - orientation is set once via esp_lcd_panel_swap_xy()/mirror() (free,
 *     hardware address-window rotation) instead of a per-pixel software
 *     rotate
 *
 * The frame buffer MUST live in internal RAM, not PSRAM, despite
 * esp_lcd_i80_alloc_draw_buffer() nominally supporting a MALLOC_CAP_SPIRAM
 * buffer here (esp32-s3's GDMA can address PSRAM directly in general). Two
 * speed experiments were tried and both failed once Doom's own render loop
 * started calling flush() at full game-loop speed (both worked fine during
 * the slow splash-screen phase, so this isn't a one-off sizing/alignment
 * issue):
 *   - a ping-pong PSRAM double-buffer (kick the DMA transfer async, start
 *     rendering the next frame into the other buffer instead of blocking,
 *     to hide the ~5ms/frame transfer behind CPU work)
 *   - even a single PSRAM buffer with the original synchronous
 *     render-kick-wait pattern
 * Both produced repeated "gdma_link_mount_buffers: no more space" errors
 * from the esp_lcd i80 driver (which doesn't check that call's return value
 * before starting the transaction regardless, so it cascades into visible
 * corruption and CPU1 watchdog stalls). Disabling audio (-nosound) ruled out
 * I2S DMA contention. Switching this buffer alone back to
 * MALLOC_CAP_INTERNAL - identical code otherwise - made the errors disappear
 * completely across the same test run, confirming a PSRAM-sourced buffer is
 * the trigger (root cause inside the GDMA driver not fully identified - would
 * need live JTAG/GDB stepping into its ISR, not available here). So: internal
 * RAM it is, same as KaRadio's own driver for this panel, at the cost of the
 * ~106KB it costs BluetoothJoystick's BLE controller's share of internal RAM.
 *
 * Pins are hardcoded rather than read from the `hardware` NVS partition
 * like KaRadio does: this app only ever targets this one physical board,
 * so there's no need for the CSV-driven multi-board config machinery.
 */
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "hagl_hal.h"
#include "hagl/bitmap.h"

#define TAG "hagl_hal_i80"

/* boards/ttgo_tdisplay_s3.csv P_LCD_* values for this board. */
#define PIN_LCD_CS      GPIO_NUM_6
#define PIN_LCD_DC      GPIO_NUM_7
#define PIN_LCD_RST     GPIO_NUM_5
#define PIN_LCD_PWR     GPIO_NUM_15
#define PIN_LCD_WR      GPIO_NUM_8
#define PIN_LCD_RD      GPIO_NUM_9
#define PIN_LCD_BACKLIGHT GPIO_NUM_38
static const gpio_num_t kDataPins[8] = {
    GPIO_NUM_39, GPIO_NUM_40, GPIO_NUM_41, GPIO_NUM_42,
    GPIO_NUM_45, GPIO_NUM_46, GPIO_NUM_47, GPIO_NUM_48,
};

/* Native panel is 170x320 portrait; presented here as 320x170 landscape via
 * hardware swap_xy, matching KaRadio's presentation. */
#define PANEL_NATIVE_WIDTH  170
#define PANEL_NATIVE_HEIGHT 320
#define PANEL_X_GAP         35

static esp_lcd_i80_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_color_done;
static hagl_bitmap_t s_bitmap;
static uint16_t *s_frame_buffer;

static bool color_transfer_done(esp_lcd_panel_io_handle_t io,
                                 esp_lcd_panel_io_event_data_t *event_data,
                                 void *user_ctx)
{
    (void)io; (void)event_data;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &woken);
    return woken == pdTRUE;
}

static size_t backend_flush(void *self)
{
    hagl_backend_t *backend = (hagl_backend_t *)self;
    if (esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   backend->buffer) != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap failed");
        return 0;
    }
    xSemaphoreTake(s_color_done, portMAX_DELAY);
    return (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT * (DISPLAY_DEPTH / 8);
}

static void backend_close(void *self)
{
    (void)self;
}

/*
 * hagl_bitmap_init()'s put_pixel/get_pixel/color/blit/scale_blit/hline/vline
 * functions expect `self` to be `&s_bitmap` (a hagl_bitmap_t*) - they read
 * its pitch/size/buffer fields, which live at different offsets than
 * hagl_backend_t's flush/close/buffer/buffer2 tail. hagl core always calls
 * backend->hline(backend, ...) etc with the backend pointer as `self`, so
 * wiring those function pointers onto `backend` directly (instead of through
 * these trampolines) reads backend's flush/close bytes as if they were
 * s_bitmap's pitch/buffer - garbage address, LoadStoreError on first draw.
 */
static void trampoline_put_pixel(void *self, int16_t x0, int16_t y0, hagl_color_t color)
{
    (void)self;
    s_bitmap.put_pixel(&s_bitmap, x0, y0, color);
}

static hagl_color_t trampoline_get_pixel(void *self, int16_t x0, int16_t y0)
{
    (void)self;
    return s_bitmap.get_pixel(&s_bitmap, x0, y0);
}

static hagl_color_t trampoline_color(void *self, uint8_t r, uint8_t g, uint8_t b)
{
    (void)self;
    return s_bitmap.color(&s_bitmap, r, g, b);
}

static void trampoline_blit(void *self, int16_t x0, int16_t y0, hagl_bitmap_t *src)
{
    (void)self;
    s_bitmap.blit(&s_bitmap, x0, y0, src);
}

static void trampoline_scale_blit(void *self, uint16_t x0, uint16_t y0, uint16_t w, uint16_t h, hagl_bitmap_t *src)
{
    (void)self;
    s_bitmap.scale_blit(&s_bitmap, x0, y0, w, h, src);
}

static void trampoline_hline(void *self, int16_t x0, int16_t y0, uint16_t width, hagl_color_t color)
{
    (void)self;
    s_bitmap.hline(&s_bitmap, x0, y0, width, color);
}

static void trampoline_vline(void *self, int16_t x0, int16_t y0, uint16_t height, hagl_color_t color)
{
    (void)self;
    s_bitmap.vline(&s_bitmap, x0, y0, height, color);
}

static void init_backlight(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num = PIN_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&channel);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (1 << 13) - 1); // full brightness
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void hagl_hal_init(hagl_backend_t *backend)
{
    memset(backend, 0, sizeof(hagl_backend_t));

    gpio_config_t control_config = {
        .pin_bit_mask = (1ULL << PIN_LCD_PWR) | (1ULL << PIN_LCD_RD),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&control_config));
    gpio_set_level(PIN_LCD_PWR, 1);
    gpio_set_level(PIN_LCD_RD, 1); // write-only bus
    vTaskDelay(pdMS_TO_TICKS(20));

    const size_t frame_bytes = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);

    esp_lcd_i80_bus_config_t bus_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = PIN_LCD_DC,
        .wr_gpio_num = PIN_LCD_WR,
        .data_gpio_nums = {
            kDataPins[0], kDataPins[1], kDataPins[2], kDataPins[3],
            kDataPins[4], kDataPins[5], kDataPins[6], kDataPins[7],
        },
        .bus_width = 8,
        .max_transfer_bytes = frame_bytes,
        .dma_burst_size = 64,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &s_bus));

    s_color_done = xSemaphoreCreateBinary();

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 2, // allow one frame in flight while the next is prepared
        .on_color_trans_done = color_transfer_done,
        .user_ctx = s_color_done,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            // KaRadio's own driver (components/ucglib/csrc/ucg_tdisplay_s3_i80.c)
            // sets this to 1, but that's specific to ucglib's own color-packing,
            // which does NOT pre-swap bytes and relies on this hardware swap
            // alone. We draw through hagl instead (both the direct doom_palette[]
            // writes in I_SetPalette() and hagl's own rgb565() used for the menu
            // overlay), and hagl's rgb565() already manually byte-swaps its
            // output for big-endian transmission. Leaving this on double-swaps
            // (software + hardware cancel out back to native little-endian),
            // which the panel then misinterprets as big-endian - garbled colors,
            // not just a clean R/B swap. Off, since hagl already did the swap.
            .swap_color_bytes = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(s_bus, &io_config, &s_io));

    // Internal RAM, not PSRAM - see the file header comment for why.
    s_frame_buffer = (uint16_t *)esp_lcd_i80_alloc_draw_buffer(
        s_io, frame_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (s_frame_buffer == NULL) {
        ESP_LOGE(TAG, "failed to allocate %u-byte frame buffer", (unsigned)frame_bytes);
        abort();
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    // Hardware landscape rotation (free) instead of a per-frame CPU rotate.
    // With swap_xy (MADCTL MV) fixed on, the two valid landscape
    // orientations - 180 degrees apart - come from swapping BOTH mirror
    // flags together (MV=1,MX=1,MY=0 vs MV=1,MX=0,MY=1); flipping just one
    // of them alone would mirror-flip the image instead of rotating it.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    // esp_lcd_panel_st7789.c's draw_bitmap() always adds x_gap to whatever
    // gets sent as CASET and y_gap to whatever gets sent as RASET - it does
    // NOT swap which one applies when swap_xy (MADCTL MV) is set; the panel
    // hardware alone reinterprets what CASET/RASET mean once MV=1. This
    // panel's physical 35px GRAM-vs-glass offset lives on the native
    // 170-wide column axis, which is what RASET addresses once MV=1 (KaRadio
    // never sets swap_xy at all, staying in native 170x320 portrait, which
    // is why *its* copy of this same 35px offset goes to x_gap instead).
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, PANEL_X_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    init_backlight();

    hagl_bitmap_init(&s_bitmap, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_DEPTH,
                      (void *)s_frame_buffer);

    backend->width = DISPLAY_WIDTH;
    backend->height = DISPLAY_HEIGHT;
    backend->depth = DISPLAY_DEPTH;
    backend->put_pixel = trampoline_put_pixel;
    backend->get_pixel = trampoline_get_pixel;
    backend->color = trampoline_color;
    backend->blit = trampoline_blit;
    backend->scale_blit = trampoline_scale_blit;
    backend->hline = trampoline_hline;
    backend->vline = trampoline_vline;
    backend->flush = backend_flush;
    backend->close = backend_close;
    backend->buffer = (uint8_t *)s_frame_buffer;
    backend->buffer2 = NULL;

    ESP_LOGI(TAG, "T-Display S3 I80 backend ready: %dx%d, frame buffer %u bytes",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, (unsigned)frame_bytes);
}
