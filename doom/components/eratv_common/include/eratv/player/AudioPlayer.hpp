/*
 * AudioPlayer.hpp - rewritten for this board's plain PCM5102A I2S DAC.
 *
 * The original targeted an ES8311 I2S codec controlled over I2C (hardware
 * volume/mute, power sequencing, etc). The PCM5102A is a dumb DAC with no
 * control bus at all (XSMT is tied high externally) - volume/mute here are
 * done in software on the PCM before writing it to I2S, matching what
 * KaRadio's own renderer_volume() does for the same reason. I2S pin/config
 * shape mirrors KaRadio32_4/components/audio_renderer/audio_renderer.c
 * exactly (I2S_NUM_0, GPIO21/18/17, mck_io_num left unset - NOT setting it
 * defaults to GPIO0 and silently claims that pin, a bug already hit and
 * fixed on the KaRadio side).
 *
 * Public interface intentionally matches the original class (begin(),
 * configurePcmOutput(), writePcm16Mono(), increase/decreaseVolume(),
 * setMute()/isMuted(), getVolume()) so doom/main/main.cpp needs no changes
 * beyond removing the ES8311/I2C-specific bits it never actually used.
 */
#ifndef MAIN_AUDIOPLAYER_HPP_
#define MAIN_AUDIOPLAYER_HPP_

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

#include "driver/i2s.h"
#include "esp_log.h"

namespace EraTV {

class AudioPlayer {
public:
    AudioPlayer() : muted(false), volumeLevel(5) {}

    void begin()
    {
        i2s_config_t i2s_config = {
            .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
            .sample_rate = 22050,
            .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = 0,
            .dma_buf_count = 6,
            .dma_buf_len = 256,
            .use_apll = false,
            .tx_desc_auto_clear = true,
        };
        i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);

        i2s_pin_config_t pin_config = {
            .mck_io_num = I2S_PIN_NO_CHANGE, // see file header comment - do not omit this field
            .bck_io_num = kPinBck,
            .ws_io_num = kPinWs,
            .data_out_num = kPinData,
            .data_in_num = I2S_PIN_NO_CHANGE,
        };
        i2s_set_pin(I2S_NUM_0, &pin_config);
    }

    // Reconfigures the sample rate; the channel/format stays fixed (stereo,
    // 16-bit) since writePcm16Mono() always duplicates mono input to L+R -
    // a plain I2S DAC/amp expects interleaved stereo frames, and true
    // single-channel I2S formats are not reliably supported across cheap
    // DAC boards.
    void configurePcmOutput(uint32_t sampleRate)
    {
        i2s_set_sample_rates(I2S_NUM_0, sampleRate);
    }

    void writePcm16Mono(const int16_t *samples, size_t sampleCount)
    {
        if (stereoBuf.size() < sampleCount * 2) {
            stereoBuf.resize(sampleCount * 2);
        }
        for (size_t i = 0; i < sampleCount; i++) {
            int16_t s = muted ? (int16_t)0 : applyGain(samples[i]);
            stereoBuf[i * 2] = s;
            stereoBuf[i * 2 + 1] = s;
        }
        size_t written = 0;
        i2s_write(I2S_NUM_0, stereoBuf.data(), sampleCount * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    }

    void increaseVolume()
    {
        if (volumeLevel < 10) volumeLevel++;
        if (volumeLevel > 0) muted = false;
    }

    void decreaseVolume()
    {
        if (volumeLevel > 0) volumeLevel--;
        if (volumeLevel == 0) muted = true;
    }

    void setMute(bool mute) { muted = mute; }
    bool isMuted() const { return muted; }
    uint8_t getVolume() const { return volumeLevel; }

    // For restoring a previously saved level directly (as opposed to the
    // step-at-a-time increase/decreaseVolume() used by the OSD menu).
    void setVolume(uint8_t level) { volumeLevel = (level > 10) ? 10 : level; }

private:
    static constexpr gpio_num_t kPinBck = GPIO_NUM_21;
    static constexpr gpio_num_t kPinWs  = GPIO_NUM_18;
    static constexpr gpio_num_t kPinData = GPIO_NUM_17;

    // 0..10 -> percent gain. Roughly matches the original's step feel, with
    // the whole scale (every step, including the 100% ceiling) trimmed down
    // by a uniform -2dB (linear factor 10^(-2/20) = 0.7943) - baked into the
    // table directly rather than an extra runtime multiply/divide per sample.
    static constexpr uint8_t kVolumePercent[11] = {0, 8, 12, 20, 30, 40, 52, 60, 68, 73, 79};

    int16_t applyGain(int16_t sample) const
    {
        int32_t s = (int32_t)sample * kVolumePercent[volumeLevel] / 100;
        if (s > INT16_MAX) s = INT16_MAX;
        if (s < INT16_MIN) s = INT16_MIN;
        return (int16_t)s;
    }

    bool muted;
    uint8_t volumeLevel; // 0-10
    std::vector<int16_t> stereoBuf;
};

} // namespace EraTV

#endif /* MAIN_AUDIOPLAYER_HPP_ */
