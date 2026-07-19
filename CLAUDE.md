# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

KaRadio32 is an ESP-IDF (v5.4.2) firmware project for ESP32/ESP32-S3 that turns the chip into a
WiFi internet-radio player. It streams and decodes internet radio (mp3, and aac/ogg when a VS1053
board or PSRAM-equipped "wrover"/S3 chip is used), and drives an optional LCD/OLED, rotary
encoders, buttons, joystick, IR remote, and touch screen. Control surfaces are an embedded web UI
(`webpage/`), a telnet interface, and a serial/UART CLI — all backed by the same command parser.

## Build commands

Standard ESP32 board (4 MB flash, default `build/` directory):

```bash
source /home/user/esp/v5.4.2/esp-idf/export.sh
idf.py fullclean   # required once after switching from an older ESP-IDF build directory
idf.py build
idf.py -p PORT flash
```

LilyGO TTGO T-Display S3 (ESP32-S3, 16 MB flash) — must build in a separate directory so its
`sdkconfig.defaults.esp32s3` settings can't leak into an ESP32 build:

```bash
source /home/user/esp/v5.4.2/esp-idf/export.sh
IDF_TARGET=esp32s3 idf.py -B build-ttgo-t-display-s3 \
  -DSDKCONFIG=build-ttgo-t-display-s3/sdkconfig build
IDF_TARGET=esp32s3 idf.py -B build-ttgo-t-display-s3 -p PORT flash
```

There is no host-side unit test suite — this is embedded firmware; "testing" means building,
flashing to a device, and exercising it over serial/telnet/web/hardware. `idf.py build` (and the
compiler warnings it emits, since many are demoted to non-fatal — see main/CMakeLists.txt) is the
main correctness gate available without hardware.

Debugging:

```bash
./dbg.sh <COM port suffix>   # attaches xtensa-esp32-elf-gdb to ./build/KaRadio32.elf
```

### Regenerating the embedded web UI

`webpage/` (HTML/CSS/JS) is minified, gzipped, and turned into C byte arrays (`xxd -i`) that get
compiled into the firmware. After editing anything under `webpage/`, regenerate the generated
arrays before rebuilding firmware:

```bash
./generate.sh          # cd webpage && ./generate.sh, then `make app`
```

Do not hand-edit the generated array files in `webpage/` (`style`, `style1`, `script`, `index`,
`logo`, `favicon`) — they're build output of `webpage/generate.sh`.

### Hardware configuration (board bring-up)

