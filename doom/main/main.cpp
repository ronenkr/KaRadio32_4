/*
 * Doom (PrBoom) Emulator Standalone App
 *
 * Boots from the ota_1 partition (see partitions.ttgo_tdisplay_s3.csv,
 * shared with KaRadio32_4). Initializes the T-Display S3 display/audio/
 * input stack, then runs the PrBoom Doom engine. The engine drives its own
 * main loop (D_DoomMain never returns); this file provides the full I_*
 * platform layer the engine expects:
 *   - video    -> EraTV FrameBuffer (hagl, T-Display S3 parallel/I80 panel,
 *                 320x170 RGB565 - see components/eratv_common/src/hagl_hal_i80.c).
 *                 The engine itself still renders its classic 320x200 canvas
 *                 (PrBoom hardcodes status-bar geometry to a 200-tall screen,
 *                 see st_stuff.h's ST_Y) - I_FinishUpdate crops 15 rows off
 *                 the top and 15 off the bottom to fit the 170-tall panel.
 *   - audio    -> EraTV AudioPlayer (PCM16 mono @ 22050 Hz, plain I2S to
 *                 this board's PCM5102A DAC)
 *   - input    -> EraTV BluetoothJoystick mapped to Doom key events (kept
 *                 unchanged from the original)
 *   - lifecycle-> return_to_karadio() (Plus+Minus reboots back into
 *                 KaRadio32_4's ota_0, symmetric to its own sys.launchapp)
 *
 * WAD files live in the `littlefs` partition, mounted at /littlefs by
 * EraTV::FileSystem - see flash_wads.sh to write them there.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <cinttypes>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hagl.h>
#include <hagl_hal.h>
#include <hagl/char.h>
#include <hagl/rectangle.h>
#include <fontx.h>

#include "eratv/BluetoothJoystick.hpp"
#include "eratv/FileSystem.hpp"
#include "eratv/FrameBuffer.hpp"
#include "eratv/player/AudioPlayer.hpp"
#include "driver/gpio.h"

// Boots back into KaRadio32_4's ota_0 partition. Symmetric counterpart to
// KaRadio's own launch_second_app() (main/ota.c) - same mechanism
// (esp_ota_set_boot_partition + esp_restart), so either side can hand off
// to the other. If this ever proves unreliable in testing (e.g. under
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE), the EraTV original used a
// different trick here - erasing the otadata partition directly - to dodge
// an image-validation failure mode; worth trying if this doesn't boot back
// into KaRadio cleanly.
static void return_to_karadio(void)
{
    ESP_LOGI("Doom_App", "return_to_karadio: called");
    const esp_partition_t *karadio = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (karadio == NULL) {
        ESP_LOGE("Doom_App", "return_to_karadio: ota_0 partition not found");
        return;
    }
    esp_err_t err = esp_ota_set_boot_partition(karadio);
    if (err != ESP_OK) {
        ESP_LOGE("Doom_App", "return_to_karadio: esp_ota_set_boot_partition failed, error=%d", err);
        return;
    }
    ESP_LOGI("Doom_App", "return_to_karadio: boot partition set to %s at 0x%06" PRIx32 ", restarting",
             karadio->label, karadio->address);
    esp_restart();
}

extern "C" {
#include "doomtype.h"
#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "d_event.h"
#include "g_game.h"
#include "i_system.h"
#include "i_video.h"
#include "i_sound.h"
#include "i_main.h"
#include "m_argv.h"
#include "m_misc.h"
#include "r_fps.h"
#include "s_sound.h"
#include "st_stuff.h"
#include "v_video.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"
#include "mus2mid.h"
#include "oplplayer.h"
}

static const char *TAG = "Doom_App";

// ── EraTV hardware stack ──────────────────────────────────────────────────────
// PMU and Led are gone entirely: T-Display S3 has no AXP2101 power-management
// chip and no addressable status LED wired the way the original board had -
// both were confirmed to have zero data dependency for game logic on the
// original hardware either (PMU only sequenced a power rail that doesn't
// exist as a separate concept here; Led was purely a cosmetic status color).

static EraTV::FrameBuffer fb;
static EraTV::FileSystem fs;
static EraTV::BluetoothJoystick btJoystick;
static EraTV::AudioPlayer audioPlayer;

// Handles used to hand the "return to KaRadio" flash sequence off the Doom
// task. Flash operations (here, esp_ota_set_boot_partition's otadata write,
// same as every littlefs/WAD read - see the doomTask stack comment in
// app_main()) disable cache access; the main task performs it after
// suspending the Doom task, mainly so nothing is still trying to render
// frames or read WADs mid-reboot.
static TaskHandle_t g_mainTaskHandle = nullptr;
static TaskHandle_t g_doomTaskHandle = nullptr;

// ── Doom audio configuration ─────────────────────────────────────────────────

#define AUDIO_SAMPLE_RATE   22050
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / TICRATE + 1)
#define NUM_MIX_CHANNELS    8

// ── Globals expected by the Doom engine ──────────────────────────────────────

extern "C" {
int snd_card = 1, mus_card = 1;
int snd_samplerate = AUDIO_SAMPLE_RATE;
int current_palette = 0;
}

// ── Video state ──────────────────────────────────────────────────────────────

static uint8_t *doom_fb = nullptr;           // 8-bit indexed framebuffer (screens[0])
static uint16_t doom_palette[256] = {0};     // active RGB565 palette (native order)
static char g_doomDir[64] = "/littlefs";  // directory holding the WAD files

// The engine renders its classic 320x200 canvas (SCREENWIDTH/SCREENHEIGHT
// below) regardless of the physical panel - PrBoom hardcodes status-bar
// placement to a 200-tall screen (st_stuff.h: ST_Y = 200 - ST_HEIGHT, not
// scaled by SCREENHEIGHT), so shrinking SCREENHEIGHT itself misplaces the
// status bar instead of just losing a few rows of view. Since the panel is
// only 170 rows tall, I_FinishUpdate instead crops the rendered frame evenly
// top and bottom to fit, leaving the full-resolution render intact.
static constexpr int kEngineWidth  = MAX_SCREENWIDTH;   // 320, matches DISPLAY_WIDTH
static constexpr int kEngineHeight = 200;                // classic Doom canvas height
static_assert(DISPLAY_WIDTH == kEngineWidth,
              "I_FinishUpdate's crop copy assumes the panel and engine widths match");
static_assert(DISPLAY_HEIGHT <= kEngineHeight,
              "crop math assumes the panel is no taller than the classic canvas");
static constexpr int kCropRows = (kEngineHeight - DISPLAY_HEIGHT) / 2; // rows dropped off top (and bottom)

// ── EraTV OSD overlay menu ───────────────────────────────────────────────────
// Opened with L2 (L1/R1 are reserved for Doom strafing). While visible, joystick
// input drives the menu instead of the game, and renderMenuOverlay() draws it on
// top of the framebuffer each frame.

static const unsigned char *const kMenuFont = font6x9;
static constexpr int kDisplayWidth  = DISPLAY_WIDTH;   // 320, T-Display S3 landscape
static constexpr int kDisplayHeight = DISPLAY_HEIGHT;  // 170
static constexpr uint8_t kMenuTextScale = 2;

enum class MenuItem : uint8_t {
    ReturnToRadio = 0,
    Count,
};

struct OsdState {
    bool menuVisible = false;
    MenuItem menuSelection = MenuItem::ReturnToRadio;
} g_osd;

// ── OSD settings persistence ─────────────────────────────────────────────────
// Volume/mute (audioPlayer's software PCM gain, not Doom's own snd_SfxVolume/
// snd_MusicVolume mixers, which persist through PrBoom's own M_SaveDefaults()/
// M_LoadDefaults()) are no longer user-adjustable from the OSD menu (see
// MenuItem), but still applied to actual playback - restore whatever was
// last saved, if anything was.

static constexpr const char *kSettingsPath = "/littlefs/doom_settings.bin";
static constexpr uint32_t kSettingsMagic = 0x444F4D31; // "DOM1"

struct DoomSettings {
    uint32_t magic;
    uint8_t volume;
    uint8_t muted;
};

static void loadSettings()
{
    FILE *f = fopen(kSettingsPath, "rb");
    if (!f) {
        ESP_LOGI(TAG, "No saved settings at %s, using defaults", kSettingsPath);
        return;
    }
    DoomSettings s = {};
    size_t n = fread(&s, sizeof(s), 1, f);
    fclose(f);
    if (n != 1 || s.magic != kSettingsMagic) {
        ESP_LOGW(TAG, "Settings file at %s missing/corrupt, using defaults", kSettingsPath);
        return;
    }
    audioPlayer.setVolume(s.volume);
    audioPlayer.setMute(s.muted != 0);
    ESP_LOGI(TAG, "Loaded settings: volume=%u muted=%d", s.volume, s.muted != 0);
}

// ── Sound mixing state ───────────────────────────────────────────────────────

typedef struct {
    uint16_t unused1;
    uint16_t samplerate;
    uint16_t length;
    uint16_t unused2;
    byte samples[];
} doom_sfx_t;

typedef struct {
    const doom_sfx_t *sfx;
    size_t pos;
    float factor;
    int starttic;
} channel_t;

static channel_t channels[NUM_MIX_CHANNELS];
static const doom_sfx_t *sfx[NUMSFX];
static const music_player_t *music_player = &opl_synth_player;
static bool musicPlaying = false;

// Stereo scratch for the OPL renderer, plus the mono output we hand to the codec.
// Kept in PSRAM (audio task only) to preserve internal RAM for the framebuffer.
EXT_RAM_BSS_ATTR static int16_t musicMix[AUDIO_BUFFER_LENGTH * 2];
EXT_RAM_BSS_ATTR static int16_t monoOut[AUDIO_BUFFER_LENGTH];

// ── Input mapping ─────────────────────────────────────────────────────────────

using Btn = EraTV::BluetoothJoystick::Button;

static const struct { Btn button; int *key; } keymap[] = {
    {Btn::DPAD_UP,    &key_up},
    {Btn::DPAD_DOWN,  &key_down},
    {Btn::DPAD_LEFT,  &key_left},
    {Btn::DPAD_RIGHT, &key_right},
    {Btn::BTN_A,      &key_fire},
    {Btn::BTN_A,      &key_backspace},    // back one level in Doom's own menu
    {Btn::BTN_B,      &key_use},
    {Btn::BTN_B,      &key_enter},        // confirm in menus
    {Btn::BTN_X,      &key_speed},        // run
    {Btn::BTN_Y,      &key_weapontoggle}, // cycle weapons
    {Btn::BTN_L1,     &key_strafeleft},
    {Btn::BTN_R1,     &key_straferight},
    {Btn::BTN_R2,     &key_strafe},        // hold-to-strafe modifier (L2 = EraTV menu)
    {Btn::BTN_PLUS,   &key_escape},       // open Doom menu
    {Btn::BTN_MINUS,  &key_map},          // automap
    {Btn::BTN_HOME,   &key_escape},
};

static inline bool isPressed(uint32_t mask, Btn button)
{
    return (mask & (1u << static_cast<uint8_t>(button))) != 0;
}

// Expand diagonal D-pad bits into clean cardinal directions.
static uint32_t normalizeDpadMask(uint32_t raw)
{
    const bool up    = isPressed(raw, Btn::DPAD_UP)    || isPressed(raw, Btn::DPAD_UP_LEFT)   || isPressed(raw, Btn::DPAD_UP_RIGHT);
    const bool down  = isPressed(raw, Btn::DPAD_DOWN)  || isPressed(raw, Btn::DPAD_DOWN_LEFT) || isPressed(raw, Btn::DPAD_DOWN_RIGHT);
    const bool left  = isPressed(raw, Btn::DPAD_LEFT)  || isPressed(raw, Btn::DPAD_UP_LEFT)   || isPressed(raw, Btn::DPAD_DOWN_LEFT);
    const bool right = isPressed(raw, Btn::DPAD_RIGHT) || isPressed(raw, Btn::DPAD_UP_RIGHT)  || isPressed(raw, Btn::DPAD_DOWN_RIGHT);

    auto setBit = [](uint32_t m, Btn b, bool v) -> uint32_t {
        const uint32_t bit = 1u << static_cast<uint8_t>(b);
        return v ? (m | bit) : (m & ~bit);
    };
    uint32_t n = raw;
    n = setBit(n, Btn::DPAD_UP,    up);
    n = setBit(n, Btn::DPAD_DOWN,  down);
    n = setBit(n, Btn::DPAD_LEFT,  left);
    n = setBit(n, Btn::DPAD_RIGHT, right);
    return n;
}

// ──────────────────────────────────────────────────────────────────────────────
// Video (i_video.h)
// ──────────────────────────────────────────────────────────────────────────────

extern "C" void I_StartFrame(void) {}
extern "C" void I_UpdateNoBlit(void) {}
extern "C" bool I_StartDisplay(void) { return true; }
extern "C" void I_EndDisplay(void) {}

// ── OSD overlay helpers ───────────────────────────────────────────────────────

static hagl_color_t uiColor(hagl_backend_t *backend, uint8_t r, uint8_t g, uint8_t b)
{
    if (backend) return hagl_color(backend, r, g, b);
    return static_cast<uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static void drawMenuText(hagl_backend_t *backend, const wchar_t *text,
                         int16_t x, int16_t y, hagl_color_t color,
                         uint8_t scale = kMenuTextScale)
{
    if (!backend || !text || scale == 0) return;

    fontx_meta_t meta = {};
    if (fontx_meta(&meta, kMenuFont) != FONTX_OK) {
        hagl_put_text(backend, text, x, y, color, kMenuFont);
        return;
    }

    int16_t cursorX = x;
    for (const wchar_t *c = text; *c; ++c) {
        fontx_glyph_t glyph = {};
        if (fontx_glyph(&glyph, *c, kMenuFont) != FONTX_OK) {
            cursorX += static_cast<int16_t>(meta.width * scale);
            continue;
        }
        const uint8_t *row = glyph.buffer;
        for (uint8_t gy = 0; gy < glyph.height; ++gy) {
            for (uint8_t gx = 0; gx < glyph.width; ++gx) {
                if (row[gx / 8] & (0x80u >> (gx % 8))) {
                    hagl_fill_rectangle_xywh(backend,
                        cursorX + gx * scale, y + gy * scale, scale, scale, color);
                }
            }
            row += glyph.pitch;
        }
        cursorX += static_cast<int16_t>(glyph.width * scale);
    }
}

static void setMenuVisible(bool visible)
{
    if (g_osd.menuVisible == visible) return;
    g_osd.menuVisible = visible;
    if (visible) g_osd.menuSelection = MenuItem::ReturnToRadio;
    ESP_LOGI(TAG, "OSD menu %s", visible ? "opened" : "closed");
}

static const char *menuItemName(MenuItem item)
{
    switch (item) {
    case MenuItem::ReturnToRadio: return "ReturnToRadio";
    case MenuItem::Count:         return "Count";
    }
    return "?";
}

static void activateMenuSelection()
{
    ESP_LOGI(TAG, "OSD menu select: %s", menuItemName(g_osd.menuSelection));
    switch (g_osd.menuSelection) {
    case MenuItem::ReturnToRadio:
        ESP_LOGI(TAG, "ReturnToRadio selected, calling I_SafeExit");
        I_SafeExit(0);
        break;
    case MenuItem::Count: break;
    }
}

static void renderMenuOverlay(hagl_backend_t *backend)
{
    if (!g_osd.menuVisible || !backend) return;

    const hagl_color_t backdrop     = uiColor(backend,  8,  12,  20);
    const hagl_color_t panel        = uiColor(backend, 18,  28,  46);
    const hagl_color_t border       = uiColor(backend, 235, 194,  77);
    const hagl_color_t selected     = uiColor(backend,  52,  92, 158);
    const hagl_color_t normalText   = uiColor(backend, 235, 239, 244);
    const hagl_color_t selectedText = uiColor(backend, 255, 255, 255);

    static constexpr int panelX     = 16;
    static constexpr int panelY     = 18;
    static constexpr int panelW     = 252;
    static constexpr int panelH     = 100;
    static constexpr int itemX      = panelX + 12;
    static constexpr int itemW      = panelW - 24;
    static constexpr int titleY     = panelY + 12;
    static constexpr int firstItemY = panelY + 64;
    static constexpr int itemStep   = 27;

    hagl_fill_rectangle_xywh(backend, 0, 0, kDisplayWidth, kDisplayHeight, backdrop);
    hagl_fill_rectangle_xywh(backend, panelX, panelY, panelW, panelH, panel);
    hagl_fill_rectangle_xywh(backend, panelX,          panelY,          panelW, 2,      border);
    hagl_fill_rectangle_xywh(backend, panelX,          panelY+panelH-2, panelW, 2,      border);
    hagl_fill_rectangle_xywh(backend, panelX,          panelY,          2,      panelH, border);
    hagl_fill_rectangle_xywh(backend, panelX+panelW-2, panelY,          2,      panelH, border);

    drawMenuText(backend, L"Doom Menu", panelX+12, titleY, border);

    for (int idx = 0; idx < static_cast<int>(MenuItem::Count); ++idx) {
        const MenuItem item = static_cast<MenuItem>(idx);
        const int rowY = firstItemY + idx * itemStep;
        const bool sel = (item == g_osd.menuSelection);
        if (sel) {
            hagl_fill_rectangle_xywh(backend, itemX-4, rowY-4, itemW, 22, selected);
        }
        wchar_t buf[48] = {};
        switch (item) {
        case MenuItem::ReturnToRadio:
            std::swprintf(buf, 48, L"Return to Radio"); break;
        case MenuItem::Count:
            break;
        }
        drawMenuText(backend, buf, itemX, rowY, sel ? selectedText : normalText);
    }
}

extern "C" void I_SetPalette(int pal)
{
    uint16_t *palette = (uint16_t *)V_BuildPalette(pal, 16);
    if (palette) {
        for (int i = 0; i < 256; i++) {
            // V_BuildPalette returns native little-endian RGB565, but the MIPI
            // panel (like hagl's rgb565()) expects big-endian, so byteswap.
            const uint16_t c = palette[i];
            doom_palette[i] = (uint16_t)((c << 8) | (c >> 8));
        }
        Z_Free(palette);
    }
    current_palette = pal;
}

extern "C" void I_FinishUpdate(void)
{
    hagl_backend_t *backend = fb.getBuffer();
    if (!backend || !backend->buffer || !doom_fb) {
        return;
    }

    uint16_t *dst = reinterpret_cast<uint16_t *>(backend->buffer);
    // doom_fb is the full 320x200 engine canvas; the panel only has
    // DISPLAY_HEIGHT (170) rows, so skip kCropRows rows off the top. Widths
    // match exactly (both 320), so cropping is just an offset + shorter
    // count - no per-row stride math needed - and taking only
    // DISPLAY_HEIGHT rows from there automatically drops the same number of
    // rows off the bottom too.
    const uint8_t *src = doom_fb + (size_t)kCropRows * SCREENWIDTH;
    const int pixels = DISPLAY_WIDTH * DISPLAY_HEIGHT;
    for (int i = 0; i < pixels; i++) {
        dst[i] = doom_palette[src[i]];
    }
    renderMenuOverlay(backend);
    hagl_flush(backend);
}

extern "C" void I_InitGraphics(void)
{
    for (int i = 0; i < 3; i++) {
        screens[i].width = SCREENWIDTH;
        screens[i].height = SCREENHEIGHT;
        screens[i].byte_pitch = SCREENWIDTH;
    }

    // Main screen renders into our internal-RAM 8-bit buffer for speed.
    screens[0].data = doom_fb;
    screens[0].not_on_heap = true;

    // Status bar working surface.
    screens[4].width = SCREENWIDTH;
    screens[4].height = (ST_SCALED_HEIGHT + 1);
    screens[4].byte_pitch = SCREENWIDTH;
}

extern "C" void I_UpdateVideoMode(void) {}
extern "C" void I_ShutdownGraphics(void) {}

// ──────────────────────────────────────────────────────────────────────────────
// System (i_system.h, i_main.h)
// ──────────────────────────────────────────────────────────────────────────────

int I_GetTimeMS(void)
{
    return (int)(esp_timer_get_time() / 1000);
}

int I_GetTime(void)
{
    return I_GetTimeMS() * TICRATE * realtic_clock_rate / 100000;
}

void I_uSleep(unsigned long usecs)
{
    // Round up to at least one tick so we always yield to the scheduler.
    uint32_t ms = (usecs + 999) / 1000;
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

const char *I_DoomExeDir(void)
{
    return g_doomDir;
}

const char *I_SigString(char *buf, size_t sz, int signum)
{
    snprintf(buf, sz, "signal %d", signum);
    return buf;
}

void I_Init(void)
{
    snd_channels = NUM_MIX_CHANNELS;
    snd_samplerate = AUDIO_SAMPLE_RATE;
    // snd_MusicVolume/snd_SfxVolume are NOT set here: M_LoadDefaults() (called
    // earlier in D_DoomMain, before I_Init()) already restored them from
    // littlefs/prboom.cfg - or left them at the defaults[] table's default of
    // 8 if there's no saved config yet. Hardcoding them here would silently
    // overwrite whatever was just loaded on every single boot.
    usegamma = 0;
    realtic_clock_rate = 100;
}

void I_SafeExit(int rc)
{
    (void)rc;
    ESP_LOGI(TAG, "I_SafeExit(%d) called, mainTaskHandle=%p", rc, (void *)g_mainTaskHandle);

    // Persist Doom's own engine-level settings (key bindings, detail level,
    // in-engine sound mixer levels, etc. - anything reachable through the
    // engine's own Options menu) to littlefs/prboom.cfg. M_LoadDefaults() is
    // already called at startup (d_main.c); this is the missing write side -
    // the vendored engine never calls M_SaveDefaults() on its own. Our own
    // OSD volume/mute (outside the engine, see saveSettings() above) is saved
    // separately, on every change rather than only at exit.
    M_SaveDefaults();

    audioPlayer.setMute(true);

    // We have decided to leave Doom, so we no longer need this task. Rather than
    // erase the boot-selection (otadata) flash region from here — this task may
    // run with its stack in PSRAM, which is inaccessible while flash caches are
    // disabled during the erase — hand the work to the main task. It suspends us
    // first, then performs the erase/restart from its internal-RAM stack.
    if (g_mainTaskHandle) {
        xTaskNotifyGive(g_mainTaskHandle);
        for (;;) {
            vTaskDelay(portMAX_DELAY); // main task suspends, then restarts us out of existence
        }
    }

    // Fallback (main task never started): do it inline.
    return_to_karadio();
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Input (i_video.h: I_StartTic / i_joy.h)
// ──────────────────────────────────────────────────────────────────────────────

void I_StartTic(void)
{
    static uint32_t prev_joystick = 0;
    static uint32_t prev_raw = 0;
    uint32_t raw = btJoystick.getButtonMask();
    uint32_t joystick = normalizeDpadMask(raw);

    // Emergency exit: Plus + Minus held together returns to the launcher.
    // L2, Plus, and Minus all decode from adjacent bits of the same raw HID
    // byte (see androidBitButton() in BluetoothJoystick.hpp: bit0=L2, bit1=R2,
    // bit2=Minus, bit3=Plus). On some controllers a single L2 press (e.g. an
    // analog trigger, or a report that sets several of those bits at once)
    // makes Plus+Minus read as pressed too, firing this combo instead of
    // opening the OSD menu - excluding L2 here prevents that misfire without
    // weakening the genuine Plus+Minus shortcut.
    if (isPressed(raw, Btn::BTN_PLUS) && isPressed(raw, Btn::BTN_MINUS) && !isPressed(raw, Btn::BTN_L2)) {
        I_SafeExit(0);
        return;
    }

    // Rising edges (newly pressed this tic).
    const uint32_t rawPressed = raw & ~prev_raw;

    // ── EraTV overlay menu (opened with L2) ──────────────────────────────────
    bool openedThisFrame = false;
    if (!g_osd.menuVisible && isPressed(rawPressed, Btn::BTN_L2)) {
        setMenuVisible(true);
        openedThisFrame = true;
        // Release any game keys still held so the player stops moving.
        if (prev_joystick) {
            for (size_t i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
                const uint32_t bit = 1u << static_cast<uint8_t>(keymap[i].button);
                if (prev_joystick & bit) {
                    event_t event = {};
                    event.type = ev_keyup;
                    event.data1 = *keymap[i].key;
                    D_PostEvent(&event);
                }
            }
        }
        prev_joystick = 0;
    }

    if (g_osd.menuVisible) {
        // Only one item (ReturnToRadio) - no up/down navigation needed.
        if (isPressed(rawPressed, Btn::BTN_A)) {
            activateMenuSelection();
        }
        if (isPressed(rawPressed, Btn::BTN_B) ||
            (!openedThisFrame && isPressed(rawPressed, Btn::BTN_L2))) {
            setMenuVisible(false);
        }
        // Keep the game-input baseline in sync with what is currently held while
        // the menu is up. Without this, any button still pressed when the menu
        // closes (e.g. A/B used to confirm, or a held D-pad) would look like a
        // fresh key-down to Doom on the next frame and leak into gameplay.
        prev_joystick = joystick;
        prev_raw = raw;
        return;
    }

    uint32_t changed = prev_joystick ^ joystick;
    if (changed) {
        for (size_t i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
            const uint32_t bit = 1u << static_cast<uint8_t>(keymap[i].button);
            if (changed & bit) {
                event_t event = {};
                event.type = (joystick & bit) ? ev_keydown : ev_keyup;
                event.data1 = *keymap[i].key;
                D_PostEvent(&event);
            }
        }
    }

    prev_joystick = joystick;
    prev_raw = raw;
}

extern "C" void I_InitJoystick(void) {}
extern "C" void I_PollJoystick(void) {}

// ──────────────────────────────────────────────────────────────────────────────
// Sound (i_sound.h)
// ──────────────────────────────────────────────────────────────────────────────

void I_UpdateSoundParams(int handle, int volume, int separation, int pitch)
{
    (void)handle; (void)volume; (void)separation; (void)pitch;
}

int I_StartSound(int sfxid, int channel, int vol, int sep, int pitch, int priority)
{
    (void)channel; (void)vol; (void)sep; (void)pitch; (void)priority;
    int oldest = gametic;
    int slot = 0;

    if (sfxid < 0 || sfxid >= NUMSFX || !sfx[sfxid]) {
        return -1;
    }

    // Single-instance sounds: stop any currently-running copy first.
    if (sfxid == sfx_sawup || sfxid == sfx_sawidl || sfxid == sfx_sawful
        || sfxid == sfx_sawhit || sfxid == sfx_stnmov || sfxid == sfx_pistol) {
        for (int i = 0; i < NUM_MIX_CHANNELS; i++) {
            if (channels[i].sfx == sfx[sfxid]) {
                channels[i].sfx = NULL;
            }
        }
    }

    // Find a free channel or steal the oldest.
    for (int i = 0; i < NUM_MIX_CHANNELS; i++) {
        if (channels[i].sfx == NULL) {
            slot = i;
            break;
        } else if (channels[i].starttic < oldest) {
            slot = i;
            oldest = channels[i].starttic;
        }
    }

    channel_t *chan = &channels[slot];
    chan->sfx = sfx[sfxid];
    chan->factor = (float)chan->sfx->samplerate / snd_samplerate;
    chan->pos = 0;
    chan->starttic = gametic;

    return slot;
}

void I_StopSound(int handle)
{
    if (handle >= 0 && handle < NUM_MIX_CHANNELS) {
        channels[handle].sfx = NULL;
    }
}

bool I_SoundIsPlaying(int handle)
{
    (void)handle;
    return false;
}

bool I_AnySoundStillPlaying(void)
{
    for (int i = 0; i < NUM_MIX_CHANNELS; i++) {
        if (channels[i].sfx) {
            return true;
        }
    }
    return false;
}

int I_GetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    char namebuf[16];
    snprintf(namebuf, sizeof(namebuf), "ds%s", sfxinfo->name);
    return W_GetNumForName(namebuf);
}

void I_SetChannels(void) {}

static void soundTask(void *arg)
{
    (void)arg;
    for (;;) {
        bool haveMusic = snd_MusicVolume > 0 && musicPlaying;
        bool haveSFX = snd_SfxVolume > 0 && I_AnySoundStillPlaying();

        if (haveMusic) {
            music_player->render(musicMix, AUDIO_BUFFER_LENGTH);
        }

        if (haveMusic || haveSFX) {
            for (int n = 0; n < AUDIO_BUFFER_LENGTH; n++) {
                int totalSample = 0;

                if (haveSFX) {
                    int sfxSample = 0;
                    for (int i = 0; i < NUM_MIX_CHANNELS; i++) {
                        channel_t *chan = &channels[i];
                        if (!chan->sfx) {
                            continue;
                        }
                        size_t pos = (size_t)(chan->pos++ * chan->factor);
                        if (pos >= chan->sfx->length) {
                            chan->sfx = NULL;
                        } else {
                            // 8-bit unsigned PCM centred at 127.
                            sfxSample += chan->sfx->samples[pos] - 127;
                        }
                    }
                    // Fixed gain, independent of how many channels are active, so
                    // the music level never changes when SFX start/stop.
                    sfxSample <<= 7;
                    sfxSample /= (16 - snd_SfxVolume);
                    totalSample += sfxSample;
                }

                if (haveMusic) {
                    totalSample += musicMix[n * 2]; // take left channel of stereo render
                }

                // Additive mix: clip rather than average (averaging caused the
                // music/SFX to duck whenever another source was playing).
                if (totalSample > 32767) {
                    totalSample = 32767;
                } else if (totalSample < -32768) {
                    totalSample = -32768;
                }
                monoOut[n] = (int16_t)totalSample;
            }
        } else {
            memset(monoOut, 0, sizeof(monoOut));
        }

        // Blocks on the I2S DMA, providing natural pacing at the sample rate.
        audioPlayer.writePcm16Mono(monoOut, AUDIO_BUFFER_LENGTH);
    }
}

void I_InitSound(void)
{
    for (int i = 1; i < NUMSFX; i++) {
        if (S_sfx[i].lumpnum != -1) {
            sfx[i] = (const doom_sfx_t *)W_CacheLumpNum(S_sfx[i].lumpnum);
        }
    }

    music_player->init(snd_samplerate);
    music_player->setvolume(snd_MusicVolume);

    xTaskCreatePinnedToCore(soundTask, "doom_sound", 4096, NULL, 5, NULL, 0);
}

void I_ShutdownSound(void)
{
    music_player->shutdown();
}

// ── Music (i_sound.h) ─────────────────────────────────────────────────────────

void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_UpdateMusic(void) {}

void I_PlaySong(int handle, int looping)
{
    music_player->play((void *)handle, looping);
    musicPlaying = true;
}

void I_PauseSong(int handle)
{
    (void)handle;
    music_player->pause();
    musicPlaying = false;
}

void I_ResumeSong(int handle)
{
    (void)handle;
    music_player->resume();
    musicPlaying = true;
}

void I_StopSong(int handle)
{
    (void)handle;
    music_player->stop();
    musicPlaying = false;
}

void I_UnRegisterSong(int handle)
{
    music_player->unregistersong((void *)handle);
}

int I_RegisterSong(const void *data, size_t len)
{
    uint8_t *mid = NULL;
    size_t midlen = 0;
    int handle = 0;

    if (mus2mid((const byte *)data, len, &mid, &midlen, 64) == 0) {
        handle = (int)music_player->registersong(mid, midlen);
    } else {
        handle = (int)music_player->registersong(data, len);
    }

    free(mid);
    return handle;
}

void I_SetMusicVolume(int volume)
{
    music_player->setvolume(volume);
}

// ──────────────────────────────────────────────────────────────────────────────
// Networking (i_network.h) — disabled, minimal stubs so the engine links.
// ──────────────────────────────────────────────────────────────────────────────

extern "C" void I_InitNetwork(void) {}

// ──────────────────────────────────────────────────────────────────────────────
// Host setup / WAD discovery
// ──────────────────────────────────────────────────────────────────────────────


static bool isIwadFile(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    char header[4] = {0};
    size_t got = fread(header, 1, sizeof(header), fp);
    fclose(fp);
    if (got < 4) {
        return false;
    }
    // "ZWAD" is a compressed IWAD/PWAD (see
    // ../zwad-compressed-wad-porting.md) - same directory-role as a plain
    // IWAD from this scan's point of view, W_AddFile()/CheckIWAD() in
    // components/prboom tell them apart from here on.
    return (header[0] == 'I' && header[1] == 'W' && header[2] == 'A' && header[3] == 'D')
        || (header[0] == 'Z' && header[1] == 'W' && header[2] == 'A' && header[3] == 'D');
}

static bool hasWadExtension(const char *name)
{
    if (!name) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".wad") == 0 || strcasecmp(dot, ".zwad") == 0);
}

// Scan a directory for the first IWAD (preferred) or any .wad as a fallback.
static bool findIwadInDir(const char *dir, char *out, size_t outSize)
{
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }

    char firstWad[320] = {0};
    char iwad[320] = {0};
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_type != DT_REG || !hasWadExtension(ent->d_name)) {
            continue;
        }
        char full[320];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (firstWad[0] == '\0') {
            strncpy(firstWad, full, sizeof(firstWad) - 1);
        }
        if (isIwadFile(full)) {
            strncpy(iwad, full, sizeof(iwad) - 1);
            break;
        }
    }
    closedir(d);

    const char *chosen = iwad[0] ? iwad : (firstWad[0] ? firstWad : nullptr);
    if (!chosen) {
        return false;
    }
    strncpy(out, chosen, outSize - 1);
    out[outSize - 1] = '\0';
    return true;
}

static void dirOf(const char *path, char *out, size_t outSize)
{
    strncpy(out, path, outSize - 1);
    out[outSize - 1] = '\0';
    char *slash = strrchr(out, '/');
    if (slash && slash != out) {
        *slash = '\0';
    }
}

// ── Startup WAD selection ────────────────────────────────────────────────────
// If littlefs holds more than one IWAD (e.g. both a compressed Doom and Doom
// II - see ../zwad-compressed-wad-porting.md), findIwadInDir() alone would
// silently boot whichever one readdir() happens to return first and leave
// the other unreachable. Scan for all of them and, when there's a choice to
// make, ask before starting the engine.

static constexpr int kMaxWadCandidates = 6;

struct WadCandidate {
    char path[320];
    char label[64]; // filename only, for display
};

// Like findIwadInDir(), but collects every IWAD/ZWAD it finds (up to
// kMaxWadCandidates) instead of stopping at the first one. PWAD-only files
// are skipped here - they need a companion IWAD and aren't a bootable
// choice on their own, so they'd make confusing entries in a "pick a game"
// list.
static int listIwadsInDir(const char *dir, WadCandidate *out, int maxCount)
{
    DIR *d = opendir(dir);
    if (!d) {
        return 0;
    }

    int count = 0;
    int skipped = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_type != DT_REG || !hasWadExtension(ent->d_name)) {
            continue;
        }
        char full[320];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (!isIwadFile(full)) {
            continue;
        }
        if (count >= maxCount) {
            skipped++;
            continue;
        }
        strncpy(out[count].path, full, sizeof(out[count].path) - 1);
        out[count].path[sizeof(out[count].path) - 1] = '\0';
        strncpy(out[count].label, ent->d_name, sizeof(out[count].label) - 1);
        out[count].label[sizeof(out[count].label) - 1] = '\0';
        count++;
    }
    closedir(d);

    if (skipped > 0) {
        ESP_LOGW(TAG, "listIwadsInDir: %s has more than %d IWADs, %d not shown",
                 dir, maxCount, skipped);
    }
    return count;
}

static void narrowToWide(wchar_t *dst, size_t dstSize, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < dstSize; i++) {
        dst[i] = static_cast<wchar_t>(static_cast<unsigned char>(src[i]));
    }
    dst[i] = L'\0';
}

// Blocks until the user picks one of `count` candidates (DPAD up/down to
// move, A to confirm), rendering an OSD-styled panel each frame. Runs
// before D_DoomMain() on the Doom task's own stack, using the same
// FrameBuffer/BluetoothJoystick objects I_FinishUpdate()/I_StartTic() use
// once the engine is running - both are already begin()'d by app_main() by
// the time doomTask() starts.
static int selectIwad(const WadCandidate *candidates, int count)
{
    hagl_backend_t *backend = fb.getBuffer();
    if (!backend) {
        return 0; // no display to choose with - just take the first one
    }

    ESP_LOGI(TAG, "%d IWADs found in %s - showing selection menu", count, g_doomDir);

    const hagl_color_t backdrop     = uiColor(backend,  8,  12,  20);
    const hagl_color_t panel        = uiColor(backend, 18,  28,  46);
    const hagl_color_t border       = uiColor(backend, 235, 194,  77);
    const hagl_color_t selectedBg   = uiColor(backend,  52,  92, 158);
    const hagl_color_t normalText   = uiColor(backend, 235, 239, 244);
    const hagl_color_t selectedText = uiColor(backend, 255, 255, 255);

    static constexpr int panelX     = 16;
    static constexpr int panelY     = 6;
    static constexpr int panelW     = kDisplayWidth - 2 * panelX;
    static constexpr int itemX      = panelX + 12;
    static constexpr int itemW      = panelW - 24;
    static constexpr int titleY     = panelY + 10;
    static constexpr int firstItemY = panelY + 32;
    static constexpr int itemStep   = 16;
    const int panelH = std::min(kDisplayHeight - 2 * panelY, 32 + count * itemStep + 8);

    int selection = 0;
    uint32_t prevRaw = 0;

    for (;;) {
        const uint32_t raw = btJoystick.getButtonMask();
        const uint32_t rawPressed = raw & ~prevRaw;
        const uint32_t joyPressed = normalizeDpadMask(raw) & ~normalizeDpadMask(prevRaw);
        prevRaw = raw;

        if (isPressed(joyPressed, Btn::DPAD_UP))   selection = (selection - 1 + count) % count;
        if (isPressed(joyPressed, Btn::DPAD_DOWN)) selection = (selection + 1) % count;
        if (isPressed(rawPressed, Btn::BTN_A)) {
            break;
        }

        hagl_fill_rectangle_xywh(backend, 0, 0, kDisplayWidth, kDisplayHeight, backdrop);
        hagl_fill_rectangle_xywh(backend, panelX, panelY, panelW, panelH, panel);
        hagl_fill_rectangle_xywh(backend, panelX,            panelY,            panelW, 2,      border);
        hagl_fill_rectangle_xywh(backend, panelX,            panelY + panelH-2, panelW, 2,      border);
        hagl_fill_rectangle_xywh(backend, panelX,             panelY,           2,      panelH, border);
        hagl_fill_rectangle_xywh(backend, panelX + panelW-2,  panelY,           2,      panelH, border);

        drawMenuText(backend, L"Select WAD", panelX + 12, titleY, border, 1);

        for (int i = 0; i < count; i++) {
            const int rowY = firstItemY + i * itemStep;
            const bool sel = (i == selection);
            if (sel) {
                hagl_fill_rectangle_xywh(backend, itemX - 4, rowY - 3, itemW, 14, selectedBg);
            }
            wchar_t buf[64];
            narrowToWide(buf, 64, candidates[i].label);
            drawMenuText(backend, buf, itemX, rowY, sel ? selectedText : normalText, 1);
        }

        hagl_flush(backend);
        vTaskDelay(pdMS_TO_TICKS(33));
    }

    ESP_LOGI(TAG, "Selected WAD: %s", candidates[selection].path);
    return selection;
}

// PrBoom entry — runs the whole game; never returns.
static void doomTask(void *arg)
{
    static char romPath[320];
    strncpy(romPath, static_cast<const char *>(arg), sizeof(romPath) - 1);
    romPath[sizeof(romPath) - 1] = '\0';

    const char *iwad = nullptr;
    const char *pwad = nullptr;
    static char iwadBuf[320];

    if (romPath[0] != '\0' && isIwadFile(romPath)) {
        iwad = romPath;
        dirOf(romPath, g_doomDir, sizeof(g_doomDir));
    } else if (romPath[0] != '\0') {
        // Selected file is a PWAD: still need an IWAD from the same directory.
        pwad = romPath;
        char dir[320];
        dirOf(romPath, dir, sizeof(dir));
        if (findIwadInDir(dir, iwadBuf, sizeof(iwadBuf))) {
            iwad = iwadBuf;
            strncpy(g_doomDir, dir, sizeof(g_doomDir) - 1);
        }
    }

    if (!iwad) {
        // No usable selection — scan the littlefs mount for every IWAD it
        // holds. More than one (e.g. a compressed Doom and Doom II side by
        // side) means there's a real choice to make, so ask instead of
        // silently booting whatever readdir() happens to return first.
        static WadCandidate candidates[kMaxWadCandidates];
        int n = listIwadsInDir(EraTV::kLittleFsMountPoint, candidates, kMaxWadCandidates);
        if (n > 1) {
            strncpy(g_doomDir, EraTV::kLittleFsMountPoint, sizeof(g_doomDir) - 1);
            int choice = selectIwad(candidates, n);
            iwad = candidates[choice].path;
        } else if (n == 1) {
            iwad = candidates[0].path;
            strncpy(g_doomDir, EraTV::kLittleFsMountPoint, sizeof(g_doomDir) - 1);
        } else if (findIwadInDir(EraTV::kLittleFsMountPoint, iwadBuf, sizeof(iwadBuf))) {
            // No genuine IWAD found (listIwadsInDir only counts those) but
            // there might still be a lone PWAD sitting there - keep the old
            // any-.wad fallback for that edge case.
            iwad = iwadBuf;
            strncpy(g_doomDir, EraTV::kLittleFsMountPoint, sizeof(g_doomDir) - 1);
        }
    }

    if (!iwad) {
        // No WAD flashed yet is a normal, recoverable first-boot state (see
        // FileSystem::begin()) - report it and let I_SafeExit() return to
        // KaRadio rather than hanging or rebooting in a loop.
        ESP_LOGE(TAG, "No Doom IWAD found in %s - run flash_wads.sh to add one", EraTV::kLittleFsMountPoint);
        I_SafeExit(1); // never returns
        return;
    }

    ESP_LOGI(TAG, "IWAD: %s%s%s", iwad, pwad ? "  PWAD: " : "", pwad ? pwad : "");

    // Saves live alongside the WADs.
    static char saveDir[80];
    snprintf(saveDir, sizeof(saveDir), "%s/saves", g_doomDir);
    mkdir(saveDir, 0777);

    static const char *doom_argv[10];
    int argc = 0;
    doom_argv[argc++] = "doom";
    doom_argv[argc++] = "-save";
    doom_argv[argc++] = saveDir;
    doom_argv[argc++] = "-iwad";
    doom_argv[argc++] = iwad;
    if (pwad) {
        doom_argv[argc++] = "-file";
        doom_argv[argc++] = pwad;
    }
    doom_argv[argc] = nullptr;

    myargc = argc;
    myargv = doom_argv;

    // Render at the engine's native 320x200, not the panel's 320x170 - see
    // the kCropRows comment above for why. I_FinishUpdate does the cropping.
    SCREENWIDTH = kEngineWidth;
    SCREENHEIGHT = kEngineHeight;

    // Allocate the 8-bit render target in PSRAM. It is CPU-converted to the
    // RGB565 display buffer in I_FinishUpdate (never DMA'd), so external RAM is
    // fine and keeps internal RAM free for the display framebuffer.
    doom_fb = static_cast<uint8_t *>(
        heap_caps_malloc(SCREENWIDTH * SCREENHEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!doom_fb) {
        ESP_LOGE(TAG, "Failed to allocate Doom framebuffer");
        I_SafeExit(1); // never returns
        return;
    }

    // Prefer PSRAM for the (large) Doom zone heap and most allocations.
    heap_caps_malloc_extmem_enable(0);

    Z_Init();
    D_DoomMain(); // never returns
    vTaskDelete(nullptr);
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Doom Emulator App starting (SDK %s)", esp_get_idf_version());

    // nvs_flash_init() is required by the Bluedroid BT stack (BluetoothJoystick);
    // the old app_switcher_init() call also did this as a side effect of its
    // NVS-based multi-app scheme, which is gone now that there are only two
    // apps, switched directly via boot-partition selection.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    // No launcher-provided ROM path in this single-WAD setup - doomTask()
    // falls back to scanning the littlefs mount when this is empty.
    static char romPath[320] = {};

    // Allocate the Doom task stack from INTERNAL RAM, and do it now, before
    // the framebuffer/littlefs/BT init below eat into that pool, to give the
    // allocator the best shot at a contiguous 32KB block.
    //
    // This *must* be internal RAM, not PSRAM: doomTask() reads WADs from the
    // littlefs partition, and every such read goes through esp_flash_read(),
    // which disables the flash cache for the duration of the SPI transaction
    // (see spi_flash_disable_interrupts_caches_and_other_cpu()). PSRAM access
    // depends on that same cache/MMU, so a task whose own stack lives in
    // PSRAM cannot survive executing through a flash read - ESP-IDF's
    // esp_task_stack_is_sane_cache_disabled() assert catches this and aborts
    // rather than silently corrupting memory. (This is why the original
    // SD-card-based EraTV version never hit this: SD reads go through a
    // different SPI host, not the flash-cache-disable path. It only became a
    // hazard once WADs moved onto the on-chip littlefs partition - and it
    // would have fired on every WAD/level read during gameplay, not just
    // this first directory scan.) The exit-time esp_ota_set_boot_partition()
    // otadata write is a similarly flash-cache-sensitive op, still handled by
    // suspending this task and handing off to the (always internal-RAM) main
    // task - see return_to_karadio()/g_mainTaskHandle - but that alone
    // wasn't sufficient once littlefs reads were in the picture too.
    constexpr uint32_t kDoomStackBytes = 32768;
    static StaticTask_t doomTaskTCB;
    StackType_t *doomStack = static_cast<StackType_t *>(
        heap_caps_malloc(kDoomStackBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!doomStack) {
        ESP_LOGE(TAG, "Failed to allocate Doom task stack");
        return_to_karadio();
        return;
    }

    fb.begin();
    if (!fb.getBuffer()) {
        ESP_LOGE(TAG, "Framebuffer init failed, returning to KaRadio");
        return_to_karadio();
        return;
    }
    hagl_clear(fb.getBuffer());
    hagl_flush(fb.getBuffer());

    fs.begin();
    fs.print_sdcard_recursive(EraTV::kLittleFsMountPoint);

    audioPlayer.begin();
    audioPlayer.configurePcmOutput(AUDIO_SAMPLE_RATE);
    loadSettings();

#if CONFIG_BT_ENABLED
    if (!btJoystick.begin()) {
        ESP_LOGW(TAG, "Bluetooth gamepad listener not active");
    }
#endif

    // Remember this (main) task so the Doom task can hand the return-to-launcher
    // flash sequence back to it. The main task runs on an internal-RAM stack.
    g_mainTaskHandle = xTaskGetCurrentTaskHandle();

    // Doom needs a large stack (recursive BSP traversal etc.). Run it on its own
    // task pinned to core 1; the audio task runs on core 0.
    g_doomTaskHandle = xTaskCreateStaticPinnedToCore(
        doomTask,
        "Doom",
        kDoomStackBytes / sizeof(StackType_t),
        romPath,
        4,
        doomStack,
        &doomTaskTCB,
        1);

    // Stay alive and wait for a "return to launcher" request from the Doom task.
    // When it arrives, suspend the Doom task (so it's not still rendering or
    // reading WADs) and then switch the boot partition / restart from here.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Return to KaRadio requested; stopping Doom and switching boot partition");
    if (g_doomTaskHandle) {
        vTaskSuspend(g_doomTaskHandle);
    }
    return_to_karadio(); // sets boot partition to ota_0 and calls esp_restart()
}
