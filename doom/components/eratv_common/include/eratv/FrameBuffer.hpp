/*
 * FrameBuffer.hpp
 *
 *  Created on: 5 May 2024
 *      Author: user
 */

#ifndef MAIN_FRAMEBUFFER_HPP_
#define MAIN_FRAMEBUFFER_HPP_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <wchar.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

#include <hagl_hal.h>
#include <hagl.h>

#include "hagl/bitmap.h"
#include <font6x9.h>
#include <functional>

#ifndef ERATV_SPLASH_HEADER
#define ERATV_SPLASH_HEADER "eratv/resources/splashimg.h"
#endif

#include ERATV_SPLASH_HEADER

#include "driver/ledc.h"

namespace EraTV{

static const uint8_t FRAME_LOADED = (1 << 0);
// Set by flush_task after hagl_flush() completes for a loaded frame. Lets a
// producer that writes directly into the shared back buffer (e.g. the MJPEG
// decode task) wait until the previous frame has finished transmitting before
// it starts overwriting the buffer, preventing torn/half-updated frames.
static const uint8_t FLUSH_COMPLETE = (1 << 1);

void fb_flush_task(void *params);

	class FrameBuffer{
	public:

		FrameBuffer(){
			bb = NULL;
			current_brightness = 100; // Default to full brightness
			frame_milestone_counter = 0;
			onFrameCountCallback = nullptr;
			oldTvHitEffectEnabled = true;
		}

		void begin(){
			/* Save the backbuffer pointer so we can later read() directly into it. */
			    bb = hagl_init();
			    if (!bb) {
			        ESP_LOGE(TAG, "hagl_init() failed, framebuffer unavailable");
			        return;
			    }
			    // Some HAL paths may return a backend object even when buffer allocation fails.
			    if (!bb->buffer) {
			        ESP_LOGE(TAG, "hagl backend created but framebuffer buffer is NULL");
			        bb = NULL;
			        return;
			    }
			    ESP_LOGI(TAG, "Back buffer: %dx%dx%d", bb->width, bb->height, bb->depth);

			    hagl_clear(bb);
				set_backlight_brightness(100); // Set initial brightness to 100%
				//Draw the splash screen
				hagl_bitmap_t splash_bitmap;
			    splash_bitmap.width = SPLASH_IMG_WIDTH;
			    splash_bitmap.height = SPLASH_IMG_HEIGHT;
			    splash_bitmap.depth = 16; // RGB565
			    splash_bitmap.buffer = (uint8_t*)EraTV_small;

			    // Blit the splash image to the center of the screen
			    //hagl_blit_xywh(bb, 0, 30, SPLASH_IMG_WIDTH, SPLASH_IMG_HEIGHT, &splash_bitmap);
				hagl_blit_xy(bb, 0, 13, &splash_bitmap);
			    
				hagl_flush(bb);
			    event = xEventGroupCreate();
			    /* Hard mutual-exclusion guard for the single shared back buffer.
			     * Held while flushing here and while a direct-to-backbuffer
			     * producer (MJPEG decode task) writes a frame, so the two cores
			     * can never touch the buffer at the same time (prevents tearing
			     * even if the event-bit pacing barrier slips). */
			    bb_mutex = xSemaphoreCreateMutex();
				vTaskDelay(pdMS_TO_TICKS(1000));//just wait here
			    xTaskCreatePinnedToCore(fb_flush_task, "Flush", 4096, this, 1, NULL, 0); //8192

		}

		void set_backlight_brightness(uint8_t brightness_percent)
		{
			prev_brightness = current_brightness;
			current_brightness = brightness_percent;
    		uint32_t duty = (brightness_percent * ((1 << LEDC_TIMER_13_BIT) - 1)) / 100;
    		ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    		ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
		}


		hagl_backend_t *getBuffer(){
			return bb;
		}

		EventGroupHandle_t *getEvent(){
			return &event;
		}

		/* Mutex guarding all access to the shared back buffer. A producer that
		 * writes directly into the back buffer should hold this for the whole
		 * frame write so it never overlaps a flush. */
		SemaphoreHandle_t getBackBufferMutex(){
			return bb_mutex;
		}

		void increase_brightness() {
			if (current_brightness <= 90) {
				current_brightness += 10;
			} else {
				current_brightness = 100;
			}
			set_backlight_brightness(current_brightness);
		}

		void decrease_brightness() {
			if (current_brightness >= 20) {
				current_brightness -= 10;
			} else {
				current_brightness = 10;
			}
			set_backlight_brightness(current_brightness);
		}