Per-board GPIO/option layouts live as CSV files in `boards/` and are compiled into an NVS binary
that gets flashed once to the `hardware` partition (offset `0x3e2000` on the default 4 MB table,
`0x622000` on the T-Display S3's 16 MB table):

```bash
cd boards
bash ./nvs_partition_generator.sh yourboard.csv     # -> build/yourboard.bin
esptool.py --chip esp32 --port PORT write_flash 0x3e2000 build/yourboard.bin
```

See [HardwareConfig.md](HardwareConfig.md) for the full CSV field reference (`P_*` pins, `O_*`
options, `K_*` IR key codes) and [boards/README.md](boards/README.md) for which sample CSV matches
which board. The compiled-in defaults (used if no hardware partition is flashed) live in
`main/include/gpio.h` / `main/include/addon.h`, mirrored by `boards/standard_adb.csv`.

## Architecture

### Task layout (`main/app_main4.h`, `void app_main()`)

`main/app_main.c` is a two-line IDF-version shim that includes `app_main4.h` (ESP-IDF >= 4) or the
now-effectively-dead `app_main3.h`. All real startup logic is in `app_main4.h`. Boot sequence:
init NVS/hardware config partition → init SPI/I2C/audio renderer → start WiFi/network → init mDNS
→ init the audio player → spawn the FreeRTOS tasks below (priorities/core pins in
`main/include/app_main.h`, `PRIO_*`/`CPU_*`):

- `timerTask` — periodic housekeeping (LCD timers, NTP, etc.)
- `uartInterfaceTask` — serial CLI, shared command parser with telnet/web
- `clientTask` (`webclient.c`) — the HTTP client that connects to the internet radio stream and
  feeds the decoder
- `serversTask` (`servers.c`) — single task multiplexing the telnet server, websocket server, and
  HTTP webserver sockets via `select()`
- `task_addon` (`addon.c`) — polls encoders/buttons/joystick/IR and drives the LCD/OLED

### Control-plane fan-in

`interface.c` implements one command grammar (`sys.*`, `cli.*`, `wifi.*`) shared by three
transports: HTTP query params (`webserver.c`), telnet (`telnet.c`), and UART (`uartInterfaceTask`).
`websocket.c` pushes async now-playing/status updates to the open web UI. When adding a new
command, it typically needs to be wired into `interface.c` once and becomes available on all three
surfaces. [Interface.md](Interface.md) documents the full command set and response formats
(`##CLI.*#`, `##SYS.*#` tags).

### Audio path

`webclient.c` opens the HTTP(S)/ICY stream and pushes data into a FIFO (`components/fifo`)
consumed by `components/audio_player` → `components/audio_renderer`, which dispatches to a decoder
(`components/mp3_decoder` / `mad`, `components/fdk-aac_decoder` or `fdk-aac-oreo-m8` for AAC) and
an output sink selected at runtime (`audio_output_mode`: I2S, I2S+Merus amp
(`components/MerusAudio`), built-in DAC, PDM, or an external VS1053 board via `main/vs1053.c` +
`main/vs1053b-patches.c`). AAC/OGG decoding is only available with a PSRAM-equipped chip
("wrover"/S3) or a VS1053 board; plain ESP32 without PSRAM is mp3-only. `wolfssl` provides TLS for
`https://` streams.

### Display / input

`addon.c` is the polling loop for all physical input (encoders via `ClickEncoder.c`, buttons via
`ClickButtons.c`, joystick via `ClickJoystick.c`, IR via `irnec.c`) and dispatches to
`addonu8g2.c` (monochrome OLED/LCD, `components/u8g2`) or `addonucg.c` (color LCD,
`components/ucglib`) depending on the configured `lcd_type`. `components/xpt2046` handles
resistive touch input for touch-capable panels.

### Persistent config

`eeprom.c` reads/writes device settings, the station list, and the hardware-config partition via
NVS (`nvs_flash`), exposing them as the global `g_device` struct plus a separate stations store.
`main/CMakeLists.txt`'s `standard_adb.csv`-equivalent defaults are the fallback when no `hardware`
NVS partition has been flashed. `ota.c` handles OTA image updates (`sys.update`/`sys.prerelease`,
pinned to the `ota_0` slot) and booting into the secondary application (`sys.launchapp`, see below).

### Partition layout

The default 4 MB table (`partitions.csv`) has dual OTA app slots plus dedicated `device`,
`stations`, `device1`, and `hardware` NVS partitions — see [HardwareConfig.md](HardwareConfig.md)
for exact offsets.

The T-Display S3 uses its own 16 MB table (`partitions.ttgo_tdisplay_s3.csv`) with a different
layout: `ota_0` (2 MB) always holds KaRadio itself — `ota.c` pins OTA updates there explicitly
rather than picking "whichever OTA slot isn't running" — and `ota_1` (4 MB) holds a separate,
independently-built application that KaRadio can boot into via `sys.launchapp`
(`esp_ota_set_boot_partition()` + reboot; returning to KaRadio is that other app's own
responsibility). The remaining ~9.8 MB is a `littlefs`-formatted data partition reserved for that
second app's assets — KaRadio itself doesn't mount or use it. The `hardware` partition lives at
`0x622000` here, not `0xc22000`; don't assume the default table's offsets apply on this board.

## Repo layout notes

- `main/` — application source (all the `.c`/`.h` files above; `app_main3.h`/`app_main4.h` are
  headers by name only — they contain the actual `app_main()` implementations, split by IDF
  version).
- `components/` — vendored/adapted third-party libraries (decoders, display libs, TLS, audio
  pipeline) plus the KaRadio-specific `audio_player`/`audio_renderer`/`bt_speaker` glue.
- `managed_components/` — ESP-IDF component-manager-fetched dependencies (currently `espressif__mdns`); managed by `idf_component.yml` / `dependencies.lock`, don't hand-edit.
- `boards/` — per-board hardware-config CSVs and the NVS-image generator tooling.
- `webpage/` — source HTML/CSS/JS for the embedded web UI; only the source files are hand-edited,
  the generated arrays are checked in as build artifacts (see "Regenerating the embedded web UI").
- `build/`, `build-ttgo-t-display-s3/` — out-of-tree CMake/ninja build directories; safe to
  `idf.py fullclean` / delete and regenerate.
- `binaries/` — prebuilt release binaries for flashing without building from source (not
  regenerated automatically by CI; see README's note that these predate the ESP-IDF 5.4.2
  migration).
