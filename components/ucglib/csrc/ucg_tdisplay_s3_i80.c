/*
 * T-Display S3 8-bit Intel 8080 transport for Ucglib.
 *
 * The standard LilyGO T-Display S3 has an ST7789V driven over an 8-bit
 * Intel 8080 bus.  This adapter deliberately uses ESP-IDF's esp_lcd panel
 * driver instead of the SPI-only Ucglib ESP32 HAL.
 */
#include <stddef.h>

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "ucg_tdisplay_s3_i80.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3) && SOC_LCD_I80_SUPPORTED

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "ucg_tdisplay_s3"

#define TDISPLAY_S3_WIDTH 170
#define TDISPLAY_S3_HEIGHT 320
#define TDISPLAY_S3_X_GAP 35
#define TDISPLAY_S3_LINE_PIXELS TDISPLAY_S3_HEIGHT

typedef struct {
    bool configured;
    bool initialized;
    bool asleep;
    ucg_tdisplay_s3_i80_pins_t pins;
    esp_lcd_i80_bus_handle_t bus;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t color_done;
    uint16_t *draw_buffer;
} tdisplay_s3_i80_state_t;

static tdisplay_s3_i80_state_t s_state;

static bool is_valid_gpio(gpio_num_t pin)
{
    return (int)pin >= GPIO_NUM_0 && (int)pin < GPIO_NUM_MAX;
}

static bool pins_are_valid(const ucg_tdisplay_s3_i80_pins_t *pins)
{
    if (pins == NULL || !is_valid_gpio(pins->power) ||
        !is_valid_gpio(pins->backlight) || !is_valid_gpio(pins->reset) ||
        !is_valid_gpio(pins->cs) || !is_valid_gpio(pins->dc) ||
        !is_valid_gpio(pins->wr) || !is_valid_gpio(pins->rd)) {
        return false;
    }

    for (size_t index = 0; index < 8; ++index) {
        if (!is_valid_gpio(pins->data[index])) {
            return false;
        }
    }

    return true;
}

static bool check_result(esp_err_t result, const char *operation)
{
    if (result == ESP_OK) {
        return true;
    }

    ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(result));
    return false;
}

