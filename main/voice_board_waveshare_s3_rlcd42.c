/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * Waveshare ESP32-S3-RLCD-4.2 audio board driver.
 *
 * Hardware (verified on the physical board; pinout from the Waveshare
 * ESP32-S3-RLCD-4.2 wiki and schematic):
 *   - ES8311 audio DAC (speaker) ........ I2C address 0x18
 *   - ES7210 audio ADC (dual-mic array) . I2C address 0x40 (some batches 0x42;
 *                                          both are probed at init)
 *   - Shared I2C bus ................... SDA = GPIO13, SCL = GPIO14, 100 kHz
 *   - I2S .............................. MCLK = GPIO16, BCLK = GPIO9,
 *                                        LRCK = GPIO45,
 *                                        DOUT = GPIO8  (ESP32 -> ES8311),
 *                                        DIN  = GPIO10 (ES7210 -> ESP32)
 *   - Speaker amplifier (NS4150B) ...... GPIO46, active high. NOTE: GPIO46 is
 *                                        an ESP32-S3 strapping pin (must be
 *                                        low at reset); it is only driven
 *                                        after boot, starting low.
 *
 * Clocking: the ESP32-S3 is the I2S master and drives a single 48 kHz clock
 * domain for both codecs, which are slaves: MCLK = 12.288 MHz (256 x 48 kHz)
 * on GPIO16, shared BCLK/LRCK. TX and RX live on the same I2S port in
 * full-duplex standard (Philips) mode, 32-bit stereo slots, following the
 * ESP-IDF i2s full-duplex example.
 *
 * The speaker path is native 48 kHz. The ES8311 DAC is mono, so the stereo
 * input is downmixed to mono in software and duplicated to both I2S slots.
 *
 * The microphone path is captured at 48 kHz stereo 32-bit and downsampled
 * 3:1 (boxcar average) plus downmixed to the 16 kHz mono PCM16 the
 * voice_board interface requires. The ES7210 runs its ADC at 48 kHz for this
 * reason; there is no separate 16 kHz clock domain on the shared bus.
 * MIC1/MIC2 (the board's dual-mic array) arrive on SDOUT1, which is the
 * ES7210 data line wired to GPIO10 in the default non-TDM configuration.
 *
 * Register sequences follow the ESPHome es8311 and es7210 components
 * (https://github.com/esphome/esphome/tree/dev/esphome/components/es8311,
 *  .../es7210), using their 48 kHz / MCLK=12.288 MHz coefficient rows.
 */
#include "voice_board.h"
#include <math.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "link.rlcd42";

#define PIN_I2C_SDA       13
#define PIN_I2C_SCL       14
#define PIN_I2S_MCLK      16
#define PIN_I2S_BCLK      9
#define PIN_I2S_LRCK      45
#define PIN_I2S_DOUT      8    // ESP32 -> ES8311 (playback)
#define PIN_I2S_DIN       10   // ES7210 -> ESP32 (capture)
#define PIN_AMP_EN        46   // NS4150B amplifier enable, active high

#define ES8311_ADDR       0x18
#define ES7210_ADDR       0x40
#define ES7210_ADDR_ALT   0x42

#define DEFAULT_VOLUME    60

// Microphone DMA: 6 x 10 ms at 48 kHz stereo 32-bit.
#define MIC_DMA_DESC      6
#define MIC_DMA_FRAMES    480
// Speaker DMA: 6 x 10 ms at 48 kHz stereo 32-bit; silence on underflow.
#define SPK_DMA_DESC      6
#define SPK_DMA_FRAMES    480
// Mic output per read: 20 ms at 16 kHz mono (matches voice.c CAPTURE_CHUNK).
#define MIC_OUT_FRAMES    (VOICE_MIC_RATE / 50)
// Mic input frames per read: 3x for the 48 kHz -> 16 kHz downsample.
#define MIC_IN_FRAMES     (MIC_OUT_FRAMES * 3)
// Speaker downmix scratch: 256 stereo frames per chunk keeps the 3 KB
// player task stack free.
#define SPK_SCRATCH_FRAMES 256

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_es8311;
static i2c_master_dev_handle_t s_es7210;
static SemaphoreHandle_t s_lock;
static i2s_chan_handle_t s_rx, s_tx;
static volatile bool s_mic_on;
static int32_t s_mic_raw[MIC_IN_FRAMES * 2];
static int32_t s_spk_scratch[SPK_SCRATCH_FRAMES * 2];

// ---- I2C helpers ------------------------------------------------------------

static esp_err_t es8311_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit(s_es8311, buf, sizeof(buf), 100);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 reg 0x%02x <- 0x%02x: %s", reg, val, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t es8311_read(uint8_t reg, uint8_t *val) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit_receive(s_es8311, &reg, 1, val, 1, 100);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 reg 0x%02x read: %s", reg, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t es7210_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit(s_es7210, buf, sizeof(buf), 100);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 reg 0x%02x <- 0x%02x: %s", reg, val, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t es7210_read(uint8_t reg, uint8_t *val) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit_receive(s_es7210, &reg, 1, val, 1, 100);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 reg 0x%02x read: %s", reg, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t es7210_update_bits(uint8_t reg, uint8_t mask, uint8_t data) {
    uint8_t v;
    esp_err_t err = es7210_read(reg, &v);
    if (err == ESP_OK) err = es7210_write(reg, (uint8_t)((v & ~mask) | (mask & data)));
    return err;
}

typedef struct {
    uint8_t reg, val;
} regval_t;

static esp_err_t write_table(esp_err_t (*w)(uint8_t, uint8_t),
                             const regval_t *t, size_t n) {
    for (size_t i = 0; i < n; i++) {
        esp_err_t err = w(t[i].reg, t[i].val);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

// ---- ES8311 (DAC) -----------------------------------------------------------
// Sequence and 48 kHz / 12.288 MHz coefficients from the ESPHome es8311
// component. DAC-only use: the ADC/mic section is left untouched because the
// microphones are wired to the ES7210.

static esp_err_t es8311_init(void) {
    uint8_t v;
    esp_err_t err = es8311_write(0x00, 0x1F);  // software reset
    if (err == ESP_OK) err = es8311_write(0x00, 0x00);
    if (err != ESP_OK) return err;

    // Clock manager: all clocks on, MCLK from the pin (12.288 MHz).
    static const regval_t clk[] = {
        {0x01, 0x3F},
        // 0x02: pre_div=1 -> (0<<5), pre_mult=1 -> (0<<3); keep low 3 bits.
        {0x03, 0x10},  // adc fs_mode=0, adc_osr=0x10
        {0x04, 0x10},  // dac_osr=0x10
        {0x05, 0x00},  // adc_div=1, dac_div=1
        // 0x06/0x07 handled below (read-modify-write).
        {0x08, 0xFF},  // lrck_l
    };
    err = write_table(es8311_write, clk, sizeof(clk) / sizeof(clk[0]));
    if (err == ESP_OK) err = es8311_read(0x02, &v);
    if (err == ESP_OK) err = es8311_write(0x02, (uint8_t)(v & 0x07));
    if (err == ESP_OK) err = es8311_read(0x06, &v);
    if (err == ESP_OK) err = es8311_write(0x06, (uint8_t)((v & 0xE0) | 0x03));  // bclk_div=4
    if (err == ESP_OK) err = es8311_read(0x07, &v);
    if (err == ESP_OK) err = es8311_write(0x07, (uint8_t)(v & 0xC0));          // lrck_h=0
    if (err != ESP_OK) return err;

    // I2S format, 32-bit words both directions.
    if (err == ESP_OK) err = es8311_read(0x00, &v);
    if (err == ESP_OK) err = es8311_write(0x00, (uint8_t)(v & 0xBF));
    static const regval_t fmt[] = {
        {0x09, 0x10},  // SDP in: I2S, 32-bit
        {0x0A, 0x10},  // SDP out: I2S, 32-bit
    };
    if (err == ESP_OK) err = write_table(es8311_write, fmt, sizeof(fmt) / sizeof(fmt[0]));
    if (err != ESP_OK) return err;

    // The Waveshare codec driver also initializes the analog reference/bias
    // registers. Reset defaults alone do not power the DAC output reliably.
    static const regval_t analog[] = {
        {0x44, 0x08}, {0x44, 0x08}, // vendor repeats the first write for reliability
        {0x0B, 0x00}, {0x0C, 0x00},
        {0x10, 0x1F}, {0x11, 0x7F},
        {0x14, 0x1A}, {0x45, 0x00},
    };
    err = write_table(es8311_write, analog, sizeof(analog) / sizeof(analog[0]));
    if (err != ESP_OK) return err;

    // Power up the DAC path.
    static const regval_t pwr[] = {
        {0x32, 0xBF},  // DAC volume 0 dB (voice_board_set_volume adjusts later)
        {0x0D, 0x01},  // analog power up
        {0x0E, 0x02},  // (ADC modulator; kept for parity with the reference)
        {0x12, 0x00},  // DAC power up
        {0x13, 0x10},  // enable output to HP drive
        {0x37, 0x08},  // bypass DAC equalizer
        {0x00, 0x80},  // power on
    };
    return write_table(es8311_write, pwr, sizeof(pwr) / sizeof(pwr[0]));
}

// ES8311 register 0x32 is in 0.5 dB steps; 0xBF is unity gain.
// A percentage is an amplitude ratio, not a register percentage.
static uint8_t dac_volume(int percent) {
    if (percent <= 0) return 0;
    if (percent >= 100) return 0xBF;
    return (uint8_t)lroundf(191.0f + 40.0f * log10f(percent / 100.0f));
}

void voice_board_set_volume(int percent) {
    if (!s_es8311) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint8_t vol = dac_volume(percent);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t buf[2] = {0x32, vol};
    esp_err_t err = i2c_master_transmit(s_es8311, buf, sizeof(buf), 100);
    // Mute the DAC at 0%.
    uint8_t r31 = 0;
    if (err == ESP_OK) err = i2c_master_transmit_receive(s_es8311, &(uint8_t){0x31}, 1, &r31, 1, 100);
    if (err == ESP_OK) {
        if (percent == 0) r31 |= 0x60;
        else r31 &= ~0x60;
        uint8_t mbuf[2] = {0x31, r31};
        err = i2c_master_transmit(s_es8311, mbuf, sizeof(mbuf), 100);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) ESP_LOGW(TAG, "volume: %s", esp_err_to_name(err));
}

// ---- ES7210 (ADC) -----------------------------------------------------------
// Sequence and 48 kHz / 12.288 MHz coefficients from the ESPHome es7210
// component. MIC1/MIC2 (the board's dual-mic array) at 30 dB PGA, slave mode,
// 32-bit I2S, non-TDM (MIC1/MIC2 -> SDOUT1).

#define ES7210_MIC_GAIN_30DB  0x0A  // gain field value; final reg = 0x10 | this

static esp_err_t es7210_mic_gain(uint8_t gain_reg, uint8_t clk_mask) {
    // Per-mic sequencing from the reference: enable the mic's clock domain,
    // cycle its power domain, then latch the gain.
    esp_err_t err = es7210_update_bits(0x01, clk_mask, 0x00);
    if (err == ESP_OK) err = es7210_write(0x4B, 0x00);
    if (err == ESP_OK) err = es7210_update_bits(gain_reg, 0x10, 0x10);
    if (err == ESP_OK) err = es7210_update_bits(gain_reg, 0x0F, ES7210_MIC_GAIN_30DB);
    return err;
}

static esp_err_t es7210_init(void) {
    esp_err_t err = es7210_write(0x00, 0xFF);  // software reset
    if (err == ESP_OK) err = es7210_write(0x00, 0x32);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));

    static const regval_t init[] = {
        {0x01, 0x3F},  // all ADC clocks off for now
        {0x09, 0x30},  // power-up init timing
        {0x0A, 0x30},
        {0x23, 0x2A},  // HPF ADC1/2
        {0x22, 0x0A},
        {0x20, 0x0A},  // HPF ADC3/4
        {0x21, 0x2A},
        // 0x08 handled below (slave mode).
        {0x40, 0xC3},  // analog power
        {0x41, 0x70},  // mic bias MIC1/2
        {0x42, 0x70},  // mic bias MIC3/4
        {0x11, 0x80},  // 32-bit I2S
        {0x12, 0x00},  // non-TDM: MIC1/2 -> SDOUT1, MIC3/4 -> SDOUT2
        // 48 kHz @ 12.288 MHz MCLK coefficient row:
        {0x02, 0xC1},  // adc_div=1, doubler on, dll bypass
        {0x07, 0x20},  // osr
        {0x04, 0x01},  // lrck_h
        {0x05, 0x00},  // lrck_l
    };
    err = write_table(es7210_write, init, sizeof(init) / sizeof(init[0]));
    if (err == ESP_OK) err = es7210_update_bits(0x08, 0x01, 0x00);  // slave mode
    if (err != ESP_OK) return err;

    // PGA gain 30 dB on MIC1 and MIC2 (clock domains: ADC1/ADC2 + MIC12).
    err = es7210_update_bits(0x43, 0x10, 0x00);
    if (err == ESP_OK) err = es7210_update_bits(0x44, 0x10, 0x00);
    if (err == ESP_OK) err = es7210_write(0x4B, 0xFF);
    if (err == ESP_OK) err = es7210_mic_gain(0x43, 0x0B);  // MIC1
    if (err == ESP_OK) err = es7210_mic_gain(0x44, 0x0B);  // MIC2
    if (err != ESP_OK) return err;

    static const regval_t pwr[] = {
        {0x47, 0x08},  // MIC1 power
        {0x48, 0x08},  // MIC2 power
        {0x06, 0x04},  // DLL power down
        {0x4B, 0x0F},  // MIC12 bias/ADC/PGA power
        {0x00, 0x71},  // enable device
        {0x00, 0x41},
    };
    return write_table(es7210_write, pwr, sizeof(pwr) / sizeof(pwr[0]));
}

// ---- I2S --------------------------------------------------------------------

static esp_err_t i2s_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = SPK_DMA_DESC;
    chan_cfg.dma_frame_num = SPK_DMA_FRAMES;
    chan_cfg.auto_clear = true;  // silence on TX underflow
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }
    // One 48 kHz clock domain shared by TX and RX on the same port (ESP-IDF
    // full-duplex pattern): MCLK = 256 x 48 kHz = 12.288 MHz for both codecs.
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_SPEAKER_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_LRCK,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) ESP_LOGE(TAG, "I2S: %s", esp_err_to_name(err));
    return err;
}