		uint8_t get_brightness() const {
			return current_brightness;
		}
		uint8_t get_prev_brightness() const {
			return prev_brightness;
		}

		void setFrameCountHandler(const std::function<void(uint32_t)>& handler) {
			ESP_LOGI(TAG, "Setting frame count handler, input valid: %s", handler ? "yes" : "no");
			onFrameCountCallback = handler;
			ESP_LOGI(TAG, "Handler set, callback valid: %s", onFrameCountCallback ? "yes" : "no");
		}

		void triggerOldTvHitEffect(uint32_t duration_ms = 500) {
			if (!oldTvHitEffectEnabled) {
				return;
			}
			old_tv_hit_effect_end_us = esp_timer_get_time() + (static_cast<int64_t>(duration_ms) * 1000);
			// Wake flush task immediately so effect starts without waiting for next frame decode.
			xEventGroupSetBits(event, FRAME_LOADED);
		}

		void setOldTvHitEffectEnabled(bool enabled) {
			oldTvHitEffectEnabled = enabled;
			if (!enabled) {
				old_tv_hit_effect_end_us = 0;
			}
		}

		// Old-time CRT power-off animation. Runs synchronously (blocks the
		// caller for ~total_ms) and owns the back buffer for the whole effect:
		//   1) the picture collapses into a thin bright horizontal line,
		//   2) the line squeezes into a small ~10x10 dot in the screen centre,
		//   3) the dot fades to black.
		// The caller MUST stop any producer that writes the back buffer (e.g.
		// the video decode task) before calling this, so it can hold the
		// back-buffer mutex exclusively for the duration.
		void playPowerOffAnimation(uint32_t total_ms = 2000) {
			if (!bb) {
				return;
			}

			// Disable the snow/hit effect so the flush task never redraws over
			// the animation, and take exclusive ownership of the back buffer.
			oldTvHitEffectEnabled = false;
			old_tv_hit_effect_end_us = 0;
			if (bb_mutex) {
				xSemaphoreTake(bb_mutex, portMAX_DELAY);
			}

			const int16_t w = bb->width;
			const int16_t h = bb->height;
			const int16_t cx = w / 2;
			const int16_t cy = h / 2;

			const int64_t total_us = static_cast<int64_t>(total_ms) * 1000;
			const int64_t p1_us = total_us * 40 / 100; // vertical collapse -> line
			const int64_t p2_us = total_us * 65 / 100; // horizontal collapse -> dot
			// remainder (p2_us .. total_us): the dot fades out.

			const hagl_color_t black = hagl_color(bb, 0, 0, 0);
			const hagl_color_t white = hagl_color(bb, 255, 255, 255);

			const int64_t start = esp_timer_get_time();
			int64_t t;
			while ((t = esp_timer_get_time() - start) < total_us) {
				hagl_fill_rectangle_xyxy(bb, 0, 0, w - 1, h - 1, black);

				if (t < p1_us) {
					// Vertical collapse: full-width bright band shrinks to a line.
					float p = static_cast<float>(t) / static_cast<float>(p1_us);
					int16_t half_h = static_cast<int16_t>((1.0f - p) * (h / 2));
					int16_t y0 = cy - half_h;
					int16_t y1 = cy + half_h;
					if (y0 < 0) y0 = 0;
					if (y1 > h - 1) y1 = h - 1;
					hagl_fill_rectangle_xyxy(bb, 0, y0, w - 1, y1,
											 hagl_color(bb, 235, 245, 255));
					// Brighter phosphor core line.
					hagl_fill_rectangle_xyxy(bb, 0, cy - 1, w - 1, cy + 1, white);
				} else if (t < p2_us) {
					// Horizontal collapse: the line squeezes toward the centre.
					float p = static_cast<float>(t - p1_us) /
							  static_cast<float>(p2_us - p1_us);
					int16_t half_w = static_cast<int16_t>((1.0f - p) * (w / 2));
					if (half_w < 6) half_w = 6;
					int16_t x0 = cx - half_w;
					int16_t x1 = cx + half_w;
					if (x0 < 0) x0 = 0;
					if (x1 > w - 1) x1 = w - 1;
					hagl_fill_rectangle_xyxy(bb, x0, cy - 2, x1, cy + 2, white);
				} else {
					// Dot fade: a ~10x10 bright dot fades to black.
					float p = static_cast<float>(t - p2_us) /
							  static_cast<float>(total_us - p2_us);
					uint8_t lvl = static_cast<uint8_t>(255.0f * (1.0f - p));
					hagl_fill_circle(bb, cx, cy, 5, hagl_color(bb, lvl, lvl, lvl));
				}

				hagl_flush(bb);
				vTaskDelay(pdMS_TO_TICKS(25));
			}

			// Guarantee a fully black screen at the end.
			hagl_fill_rectangle_xyxy(bb, 0, 0, w - 1, h - 1, black);
			hagl_flush(bb);

			if (bb_mutex) {
				xSemaphoreGive(bb_mutex);
			}
		}