static bool color_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *event_data,
                                void *user_ctx)
{
    (void)panel_io;
    (void)event_data;

    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx,
                          &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static bool wake_panel(void)
{
    if (!s_state.asleep) {
        return true;
    }

    if (!check_result(esp_lcd_panel_disp_sleep(s_state.panel, false),
                      "wake LCD panel") ||
        !check_result(esp_lcd_panel_disp_on_off(s_state.panel, true),
                      "turn LCD panel on")) {
        return false;
    }

    gpio_set_level(s_state.pins.backlight, 1);
    s_state.asleep = false;
    return true;
}

static bool initialize_panel(void)
{
    if (s_state.initialized) {
        return wake_panel();
    }

    if (!s_state.configured || !pins_are_valid(&s_state.pins)) {
        ESP_LOGE(TAG, "invalid T-Display S3 pin configuration");
        return false;
    }

    gpio_config_t control_config = {
        .pin_bit_mask = (1ULL << (uint32_t)s_state.pins.power) |
                        (1ULL << (uint32_t)s_state.pins.backlight) |
                        (1ULL << (uint32_t)s_state.pins.rd),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (!check_result(gpio_config(&control_config), "configure LCD control pins")) {
        return false;
    }

    /* GPIO15 enables panel power; GPIO9 is tied high for write-only use. */
    gpio_set_level(s_state.pins.power, 1);
    gpio_set_level(s_state.pins.backlight, 0);
    gpio_set_level(s_state.pins.rd, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    esp_lcd_i80_bus_config_t bus_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = s_state.pins.dc,
        .wr_gpio_num = s_state.pins.wr,
        .data_gpio_nums = {
            s_state.pins.data[0], s_state.pins.data[1],
            s_state.pins.data[2], s_state.pins.data[3],
            s_state.pins.data[4], s_state.pins.data[5],
            s_state.pins.data[6], s_state.pins.data[7],
        },
        .bus_width = 8,
        .max_transfer_bytes = TDISPLAY_S3_LINE_PIXELS * sizeof(uint16_t),
        .dma_burst_size = 16,
    };
    if (!check_result(esp_lcd_new_i80_bus(&bus_config, &s_state.bus),
                      "create LCD I80 bus")) {
        return false;
    }

    s_state.color_done = xSemaphoreCreateBinary();
    if (s_state.color_done == NULL) {
        ESP_LOGE(TAG, "allocate LCD DMA semaphore failed");
        return false;
    }

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = s_state.pins.cs,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 1,
        .on_color_trans_done = color_transfer_done,
        .user_ctx = s_state.color_done,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .swap_color_bytes = 1,
        },
    };
    if (!check_result(esp_lcd_new_panel_io_i80(s_state.bus, &io_config,
                                               &s_state.io),
                      "create LCD I80 panel IO")) {
        return false;
    }

    s_state.draw_buffer = esp_lcd_i80_alloc_draw_buffer(
        s_state.io, TDISPLAY_S3_LINE_PIXELS * sizeof(uint16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (s_state.draw_buffer == NULL) {
        ESP_LOGE(TAG, "allocate LCD DMA buffer failed");
        return false;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = s_state.pins.reset,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    if (!check_result(esp_lcd_new_panel_st7789(s_state.io, &panel_config,
                                               &s_state.panel),
                      "create ST7789 panel") ||
        !check_result(esp_lcd_panel_reset(s_state.panel), "reset LCD panel") ||
        !check_result(esp_lcd_panel_init(s_state.panel), "initialize LCD panel") ||
        !check_result(esp_lcd_panel_invert_color(s_state.panel, true),
                      "enable LCD inversion") ||
        !check_result(esp_lcd_panel_set_gap(s_state.panel, TDISPLAY_S3_X_GAP, 0),
                      "set LCD panel gap") ||
        !check_result(esp_lcd_panel_disp_on_off(s_state.panel, true),
                      "turn LCD panel on")) {
        return false;
    }

    gpio_set_level(s_state.pins.backlight, 1);
    s_state.initialized = true;
    s_state.asleep = false;
    return true;
}

static void sleep_panel(void)
{
    if (!s_state.initialized || s_state.asleep) {
        return;
    }

    gpio_set_level(s_state.pins.backlight, 0);
    (void)check_result(esp_lcd_panel_disp_on_off(s_state.panel, false),
                       "turn LCD panel off");
    (void)check_result(esp_lcd_panel_disp_sleep(s_state.panel, true),
                       "sleep LCD panel");
    s_state.asleep = true;
}

bool ucg_tdisplay_s3_i80_configure(const ucg_tdisplay_s3_i80_pins_t *pins)
{
    if (!pins_are_valid(pins)) {
        return false;
    }

    if (s_state.initialized) {
        ESP_LOGE(TAG, "T-Display S3 transport cannot be reconfigured");
        return false;
    }

    s_state.pins = *pins;
    s_state.configured = true;
    return true;
}

uint16_t *ucg_tdisplay_s3_i80_draw_buffer(void)
{
    return s_state.initialized ? s_state.draw_buffer : NULL;
}

bool ucg_tdisplay_s3_i80_submit_buffer(int x, int y, int width, int height)
{
    if (!s_state.initialized || s_state.draw_buffer == NULL || width <= 0 ||
        height <= 0 || x < 0 || y < 0 || width > TDISPLAY_S3_WIDTH - x ||
        height > TDISPLAY_S3_HEIGHT - y ||
        (size_t)width * (size_t)height > TDISPLAY_S3_LINE_PIXELS) {
        return false;
    }

    if (!check_result(esp_lcd_panel_draw_bitmap(s_state.panel, x, y,
                                                x + width, y + height,
                                                s_state.draw_buffer),
                      "draw LCD bitmap")) {
        return false;
    }

    if (xSemaphoreTake(s_state.color_done, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "wait for LCD DMA transfer failed");
        return false;
    }

    return true;
}

bool ucg_tdisplay_s3_i80_fill_rect(int x, int y, int width, int height,
                                   const uint8_t color[3])
{
    if (!s_state.initialized || color == NULL || width <= 0 || height <= 0 ||
        x < 0 || y < 0 || width > TDISPLAY_S3_WIDTH - x ||
        height > TDISPLAY_S3_HEIGHT - y) {
        return false;
    }

    const uint16_t color565 = ((uint16_t)(color[0] & 0xf8) << 8) |
                              ((uint16_t)(color[1] & 0xfc) << 3) |
                              ((uint16_t)color[2] >> 3);
    int remaining_rows = height;
    int row = y;
    while (remaining_rows > 0) {
        const int rows = remaining_rows < (TDISPLAY_S3_LINE_PIXELS / width)
                             ? remaining_rows
                             : TDISPLAY_S3_LINE_PIXELS / width;
        const size_t pixels = (size_t)width * (size_t)rows;
        for (size_t index = 0; index < pixels; ++index) {
            s_state.draw_buffer[index] = color565;
        }

        if (!ucg_tdisplay_s3_i80_submit_buffer(x, row, width, rows)) {
            return false;
        }

        row += rows;
        remaining_rows -= rows;
    }

    return true;
}

int16_t ucg_com_tdisplay_s3_i80(ucg_t *ucg, int16_t msg, uint16_t arg,
                                uint8_t *data)
{
    (void)ucg;
    (void)arg;
    (void)data;

    switch (msg) {
    case UCG_COM_MSG_POWER_UP:
        return initialize_panel() ? 1 : 0;
    case UCG_COM_MSG_POWER_DOWN:
        sleep_panel();
        break;
    default:
        break;
    }

    return 1;
}

#else

bool ucg_tdisplay_s3_i80_configure(const ucg_tdisplay_s3_i80_pins_t *pins)
{
    (void)pins;
    return false;
}

uint16_t *ucg_tdisplay_s3_i80_draw_buffer(void)
{
    return NULL;
}

bool ucg_tdisplay_s3_i80_submit_buffer(int x, int y, int width, int height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    return false;
}

bool ucg_tdisplay_s3_i80_fill_rect(int x, int y, int width, int height,
                                   const uint8_t color[3])
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)color;
    return false;
}

int16_t ucg_com_tdisplay_s3_i80(ucg_t *ucg, int16_t msg, uint16_t arg,
                                uint8_t *data)
{
    (void)ucg;
    (void)msg;
    (void)arg;
    (void)data;
    return 0;
}

#endif /* CONFIG_IDF_TARGET_ESP32S3 && SOC_LCD_I80_SUPPORTED */