// ---- voice_board interface --------------------------------------------------

esp_err_t voice_board_mic_start(void) {
    if (!s_rx) return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2s_channel_enable(s_rx);
    if (err == ESP_OK) s_mic_on = true;
    return err;
}

void voice_board_mic_stop(void) {
    if (s_mic_on) {
        i2s_channel_disable(s_rx);
        s_mic_on = false;
    }
}

size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    *peak = 0;
    if (!s_mic_on) return 0;
    if (frames > MIC_OUT_FRAMES) frames = MIC_OUT_FRAMES;
    size_t bytes = 0;
    if (i2s_channel_read(s_rx, s_mic_raw, frames * 3 * 2 * sizeof(int32_t), &bytes,
                         pdMS_TO_TICKS(500)) != ESP_OK) {
        return 0;
    }
    // 48 kHz stereo 32-bit -> 16 kHz mono PCM16: average each 3 consecutive
    // frames and downmix L+R (MIC1+MIC2).
    size_t n = bytes / (2 * sizeof(int32_t)) / 3;
    for (size_t i = 0; i < n; i++) {
        int64_t sum = 0;
        for (int k = 0; k < 3; k++) {
            sum += (int64_t)s_mic_raw[6 * i + 2 * k];
            sum += (int64_t)s_mic_raw[6 * i + 2 * k + 1];
        }
        int16_t s = (int16_t)((int32_t)(sum / 6) >> 16);
        pcm[i] = s;
        int a = s < 0 ? -(int)s : (int)s;
        if (a > *peak) *peak = a;
    }
    return n;
}

esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    if (!s_tx) return ESP_ERR_INVALID_STATE;
    // The ES8311 DAC is mono: downmix stereo to mono and duplicate it to both
    // I2S slots so the codec's left-channel pick-up always gets the mix.
    while (count) {
        size_t n = count < SPK_SCRATCH_FRAMES ? count : SPK_SCRATCH_FRAMES;
        for (size_t i = 0; i < n; i++) {
            int64_t sum = (int64_t)frames[2 * i] + (int64_t)frames[2 * i + 1];
            int32_t mono = (int32_t)(sum / 2);
            s_spk_scratch[2 * i] = mono;
            s_spk_scratch[2 * i + 1] = mono;
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_spk_scratch, n * 2 * sizeof(int32_t),
                                          &written, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) return err;
        if (written != n * 2 * sizeof(int32_t)) return ESP_ERR_TIMEOUT;
        frames += 2 * n;
        count -= n;
    }
    return ESP_OK;
}

void voice_board_amp(bool on) {
    gpio_set_level(PIN_AMP_EN, on ? 1 : 0);
}

bool voice_board_muted(void) {
    return false;  // no hardware mute switch on this board
}

int voice_board_dial_steps(void) {
    return 0;  // no dial on this board
}

esp_err_t voice_board_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    gpio_config_t amp_cfg = {
        .pin_bit_mask = 1ULL << PIN_AMP_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&amp_cfg);
    if (err != ESP_OK) return err;
    voice_board_amp(false);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus: %s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_probe(s_bus, ES8311_ADDR, 100) != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 not answering at 0x%02x", ES8311_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    dev_cfg.device_address = ES8311_ADDR;
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_es8311);
    if (err != ESP_OK) return err;

    uint8_t es7210_addr = ES7210_ADDR;
    if (i2c_master_probe(s_bus, es7210_addr, 100) != ESP_OK) {
        es7210_addr = ES7210_ADDR_ALT;
        if (i2c_master_probe(s_bus, es7210_addr, 100) != ESP_OK) {
            ESP_LOGE(TAG, "ES7210 not answering at 0x%02x or 0x%02x",
                     ES7210_ADDR, ES7210_ADDR_ALT);
            return ESP_ERR_NOT_FOUND;
        }
    }
    ESP_LOGI(TAG, "ES7210 at 0x%02x", es7210_addr);
    dev_cfg.device_address = es7210_addr;
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_es7210);
    if (err != ESP_OK) return err;

    err = es8311_init();
    if (err == ESP_OK) err = es7210_init();
    if (err == ESP_OK) err = i2s_init();
    if (err != ESP_OK) return err;

    voice_board_set_volume(DEFAULT_VOLUME);
    ESP_LOGI(TAG, "audio ready: ES8311 DAC + ES7210 dual-mic, 48 kHz I2S, mic downsampled to 16 kHz");
    return ESP_OK;
}