		// Force an immediate display refresh — used by menu navigation when video is stopped.
		void forceFlush() {
			xEventGroupSetBits(event, FRAME_LOADED);
		}

		/*
		 * Flush backbuffer to display always when new frame is loaded.
		 */
		void flush_task(void *params)
		{
			//hagl_bitmap_t bitmap;
			//bitmap.buffer = (uint8_t *) malloc(6 * 9 * sizeof(hagl_color_t));
			while (1) {
				EventBits_t bits = xEventGroupWaitBits(
					event,
					FRAME_LOADED,
					pdTRUE,
					pdFALSE,
					40 / portTICK_PERIOD_MS
				);
				bool frame_loaded = ((bits & FRAME_LOADED) != 0);
				bool effect_active = isOldTvHitEffectActive();

				if (frame_loaded || effect_active) {
					if (bb_mutex) {
						xSemaphoreTake(bb_mutex, portMAX_DELAY);
					}

					/* Draw overlays while holding the same mutex used by the decode
					 * path, so video bands cannot overdraw OSD elements (mute/volume)
					 * between draw operations. */
					if (frame_loaded) {
						if (onFrameCountCallback) {
							onFrameCountCallback(frame_milestone_counter);
						}
					}

					if (effect_active) {
						applyOldTvHitEffectFrame();
					}
					hagl_flush(bb);
					if (bb_mutex) {
						xSemaphoreGive(bb_mutex);
					}
				}

				/* Signal frame-level flush completion so a direct-to-backbuffer
				 * producer can safely start the next frame. Only meaningful for
				 * real frames (not the standalone old-TV effect refreshes). */
				if (frame_loaded) {
					xEventGroupSetBits(event, FLUSH_COMPLETE);
				}
			}

			vTaskDelete(NULL);
		}


	protected:
		hagl_backend_t *bb;
		EventGroupHandle_t event;
		SemaphoreHandle_t bb_mutex = nullptr;
		uint8_t current_brightness; // Brightness level (0-100)
		uint8_t prev_brightness; // Brightness level (0-100)
		uint8_t frame_milestone_counter; // Counter for 20-frame milestones (0-19)
		std::function<void(uint32_t)> onFrameCountCallback; // Callback for every 20 frames
		volatile int64_t old_tv_hit_effect_end_us = 0;
		bool oldTvHitEffectEnabled = true;

		bool isOldTvHitEffectActive() const {
			return oldTvHitEffectEnabled && (old_tv_hit_effect_end_us > esp_timer_get_time());
		}

		void applyOldTvHitEffectFrame() {
			if (!bb) {
				return;
			}

			const int w = bb->width;
			const int h = bb->height;
			const int y0 = 13;
			const int video_h = 213;
			const int y_end = (y0 + video_h < h) ? (y0 + video_h) : h; // exclusive upper bound
			const int hh = y_end - y0;
			if (hh <= 0) {
				return;
			}

			// Light snow over current frame.
			for (int i = 0; i < 500; ++i) {
				int x = rand() % w;
				int y = y0 + (rand() % hh);
				uint8_t n = static_cast<uint8_t>(170 + (rand() % 86));
				hagl_put_pixel(bb, x, y, hagl_color(bb, n, n, n));
			}

			// Horizontal tearing bands to mimic a hit/skew glitch.
			for (int b = 0; b < 6; ++b) {
				int y = y0 + (rand() % hh);
				int band_h = 2 + (rand() % 4);
				int shift = (rand() % 31) - 15;
				for (int yy = y; yy < y + band_h && yy < y_end; ++yy) {
					for (int x = 0; x < w; x += 2) {
						int sx = x + shift;
						if (sx >= 0 && sx < w) {
							uint8_t c = static_cast<uint8_t>(120 + (rand() % 120));
							hagl_put_pixel(bb, sx, yy, hagl_color(bb, c, c, c));
						}
					}
				}
			}
		}

	private:
		const char *TAG = "EraTV::FrameBuffer";
	};

	void fb_flush_task(void *params){
		((FrameBuffer *)params)->flush_task(NULL);
	}
}


#endif /* MAIN_FRAMEBUFFER_HPP_ */
