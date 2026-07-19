# KaRadio32_4

An ESP32/ESP32-S3 fork of [karawin/Ka-Radio32](https://github.com/karawin/Ka-Radio32) — turns the
chip into a WiFi internet-radio player with an embedded web UI, telnet, and serial control. All
credit for the original design, protocol, and the bulk of this codebase goes to the upstream
project; see [What this fork changes](#what-this-fork-changes) below for what's different here.

## What this fork changes

- **Ports the build to ESP-IDF v5.4.2** (the upstream project targets older IDF releases). No
  local ESP-IDF patch is required for DAC output.
- **Adds LilyGO TTGO T-Display S3 support** — a dedicated 16 MB partition table, parallel/I80
  ST7789 panel driver, and PCM5102A I2S DAC audio output for a board with no built-in DAC and no
  SPI display bus. See [LilyGO TTGO T-Display S3](#lilygo-ttgo-t-display-s3).
- **Adds a Doom port as a second bootable app** on the T-Display S3's spare OTA partition, launched
  from KaRadio via a long GPIO0 press or `sys.launchapp`, with its own return path back to the
  radio. See [Doom on the T-Display S3](#doom-on-the-t-display-s3).

Everything else below — the web UI, telnet/serial command set, station list, hardware
configuration partition, supported audio outputs, and the huge list of supported displays — is
the original KaRadio32 feature set, largely unchanged.

## STATE
Release 2.4.0 (fork), targeting ESP-IDF v5.4.2.

Works on any esp32 board.
See the boards directory for a list of pre-configured boards.
See : [Hardware configuration partition](HardwareConfig.md)
- The esp32 adds the output on the internal dac or with i2s to an external dac but only mp3 stations can be played.
Adding a vs1053 board, all stations can be played.
On a wrover cpu with external psram, every AAC streams can be played without the vs1053 board.
- Compatible with esp8266 KaRadio addons.
- Serial or telnet commands : [Interfaces description](Interface.md)

### Android remote command
A new android application is born:

- KaRadio Remote Control by Vassilis Serasidis on google Play.

It is an easy and fast WiFi remote control for your KaRadio or KaRadio32 hardware.
With this android app you can select the WebRadio station you want to play, set the volume and get the station information such as Station name, Genre, Bit rate, Meta data and more.
Found it at
https://play.google.com/store/apps/details?id=com.serasidis.karadio.rc

Thanks Vassilis.

### The facebook group
https://www.facebook.com/groups/162949914181385
Just register and talk to other users.

## Added features from KaRadio
Work with i2s, internal DAC or a vs1053.
Output mode set in Setting panel on web page of KaraDio32 :

- I2S for connection to an external DAC
- I2SMERUS to connect a merus amplifier
- DAC to use the built in DAC of the esp32
- PDM to output a PDM (Pulse Density Modulation) stream
- VS1053 to connect to a vs1053 board, I2S output of the vs1053 enabled.
- all VS1053 tones control
- AAC decoding on I2S with a wrover cpu (4mB of external psram). Mp3 only on I2S with a wroom cpu.
- https stations are played.

### Others features :

- Interfaces available on serial, telnet and html
- up to 255 editable stations.
- Handles bitrates up to 320 kbps.
- Stations can be saved and restored from html in and from a file.
- Hardware configuration file to adapt the standard delivery to all boards and addons you need.
- all configurable parameters automatically saved.
- mDNS support.
- wrover esp32 support. About 10 seconds of buffering, and soon A2DP support (sink and source)
- Time Zone Offset support.
- Latin, Cyrillic and Greek support on screen.
- 28 types of lcd or oled  B/W or Color supported.
- Programmable lcd backlight off timer.
- Touch screen if the hardware supports it.
- Two joysticks (set of two adc buttons), one for volume, one for stations.
- Two max  rotary encoders. One for volume priority, one for station change priority.
- Two max set of 3 buttons. One for volume priority, one for station change priority.
- Rotary encoder and set of buttons support included. Common functions : play/stop, volume, station change, date time display.
- ADC keyboard with 6 buttons.
- Date format DD:MM:YYYY or MM:DD:YYYY .
- Remote IR support integrated. Nec protocol only.
- Two configurable access points .
- OTA (Over The Air) update of the software.
- many more configurable parameters. See [Interfaces document](Interface.md )


### Played stream
|   CPU   |  vs1053b      |  other |
| ------- | -------       | ------ |
| Wroom   | mp3 aac ogg   | mp3 |
| Wrover  | mp3 aac ogg   | mp3 aac  |

### Buffering audio
|   CPU        |  vs1053b      |  other |
| -------      | -------       | ------ |
| Wroom http   | 60k           | 45k    |
| Wroom https  | 50k           | 25k    |
| Wrover http  | 400k          | 400k   |
| Wrover https | 400k          | 400k   |



## Configure the hardware and IR codes
If the default configuration doesn't fit your needs, you can externally configure the software to fit your hardware and peripherals to suit your needs.
The configuration file is to be flashed only one time. After, the standard delivery will become compatible with your hardware gpio use and peripherals configuration. A future standard OTA will automatically works for your configuration.
See : [Hardware configuration partition](HardwareConfig.md)
For a wrover cpu you need a csv file with psram in the name. Without it, the default configuration will fail.

## Build your own

This project targets ESP-IDF v5.4.2. The default configuration uses a dual-OTA partition table for a 4 MB flash chip; boards with less flash need a custom partition layout.

```bash
source /home/user/esp/v5.4.2/esp-idf/export.sh
# Run this once after switching from an older ESP-IDF build directory.
idf.py fullclean
idf.py build
idf.py -p PORT flash
```

No local ESP-IDF patch is required for DAC output with v5.4.2.

The larger OTA image relocates the `hardware` NVS partition. Existing devices must be flashed over serial with the new partition table, then have their hardware configuration regenerated and flashed with the updated `boards` tooling before using OTA again.

## LilyGO TTGO T-Display S3

[`boards/ttgo_tdisplay_s3.csv`](boards/ttgo_tdisplay_s3.csv) targets the standard 1.9-inch T-Display S3: ESP32-S3, 16 MB flash, 8 MB OPI PSRAM, and a native 170×320 ST7789 I80 panel. KaRadio presents it as a 320×170 landscape display. It is not compatible with the original ESP32 T-Display, T-Display S3 AMOLED, or Pro variants.

Build in a separate directory so the ESP32-S3 settings cannot reuse an ESP32 build. `sdkconfig.defaults.esp32s3` is loaded automatically and selects the 16 MB partition table.

```bash
source /home/user/esp/v5.4.2/esp-idf/export.sh
IDF_TARGET=esp32s3 idf.py -B build-ttgo-t-display-s3 \
  -DSDKCONFIG=build-ttgo-t-display-s3/sdkconfig build
IDF_TARGET=esp32s3 idf.py -B build-ttgo-t-display-s3 -p PORT flash
```

The 16 MB partition table reserves 2 MB for KaRadio itself (`ota_0`), 2 MB for an optional
separate, independently-built application launchable from KaRadio via `sys.launchapp`
(`ota_1` — this is where the Doom port below lives), and dedicates the remaining ~11.8 MB to a
`littlefs` partition for that second app's own data. This reshuffles the offsets of the
`device`/`stations`/`device1`/`hardware` partitions relative to earlier releases — a board already
flashed with an older T-Display S3 partition table needs a full `esptool.py erase_flash` before
adopting this table, since none of its saved station list, device settings, or board GPIO config
carries forward automatically to the new offsets.

After flashing the firmware and its partition table, create and flash the board configuration at its S3-specific hardware-NVS offset:

```bash
cd boards
NVS_FLASH_OFFSET=0x422000 ESPTOOL_CHIP=esp32s3 \
  bash ./nvs_partition_generator.sh ttgo_tdisplay_s3.csv
esptool.py --chip esp32s3 --port PORT write_flash 0x422000 \
  build/ttgo_tdisplay_s3.bin
```

The display uses PWR/BL/RST/CS/DC/WR/RD GPIOs 15/38/5/6/7/8/9, plus data GPIOs 39/40/41/42/45/46/47/48. `O_LCD_ROTA=0` is the normal landscape direction; set it to `1` for the opposite direction. The two onboard buttons are active-low: GPIO0 is Button A and GPIO14 is Button B. GPIO0 is also the BOOT strap, so do not hold it while resetting or powering on.

The board has no audio output and ESP32-S3 has no built-in DAC. This profile enables a PCM5102A-compatible three-wire I2S DAC: connect `BCK` to GPIO21, `DIN` to GPIO17, and `LCK`/`LRCK` to GPIO18. Leave the DAC's `SCL`/`SCK` (MCLK) pin unconnected. `O_AUDIO=0` selects the normal I2S output. `O_STARTUP_BEEP=1` plays a 1 kHz tone for 0.5 seconds on boot to test the DAC path. GPIO17 and GPIO18 share the board's Qwiic I2C connector, so do not use that connector while this audio layout is active.

## Doom on the T-Display S3

The T-Display S3's spare `ota_1` partition (see above) can hold a second, independently-built
application instead of sitting empty. This fork ships one: a port of the
[PrBoom](https://prboom.sourceforge.net/) Doom source port, playable on the board's own screen,
buttons, and a Bluetooth gamepad. It lives entirely under [`doom/`](doom/) as its own ESP-IDF
project sharing KaRadio's partition table, and does not affect a plain ESP32 build at all.

### Building and flashing

`tools/build_all.sh` builds both apps (KaRadio for `ota_0`, Doom for `ota_1`) and checks their
sizes against their 2 MB partition budgets. `tools/flash_all.sh PORT` flashes KaRadio's full image
(bootloader + partition table + app — this establishes the partition table a fresh chip needs) and
then writes Doom's app binary directly to `ota_1`, without touching the shared partition table a
second time:

```bash
source /home/user/esp/v5.4.2/esp-idf/export.sh
bash tools/build_all.sh
bash tools/flash_all.sh PORT
```

Doom needs at least one WAD file to run. Put `.wad` file(s) — e.g. the shareware `DOOM1.WAD`, or a
retail `DOOM.WAD`/`DOOM2.WAD` you own — in a folder and flash them to the `littlefs` partition:

```bash
bash tools/flash_wads.sh PORT /path/to/wads/
```

This builds a LittleFS image sized to fit the WAD folder (not the whole partition — see the
comments in `tools/flash_wads.sh` for why: the image-building tool's own free-space accounting is
unreliable when built with extra headroom baked in) and lets the device grow it to the full
partition size itself on first mount. Re-run this script any time you want to change which WADs
are flashed; it replaces the whole `littlefs` partition contents.

### Entering and leaving Doom

- **Enter**: hold the board's BOOT button (GPIO0) for about a second, or send `sys.launchapp` over
  serial/telnet/HTTP. Either reboots the board into `ota_1`.
- **Leave**: open the in-game overlay menu (gamepad L2) and select **Return to Radio**, or hold
  Plus+Minus together on the gamepad. Both reboot back into KaRadio (`ota_0`).

### Controls (Bluetooth gamepad, Android-HID-style)

| Button | Action |
| ------ | ------ |
| D-pad | Move / turn |
| A | Fire / confirm in Doom's own menus |
| B | Use / confirm |
| X | Run (hold) |
| Y | Cycle weapons |
| L1 / R1 | Strafe left / right |
| R2 | Hold-to-strafe modifier |
| L2 | Open/close the overlay menu (Return to Radio) |
| Plus | Open Doom's own menu (Escape) |
| Minus | Automap |
| Home | Also opens Doom's own menu |
| Plus + Minus (held) | Emergency exit straight back to KaRadio |

Sound effect/music volume are adjusted from Doom's own in-game Sound menu (Options → Sound
Volume) and persist across reboots (`littlefs/prboom.cfg`), same as the rest of Doom's own
settings. The overall output level is a separate, fixed software gain stage for the board's
PCM5102A DAC (`doom/components/eratv_common/include/eratv/player/AudioPlayer.hpp`).

### Architecture notes

Doom's platform layer (video/audio/input/lifecycle) lives in `doom/main/main.cpp`; the engine
itself (`doom/components/prboom`) is vendored, largely unmodified PrBoom. Video goes through a
custom `hagl_hal` backend (`doom/components/hagl_hal`) driving the same parallel/I80 ST7789 panel
KaRadio itself uses, with a full-frame DMA buffer instead of KaRadio's line-at-a-time UI redraws.
Audio goes out over plain I2S to the board's PCM5102A DAC
(`doom/components/eratv_common/include/eratv/player/AudioPlayer.hpp`). Input comes from a BLE HID
gamepad (`doom/components/eratv_common/include/eratv/BluetoothJoystick.hpp`). WADs and all
persistent state (Doom's own config, this port's own volume/mute file, save games) live on the
shared `littlefs` partition, mounted at `/littlefs`.

## GPIO Definition
The default configuration is given below. It includes an encoder, an IR remote and a LCD or OLED.
To add or edit GPIO definitions and add or remove some devices, you may need a hardware configuration file. Some examples are in the boards directory.
See the file main/include/gpio.h and main/include/addon.h
WARNING: this configuration will not work for a wrover processor.
You need an adapted csv configuration.

```
//--------------------------------------//
// Define GPIO used in KaRadio32        //
// if no external configuration is used //
//--------------------------------------//
// Compatible ESP32 ADB
// https://www.tindie.com/products/microwavemont/esp32-audio-developing-board-esp32-adb/
// Default value, can be superseeded by the hardware partition.

// Must be HSPI or VSPI

#define KSPI VSPI_HOST

// KSPI pins of the SPI bus
//-------------------------
#define PIN_NUM_MISO GPIO_NUM_19 	// Master Input, Slave Output
#define PIN_NUM_MOSI GPIO_NUM_23	// Master Output, Slave Input   Named Data or SDA or D1 for oled
#define PIN_NUM_CLK  GPIO_NUM_18 	// Master clock  Named SCL or SCK or D0 for oled

// status led if any.
//-------------------
// Set the right one with command sys.led
// GPIO can be changed with command sys.ledgpio("x")
#define GPIO_LED	GPIO_NUM_4		// Flashing led or Playing led

// gpio of the vs1053
//-------------------
#define PIN_NUM_XCS  GPIO_NUM_32
#define PIN_NUM_RST  GPIO_NUM_12
#define PIN_NUM_XDCS GPIO_NUM_33
#define PIN_NUM_DREQ GPIO_NUM_34
// + KSPI pins

// Encoder knob
//-------------
#define PIN_ENC0_A   GPIO_NUM_16	//16	// 255 if encoder not used
#define PIN_ENC0_B   GPIO_NUM_17	//17	// DT
#define PIN_ENC0_BTN GPIO_NUM_5		//5// SW
#define PIN_ENC1_A   GPIO_NONE		// 255 if encoder not used
#define PIN_ENC1_B   GPIO_NONE		// DT
#define PIN_ENC1_BTN GPIO_NONE		// SW

// 3 Buttons
//-------------
#define PIN_BTN0_A   GPIO_NONE
#define PIN_BTN0_B   GPIO_NONE
#define PIN_BTN0_C   GPIO_NONE
#define PIN_BTN1_A   GPIO_NONE
#define PIN_BTN1_B   GPIO_NONE
#define PIN_BTN1_C 	 GPIO_NONE

// Joystick (2 buttons emulation on ADC)
//--------------------------------------
#define PIN_JOY_0	GPIO_NONE
#define PIN_JOY_1	GPIO_NONE

// I2C lcd (and rda5807 if lcd is i2c or LCD_NONE)
//------------------------------------------------
#define PIN_I2C_SCL GPIO_NUM_14
#define PIN_I2C_SDA GPIO_NUM_13
#define PIN_I2C_RST	GPIO_NUM_2		// or not used


// SPI lcd
//---------
#define PIN_LCD_CS	GPIO_NUM_13		//CS
#define PIN_LCD_A0	GPIO_NUM_14		//A0 or D/C
#define PIN_LCD_RST	GPIO_NUM_2		//Reset RES RST or not used
// KSPI pins +

// IR Signal
//-----------
#define PIN_IR_SIGNAL GPIO_NUM_21	// Remote IR source


// I2S DAC or PDM output
//-----------------------
#define PIN_I2S_LRCK GPIO_NUM_25	// or Channel1
#define PIN_I2S_BCLK GPIO_NUM_26	// or channel2
#define PIN_I2S_DATA GPIO_NUM_22	//

// ADC for keyboard buttons
#define PIN_ADC	GPIO_NONE	//GPIO_NUM_32 TO GPIO_NUM_39 or GPIO_NONE if not used.

// LCD backlight control
#define PIN_LCD_BACKLIGHT	GPIO_NONE // the gpio to be used in custom.c

// touch screen  T_DO is MISO, T_DIN is MOSI, T_CLK is CLk of the spi bus
#define PIN_TOUCH_CS	GPIO_NONE //Chip select T_CS

```
### LCD or oled declaration
You can configure the kind of display used in your configuration with the command
'sys.lcd("x")' with x:

```
#define LCD_NONE		255

// Black&White
//I2C
#define LCD_I2C_SH1106		0 //128X64
#define LCD_I2C_SSD1306		1 //128X64
#define LCD_I2C_SSD1309		2 //128X64
#define LCD_I2C_SSD1325 	3 //128X64
#define LCD_I2C_SSD1306NN	4 //128X64
#define LCD_I2C_SSD1309NN	5 //128X64
#define LCD_I2C_SSD1306UN	6 //128x32
#define LCD_I2C_ST7567		7 //64x32

//SPI
#define LCD_SPI_SSD1306 		64 //128X32 (LCD_SPI =0x40)
#define LCD_SPI_SSD1309 		65 //128X64
#define LCD_SPI_ST7565_ZOLEN	66 //128X64
#define LCD_SPI_SSD1322_NHD		67 //256X64
#define LCD_SPI_IL3820_V2		68 //296X128
#define LCD_SPI_SSD1607			69 //200X200
#define LCD_SPI_LS013B7DH03		70 //128X128
#define LCD_SPI_SSD1306NN 		71 //128X64
#define LCD_SPI_SSD1309NN 		72 //128X64
#define LCD_SPI_ST7920 			73 //128X64
#define LCD_SPI_ST7567_pi 		74 //132X64
#define LCD_SPI_ST7567	 		75 //64X32
#define LCD_SPI_ST7565_NHD_C12864	76 //128X64

// Colors
#define LCD_SPI_ST7735			192 // 128x160  (LCD_COLOR|LCD_SPI =0xC0)
#define LCD_SPI_SSD1351			193 // 128x128
#define LCD_SPI_ILI9341			194 // 240x320
#define LCD_SPI_ILI9163			195 // 128x128
#define LCD_SPI_PCF8833			196 // 132x132
#define LCD_SPI_SSD1331			197 // 96x64
#define LCD_SPI_SEPS225			198 // 96x64
#define LCD_SPI_ST7789V			199 // 240x320
#define LCD_SPI_ST7735S			200 // 128x128
#define LCD_SPI_ST7735L			201 // 80x160
#define LCD_SPI_ST7735W			202 // 128x160 shifted 2+1
#define LCD_SPI_ST7789S			203 // 240x240
#define LCD_SPI_ST7789T			204 // 135x240

```


## First use
- If the access point of your router is not known, the webradio initialize itself as an AP. Connect the wifi of your computer to the ssid "WifiKaRadio",
- Browse to 192.168.4.1 to display the page, got to "setting" "Wifi" and configure your ssid ap, the password if any, the wanted IP or use dhcp if you know how to retrieve the dhcp given ip (terminal or scan of the network).
- In the gateway field, enter the ip address of your router.
- Validate. The equipment restart to the new configuration. Connect your wifi to your AP and browse to the ip given in configuration.
- Congratulation, you can edit your own station list. Don't forget to save your stations list in case of problem or for new equipments.
- if the AP is already know by the esp32, the default ip is given by dhcp.
- a sample of stations list is on http://karadio.karawin.fr/WebStations.txt . Can be uploaded via the web page.

To flash your KaRadio32 without generating it, you will need the files in the binaries directory. Those prebuilt files have not been regenerated as part of the ESP-IDF v5.4.2 source migration; build from source using the instructions above for the migrated firmware.

The tool to use is here :
http://espressif.com/en/support/download/other-tools
(change the security of the installation directory to permit all)

See the image at :
http://karadio.karawin.fr/karawin32Flash.jpg
WARNING: the standard_adb.bin must be changed for a wrover processor or another configuration.

![Screenshoot of download tool](https://raw.githubusercontent.com/karawin/Ka-Radio32/master/images/downloadtool32.jpg)

### The scheme from tomasf71 :
Just an example of a realization.

![Scheme](https://raw.githubusercontent.com/karawin/Ka-Radio32/master/images/schemekaradio32.jpg)

## Audio output

In the Setting panel on the webpage of KaraDio32 you can set the desired output method for audio.

For output on a simple speaker or a general analog amplifier :

- DAC to use the built in DAC of the esp32
- PDM to output a PDM (Pulse Density Modulation) stream. Wiki on PDM : https://en.wikipedia.org/wiki/Pulse-density_modulation

For output via addional I2S or VS1053 hardware :

- I2S for connection to ac external DAC
- I2SMERUS to connect a merus amplifier
- VS1053 to connect to a vs1053 board.

### Connecting a speaker, earphone or amplifier with DAC or PDM setting
You can connect the GPIO25 (left audio channel), GPIO26 (right audio channel) and Ground directly to a small loudspeaker, a simple earphone (like from an mp3 player) or the input of an analog amplifier. The quality is less perfect than using an I2S or VS1053 board, bit it is very simple hardware-wise.

Connect like this :

```
                         +------------------------------------+
ESP32-GPIO25 (left) -----+speaker/earphone/analog-amp (left)   +-----+
                         +------------------------------------+      |
                                                                     |
                         +------------------------------------+      |
ESP32-GPIO26 (right) ----+speaker/earphone/analog-amp (right  +------+
                         +------------------------------------+      |
                                                                     |
ESP32-GROUND --------------------------------------------------------+
```

### Reducing the analog output level
If the audio signal level is too high for your speaker, earphone or amplifier you can add a potentiometer to decrease the level. Connect the potentiometer like this (only pictured for 1 audio channel, for stereo you need 2 potentiometers or a stereo-potmeter.

```
                                     +--------------------------------------+
ESP32-GPIO25 (left) -------+   +-----+speaker/earphone/analog-amp (left)    +------+
                          +-+  |     +--------------------------------------+      |
                          +P+  |                                                   |
                          +o+--+                                                   |
                          +t+                                                      |
                          +++                                                      |
                           |                                                       |
ESP32-GROUND --------------+-------------------------------------------------------+
```

### Improving the audio quality with a filter
An analog DAC signal always has some noise that may cause some distortion on the audio output, especially on low volume passages in the sound. This noise can be decreased with a low-pass filter. The digital PDM signal needs allways low pass filter to convert the digital signal to an analog signal. Fortunately the speaker and earphone acts as a low pass filter, although not in a perfect way.

You can improve the analog signal with an external low-pass filter. A simple low-pass RC filter can be find on the internet, f.i. here :
http://www.pavouk.org/hw/usbdac/en_index.html.

## Radio streams
If Karadio32 is configured to use ADC, PDM or I2S output (to an external dac) it can play audio streams in mp3 format. Other formats like AAC and WMV require too much dynamic memories and can not be played this way.

When you want to play other formats you add an external VS1053 decoder to Karadio. Details on the features of this decoder can be found here : http://www.vlsi.fi/en/products/vs1053.html

Many boards can be found at :
https://www.aliexpress.com/wholesale?catId=0&initiative_id=SB_20180113141636&SearchText=vs1053b

## Handling streaming errors
Normally a radio station is played without interruption.

But when a connection to the server of the radio station is (temporarily) disrupted (due to internet problems or server problems) Karadio behaves as follows:

- Playing of the radio stream stops, after a short while Karadio retries to relaunch the connection to the radio stream.
- If the relaunch of the connection fails again, the webpage of Karadio then shows “invalid address” and goes into the stop state. If you have a display connected to Karadio it will then show the Date, Time and IP address.
- You can try to relaunch the connection manually by pressing “play” on the Karadio webpage, or resetting the ESP32.
- If the wifi is disconnected, the esp is rebooted.

## List of sources and components adapted for KaRadio32

- The original project this is forked from : https://github.com/karawin/Ka-Radio32
- The Espressif IDF : https://github.com/espressif/esp-idf
- The Esp8266 KaRadio : https://hackaday.io/project/11570-wifi-webradio-with-esp8266-and-vs1053
- The KaRadio addons : https://github.com/karawin/karadio-addons
- Webradio for ESP-ADB module : https://github.com/kodera2t/ESP32_OLED_webradio
- Webradio with the MP3 decoder software : https://github.com/MrBuddyCasino/ESP32_MP3_Decoder
- Black/White oled lcd library : https://github.com/olikraus/u8g2
- Color LCD library : https://github.com/olikraus/ucglib
- u8g2 Hal for esp32 : https://github.com/nkolban/esp32-snippets/tree/master/hardware/displays/U8G2
- Encoder : https://github.com/soligen2010/encoder
- PrBoom (Doom source port used for the Doom-on-T-Display-S3 port) : https://prboom.sourceforge.net/
- HAGL graphics library (Doom port's display backend) : https://github.com/tuupola/hagl
- ESP LittleFS (Doom port's WAD/save storage) : https://github.com/joltwallet/esp_littlefs

![Encoder an Oled display](https://raw.githubusercontent.com/karawin/Ka-Radio32/master/images/2017-11-11%2009.50.50.jpg)
