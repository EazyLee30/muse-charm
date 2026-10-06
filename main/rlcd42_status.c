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

// led_status.h on the Waveshare ESP32-S3-RLCD-4.2 (4.2" reflective mono LCD).
//
// Hardware (pins verified against the Waveshare official schematic, and
// against board_pins.h in our muse-charm project):
//   - ST7305 controller, 300x400 portrait panel, driven here as 400x300
//     landscape: the same 90-degree clockwise rotation our Arduino firmware
//     uses through U8G2_R1, so the screen orientation matches muse-charm.
//   - 4-wire SPI, write-only (no MISO): SCLK=GPIO11, MOSI=GPIO12,
//     CS=GPIO40, DC=GPIO5, RST=GPIO41. 24 MHz, the value Waveshare's own
//     examples use and the one verified stable on this board.
//   - 1 bit per pixel, no backlight: a reflective panel readable in ambient
//     light, like e-paper but with millisecond refreshes.
//
// Like epaper_status.c this file replaces led_status.c: instead of an LED
// animation it shows a still status screen (the agent's name, a status line
// and a small voice-state icon) and redraws only when something changes.
// The ST7305 runs at 32 Hz in high-power mode, so a mic level meter while
// listening is cheap.
//
// ST7305 init: the 300x400 sequence from U8g2's u8x8_d_st7305.c, which is
// exactly what our Arduino muse-charm firmware uses
// (U8G2_ST7305_300X400_F_4W_HW_SPI) and is verified working on this board.
// Cross-checked against Waveshare's official ESP-IDF driver
// (waveshareteam/ESP32-S3-RLCD-4.2, Apache-2.0,
// 02_Example/ESP-IDF/11_U8G2_Test/components/u8g2_st7305/u8g2_st7305.c),
// which documents the full-frame address window used below:
//   0x2A {0x12, 0x2A} (columns in 12-px units), 0x2B {0x00, 0xC7} (rows in
//   2-px units), then 0x2C with 15000 bytes, 2 bits per pixel, MSB first.
// Each 2-bit unit packs two vertical 1-bit pixels (bit 1 = even row,
// bit 0 = odd row); with inversion off (0x20) a set bit is black.
//
// The icons below are original single-bit drawings; no Jollybot/avatar
// content is used or referenced.

#include "led_status.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pixel_font.h"
#include "stack_monitor.h"

static const char *TAG = "link.led";

// ---- Panel ---------------------------------------------------------------

#define RLCD_HOST     SPI3_HOST  // Waveshare's ESP-IDF driver uses SPI3
#define RLCD_PIN_DC   5
#define RLCD_PIN_SCLK 11
#define RLCD_PIN_MOSI 12
#define RLCD_PIN_CS   40
#define RLCD_PIN_RST  41
#define RLCD_SPI_HZ   (24 * 1000 * 1000)

// Logical screen: 400x300 landscape (U8G2_R1 of the 300x400 panel).
#define RLCD_W         400
#define RLCD_H         300
#define RLCD_ROW_BYTES (RLCD_W / 8)
#define RLCD_FB_BYTES  (RLCD_ROW_BYTES * RLCD_H)  // 15000

// Full-frame transfer: 200 row units (2 px each) x 25 column groups
// (12 px each) x 3 bytes (12 px at 2 bits, MSB first).
#define RLCD_ROW_UNITS  200
#define RLCD_COL_GROUPS 25
#define RLCD_TX_BYTES   (RLCD_ROW_UNITS * RLCD_COL_GROUPS * 3)  // 15000
#define RLCD_CHUNK_BYTES 4096

#define VOLUME_MS   1500
#define LEVEL_STEPS 10

static spi_device_handle_t s_spi;
static uint8_t *s_fb;  // 1-bit landscape framebuffer, 1 = black
static uint8_t *s_tx;  // DMA buffer holding the converted panel frame
static bool s_ready;

static SemaphoreHandle_t s_panel_lock;  // serialises SPI access
static SemaphoreHandle_t s_lock;        // guards s_fb and s_image_mode
static SemaphoreHandle_t s_mutex;       // guards the requested state below
static bool s_image_mode;               // a draw_rect image replaces the status
static led_state_t s_state = LED_STATE_BOOT;
static led_voice_t s_voice = LED_VOICE_IDLE;
static int s_level_q;  // mic level, quantised to LEVEL_STEPS
static char s_title[48];
static int s_volume;  // -1 when no volume overlay is showing
static int64_t s_volume_until;
static TaskHandle_t s_task;

// ---- SPI ------------------------------------------------------------------

static esp_err_t rlcd_tx(bool dc, const void *buf, size_t len) {
    gpio_set_level(RLCD_PIN_DC, dc ? 1 : 0);
    gpio_set_level(RLCD_PIN_CS, 0);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = buf,
    };
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    gpio_set_level(RLCD_PIN_CS, 1);
    return err;
}

static esp_err_t rlcd_cmd(uint8_t cmd) {
    uint8_t c = cmd;  // stack: internal RAM, DMA-safe
    return rlcd_tx(false, &c, 1);
}

// Command followed by data, CS held low across both (as Waveshare does).
static esp_err_t rlcd_cmd_data(uint8_t cmd, const uint8_t *data, size_t len) {
    uint8_t buf[11];
    if (len > sizeof(buf) - 1) return ESP_ERR_INVALID_ARG;
    buf[0] = cmd;
    memcpy(buf + 1, data, len);
    gpio_set_level(RLCD_PIN_DC, 0);
    gpio_set_level(RLCD_PIN_CS, 0);
    spi_transaction_t t = {.length = 8, .tx_buffer = buf};
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err == ESP_OK && len) {
        gpio_set_level(RLCD_PIN_DC, 1);
        t.length = len * 8;
        t.tx_buffer = buf + 1;
        err = spi_device_polling_transmit(s_spi, &t);
    }
    gpio_set_level(RLCD_PIN_CS, 1);
    return err;
}

// ---- ST7305 init ------------------------------------------------------------

typedef struct {
    uint8_t cmd, len, delay_ms;
    uint8_t data[10];
} rlcd_init_cmd_t;

// U8g2's u8x8_d_st7305_300x400_init_seq (see the file header for provenance).
static const rlcd_init_cmd_t s_init[] = {
    {0x01, 0, 100, {0}},                          // software reset
    {0xD6, 2, 0, {0x13, 0x02}},                   // NVM load control
    {0xD1, 1, 0, {0x01}},                         // booster enable
    {0xC0, 2, 0, {0x12, 0x0A}},                   // gate voltage
    {0xC1, 4, 0, {0x3C, 0x3E, 0x3C, 0x3C}},       // VSHP
    {0xC2, 4, 0, {0x23, 0x21, 0x23, 0x23}},       // VSLP
    {0xC4, 4, 0, {0x5A, 0x5C, 0x5A, 0x5A}},       // VSHN
    {0xC5, 4, 0, {0x37, 0x35, 0x37, 0x37}},       // VSLN
    {0xD8, 2, 0, {0xA6, 0xE9}},                   // OSC setting
    {0xB2, 1, 0, {0x12}},                         // frame rate: HPM 32 Hz
    {0xB3, 10, 0, {0xE5, 0xF6, 0x17, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x71}},
    {0xB4, 8, 0, {0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45}},
    {0x62, 3, 0, {0x32, 0x03, 0x1F}},             // gate timing
    {0xB7, 1, 0, {0x13}},                         // source EQ enable
    {0xB0, 1, 0, {0x64}},                         // duty: 400 lines
    {0x11, 0, 10, {0}},                           // sleep out
    {0xC9, 1, 0, {0x00}},                         // source voltage select
    {0x36, 1, 0, {0x48}},                         // MADCTL
    {0x3A, 1, 0, {0x11}},                         // 4 gray levels
    {0xB9, 1, 0, {0x20}},                         // gamma: mono
    {0xB8, 1, 0, {0x29}},                         // panel setting
    {0x35, 1, 0, {0x00}},                         // TE off
    {0xD0, 1, 0, {0xFF}},                         // auto power down
    {0x38, 0, 0, {0}},                            // high-power mode
    {0x29, 0, 0, {0}},                            // display on
    {0x20, 0, 0, {0}},                            // inversion off: white paper
    {0xBB, 1, 0, {0x4F}},                         // clear RAM
};

static esp_err_t rlcd_init_panel(void) {
    gpio_set_level(RLCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(RLCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(RLCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    for (size_t i = 0; i < sizeof(s_init) / sizeof(s_init[0]); i++) {
        esp_err_t err = rlcd_cmd_data(s_init[i].cmd, s_init[i].data, s_init[i].len);
        if (err != ESP_OK) return err;
        if (s_init[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(s_init[i].delay_ms));
    }
    return ESP_OK;
}

// ---- 1-bit drawing ------------------------------------------------------------

// Framebuffer: row-major, 8 horizontal pixels per byte, MSB leftmost,
// 1 = black. Caller holds s_lock.

static inline void fb_px(int x, int y, bool black) {
    if ((unsigned)x >= RLCD_W || (unsigned)y >= RLCD_H) return;
    uint8_t *b = s_fb + (size_t)y * RLCD_ROW_BYTES + (x >> 3);
    uint8_t m = (uint8_t)(0x80 >> (x & 7));
    if (black) *b |= m;
    else *b &= (uint8_t)~m;
}

static void fb_rect(int x, int y, int w, int h, bool black) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > RLCD_W) w = RLCD_W - x;
    if (y + h > RLCD_H) h = RLCD_H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) fb_px(xx, yy, black);
}

static void fb_hline(int x, int y, int w, bool black) { fb_rect(x, y, w, 1, black); }

static void fb_circle(int cx, int cy, int r, bool black) {
    for (int y = -r; y <= r; y++) {
        int w = (int)sqrt((double)(r * r - y * y));
        fb_hline(cx - w, cy + y, 2 * w + 1, black);
    }
}

static int text_width(const char *t, int scale) {
    return (int)strlen(t) * (PIXEL_FONT_WIDTH + 1) * scale - scale;
}

// Black text; out-of-range bytes show as '?'. Caller holds s_lock.
static void fb_text(const char *t, int x, int y, int scale) {
    for (size_t i = 0; t[i]; i++) {
        unsigned char ch = (unsigned char)t[i];
        if (ch < PIXEL_FONT_FIRST || ch > PIXEL_FONT_LAST) ch = '?';
        const uint8_t *glyph = pixel_font[ch - PIXEL_FONT_FIRST];
        for (int gx = 0; gx < PIXEL_FONT_WIDTH; gx++)
            for (int gy = 0; gy < PIXEL_FONT_HEIGHT; gy++)
                if ((glyph[gx] >> gy) & 1)
                    fb_rect(x + (int)((i * (PIXEL_FONT_WIDTH + 1) + gx) * scale),
                            y + gy * scale, scale, scale, true);
    }
}

static void fb_text_centered(const char *t, int y, int scale) {
    fb_text(t, (RLCD_W - text_width(t, scale)) / 2, y, scale);
}

// ---- Charm mascot (original fluffy blob, pixel-art) ---------------------------
// A cute fluffy round creature: jagged fur edge, dot eyes, small smile.
// Expressions change with the voice state. Centered at (cx, cy), ~110px wide.

typedef enum {
    CHARM_HAPPY,      // idle / ready: smile
    CHARM_LISTENING,  // big attentive eyes
    CHARM_THINKING,   // eyes looking up
    CHARM_SPEAKING,   // open mouth
    CHARM_SAD,        // error: frown
} charm_expr_t;

static void draw_charm(int cx, int cy, charm_expr_t expr) {
    const int R = 52;
    // Fluffy body: filled circle + fur spikes around the edge.
    fb_circle(cx, cy, R, true);
    for (int a = 0; a < 360; a += 12) {
        double rad = a * 3.141592653589793 / 180.0;
        int sx = (int)(cx + (R - 2) * cos(rad));
        int sy = (int)(cy + (R - 2) * sin(rad));
        int ex = (int)(cx + (R + 10) * cos(rad));
        int ey = (int)(cy + (R + 10) * sin(rad));
        // spike: small thick line from edge outward
        for (int t = 0; t <= 6; t++) {
            int px = sx + (ex - sx) * t / 6;
            int py = sy + (ey - sy) * t / 6;
            fb_rect(px - 2, py - 2, 5, 5, true);
        }
    }
    // Belly: white patch to suggest fluff shading.
    fb_circle(cx, cy + 18, 30, false);
    // Eyes.
    int eye_y = cy - 8;
    int eye_dx = 20;
    int eye_r = (expr == CHARM_LISTENING) ? 9 : 7;
    if (expr == CHARM_THINKING) eye_y -= 6;
    if (expr == CHARM_SAD) {
        // X eyes for error
        for (int i = -6; i <= 6; i++) {
            fb_px(cx - eye_dx + i, eye_y + i, true);
            fb_px(cx - eye_dx + i, eye_y - i, true);
            fb_px(cx + eye_dx + i, eye_y + i, true);
            fb_px(cx + eye_dx + i, eye_y - i, true);
        }
    } else {
        fb_circle(cx - eye_dx, eye_y, eye_r, true);
        fb_circle(cx + eye_dx, eye_y, eye_r, true);
        // eye highlights (white dots)
        fb_circle(cx - eye_dx + 2, eye_y - 2, 2, false);
        fb_circle(cx + eye_dx + 2, eye_y - 2, 2, false);
    }
    // Mouth.
    int my = cy + 16;
    switch (expr) {
        case CHARM_HAPPY:
        case CHARM_LISTENING:
            // smile: arc
            for (int i = -12; i <= 12; i++) {
                int yy = my + (i * i) / 24;
                fb_rect(cx + i - 1, yy, 3, 3, true);
            }
            break;
        case CHARM_THINKING:
            // small flat line, slightly off-center
            fb_rect(cx - 8, my + 2, 16, 4, true);
            break;
        case CHARM_SPEAKING:
            // open oval mouth
            for (int yy = -8; yy <= 8; yy++) {
                int w = (int)(10 * sqrt(1.0 - (double)(yy * yy) / 64.0));
                fb_hline(cx - w, my + yy, 2 * w + 1, true);
            }
            break;
        case CHARM_SAD:
            // frown: inverted arc
            for (int i = -12; i <= 12; i++) {
                int yy = my + 8 - (i * i) / 24;
                fb_rect(cx + i - 1, yy, 3, 3, true);
            }
            break;
    }
}

// ---- Voice icons (original single-bit drawings) -------------------------------

static void draw_mic(void) {
    fb_rect(188, 118, 24, 42, true);       // capsule
    fb_rect(192, 127, 16, 3, false);       // grill slits
    fb_rect(192, 137, 16, 3, false);
    fb_rect(192, 147, 16, 3, false);
    fb_rect(182, 148, 6, 24, true);        // yoke
    fb_rect(212, 148, 6, 24, true);
    fb_rect(182, 166, 36, 6, true);
    fb_rect(197, 172, 6, 16, true);        // stand
    fb_rect(184, 188, 32, 6, true);
}

static void draw_level(int q) {
    for (int i = 0; i < LEVEL_STEPS; i++) {
        int h = i < q ? 36 : 6;
        fb_rect(150 + i * 10, 246 - h, 8, h, true);
    }
}

static void draw_speaker(void) {
    fb_rect(166, 138, 28, 54, true);  // box
    for (int x = 194; x <= 226; x++) {  // cone
        int t = x - 194;
        fb_rect(x, 138 - t * 26 / 32, 1, 54 + t * 52 / 32, true);
    }
    for (int deg = -45; deg <= 45; deg += 5) {  // sound waves
        double rad = deg * 3.141592653589793 / 180.0;
        fb_rect((int)(228 + 28 * cos(rad)), (int)(165 + 28 * sin(rad)), 5, 5, true);
        fb_rect((int)(228 + 42 * cos(rad)), (int)(165 + 42 * sin(rad)), 5, 5, true);
    }
}

static void draw_dots(void) {
    fb_circle(172, 165, 9, true);
    fb_circle(200, 165, 9, true);
    fb_circle(228, 165, 9, true);
}

static void draw_arrow_down(void) {
    fb_rect(192, 118, 16, 40, true);
    for (int i = 0; i < 16; i++) fb_hline(184 + i, 158 + i, 32 - 2 * i, true);
}

static void draw_x(void) {
    for (int i = 0; i < 56; i++) {
        fb_rect(172 + i, 137 + i, 8, 8, true);
        fb_rect(228 - i, 137 + i, 8, 8, true);
    }
}

// ---- Status screen ------------------------------------------------------------

static const char *conn_label(led_state_t s) {
    switch (s) {
        case LED_STATE_BOOT:                     return "Starting";
        case LED_STATE_SETUP_IDLE:               return "Setup";
        case LED_STATE_BLE_ADVERTISING:          return "Pair in the Muse app";
        case LED_STATE_BLE_CONNECTED:            return "Pairing";
        case LED_STATE_PAIRING_CONFIRM_REQUIRED: return "Press BOOT to confirm";
        case LED_STATE_WAITING_FOR_WIFI: return "Waiting for WiFi setup";
        case LED_STATE_WIFI_CONNECTING:          return "WiFi connecting";
        case LED_STATE_WIFI_CONNECTED:
        case LED_STATE_AUTH_OK:
        case LED_STATE_VM_SWITCHING:
        case LED_STATE_VM_OK:                    return "Connecting";
        case LED_STATE_WS_CONNECTED:             return "Ready";
        case LED_STATE_WS_DISCONNECTED:          return "Reconnecting";
        case LED_STATE_UNPAIRED:                 return "Not paired";
        case LED_STATE_ERROR:                    return "Error";
    }
    return "";
}

static const char *voice_label(led_voice_t v) {
    switch (v) {
        case LED_VOICE_IDLE:        return NULL;
        case LED_VOICE_LISTENING:   return "Listening";
        case LED_VOICE_TRANSCRIBING: return "Transcribing";
        case LED_VOICE_THINKING:    return "Thinking";
        case LED_VOICE_BUFFERING:   return "Replying";
        case LED_VOICE_SPEAKING:    return "Speaking";
        case LED_VOICE_ERROR:       return "Voice error";
    }
    return NULL;
}

// Caller holds s_lock.
static void render_status(const char *title, led_state_t state, led_voice_t voice, int level_q) {
    memset(s_fb, 0x00, RLCD_FB_BYTES);  // white paper
    if (title[0]) fb_text_centered(title, 20, 3);
    fb_rect(24, 60, RLCD_W - 48, 2, true);
    const char *vl = voice_label(voice);
    fb_text_centered(vl ? vl : conn_label(state), 80, 2);
    // Charm mascot with expression per state, centered on screen.
    charm_expr_t expr = CHARM_HAPPY;
    switch (voice) {
        case LED_VOICE_LISTENING:   expr = CHARM_LISTENING; break;
        case LED_VOICE_THINKING:
        case LED_VOICE_TRANSCRIBING: expr = CHARM_THINKING; break;
        case LED_VOICE_SPEAKING:
        case LED_VOICE_BUFFERING:   expr = CHARM_SPEAKING; break;
        case LED_VOICE_ERROR:       expr = CHARM_SAD; break;
        case LED_VOICE_IDLE:        break;
    }
    if (state == LED_STATE_ERROR) expr = CHARM_SAD;
    draw_charm(RLCD_W / 2, 195, expr);
    // Mic level meter while listening, below the charm.
    if (voice == LED_VOICE_LISTENING) draw_level(level_q);
}

// Caller holds s_lock.
static void render_volume(int percent) {
    memset(s_fb, 0x00, RLCD_FB_BYTES);
    fb_text_centered("Volume", 66, 3);
    const int bx = 60, bw = RLCD_W - 120, by = 140, bh = 34;
    fb_rect(bx, by, bw, 3, true);
    fb_rect(bx, by + bh - 3, bw, 3, true);
    fb_rect(bx, by, 3, bh, true);
    fb_rect(bx + bw - 3, by, 3, bh, true);
    fb_rect(bx + 6, by + 6, (bw - 12) * percent / 100, bh - 12, true);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", percent);
    fb_text_centered(pct, 200, 3);
}

// ---- Panel flush ---------------------------------------------------------------

// Convert the 400x300 1-bit framebuffer into the panel's native 300x400,
// 2-bits-per-pixel frame with the U8G2_R1 rotation (landscape pixel (x, y)
// shows at panel (299 - y, x)), then write the full window.
// Caller holds s_panel_lock and s_lock.
static esp_err_t rlcd_flush(void) {
    for (int ru = 0; ru < RLCD_ROW_UNITS; ru++) {
        for (int cg = 0; cg < RLCD_COL_GROUPS; cg++) {
            uint8_t *out = s_tx + ((size_t)ru * RLCD_COL_GROUPS + cg) * 3;
            for (int b = 0; b < 3; b++) {
                unsigned v = 0;
                for (int p = 0; p < 4; p++) {
                    int bx = cg * 12 + b * 4 + p;  // panel column 0..299
                    // R1: panel (bx, by) shows fb (x, y) = (by, 299 - bx).
                    int fb_y = 299 - bx;
                    int fb_x_even = ru * 2, fb_x_odd = ru * 2 + 1;
                    const uint8_t *row = s_fb + (size_t)fb_y * RLCD_ROW_BYTES;
                    int even = (row[fb_x_even >> 3] >> (7 - (fb_x_even & 7))) & 1;
                    int odd = (row[fb_x_odd >> 3] >> (7 - (fb_x_odd & 7))) & 1;
                    v = (v << 2) | (unsigned)(even ? 2 : 0) | (unsigned)(odd ? 1 : 0);
                }
#ifdef CONFIG_RLCD42_DARK_MODE
                // Invert only the transfer data; repeated image flushes and
                // partial draws must preserve the canonical framebuffer.
                out[b] = (uint8_t)(v ^ 0xFF);
#else
                out[b] = (uint8_t)v;
#endif
            }
        }
    }

    static const uint8_t win_ca[2] = {0x12, 0x2A};
    static const uint8_t win_ra[2] = {0x00, 0xC7};
    esp_err_t err = rlcd_cmd_data(0x2A, win_ca, sizeof(win_ca));
    if (err == ESP_OK) err = rlcd_cmd_data(0x2B, win_ra, sizeof(win_ra));
    if (err == ESP_OK) err = rlcd_cmd(0x2C);
    if (err != ESP_OK) return err;

    gpio_set_level(RLCD_PIN_DC, 1);
    gpio_set_level(RLCD_PIN_CS, 0);
    for (size_t off = 0; off < RLCD_TX_BYTES && err == ESP_OK; off += RLCD_CHUNK_BYTES) {
        size_t n = RLCD_TX_BYTES - off < RLCD_CHUNK_BYTES ? RLCD_TX_BYTES - off : RLCD_CHUNK_BYTES;
        spi_transaction_t t = {.length = n * 8, .tx_buffer = s_tx + off};
        err = spi_device_polling_transmit(s_spi, &t);
    }
    gpio_set_level(RLCD_PIN_CS, 1);
    return err;
}

// ---- Background task -------------------------------------------------------------

static void rlcd_task(void *arg) {
    (void)arg;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    for (;;) {
        // A shown volume overlay reverts to the status screen on its own.
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        int64_t until = s_volume_until;
        xSemaphoreGive(s_mutex);
        TickType_t wait = portMAX_DELAY;
        int64_t now = esp_timer_get_time();
        if (until > now) wait = pdMS_TO_TICKS((uint32_t)((until - now) / 1000) + 5);
        ulTaskNotifyTake(pdTRUE, wait);

        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool image = s_image_mode;
        xSemaphoreGive(s_lock);
        if (!image) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            led_state_t state = s_state;
            led_voice_t voice = s_voice;
            int level_q = s_level_q;
            int volume = s_volume;
            bool show_vol = s_volume_until > esp_timer_get_time();
            char title[sizeof(s_title)];
            memcpy(title, s_title, sizeof(title));
            xSemaphoreGive(s_mutex);

            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (show_vol) render_volume(volume);
            else render_status(title, state, voice, level_q);
            esp_err_t err = rlcd_flush();
            xSemaphoreGive(s_lock);
            if (err != ESP_OK) ESP_LOGE(TAG, "panel refresh failed: %s", esp_err_to_name(err));
        }
        xSemaphoreGive(s_panel_lock);
        stack_monitor_poll(&stack);
    }
}

// ---- led_status.h -----------------------------------------------------------

static esp_err_t rlcd_spi_init(void) {
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << RLCD_PIN_DC | 1ULL << RLCD_PIN_CS | 1ULL << RLCD_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
    gpio_set_level(RLCD_PIN_CS, 1);
    gpio_set_level(RLCD_PIN_DC, 1);
    gpio_set_level(RLCD_PIN_RST, 1);

    const spi_bus_config_t bus = {
        .mosi_io_num = RLCD_PIN_MOSI,
        .miso_io_num = -1,  // write-only
        .sclk_io_num = RLCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = RLCD_CHUNK_BYTES,
    };
    // CS is driven manually (spics_io_num = -1), as in Waveshare's driver.
    const spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = RLCD_SPI_HZ,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    err = spi_bus_initialize(RLCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) err = spi_bus_add_device(RLCD_HOST, &dev, &s_spi);
    return err;
}

bool led_status_init(void) {
    s_fb = heap_caps_malloc(RLCD_FB_BYTES, MALLOC_CAP_SPIRAM);
    s_tx = heap_caps_malloc(RLCD_TX_BYTES, MALLOC_CAP_DMA);
    s_panel_lock = xSemaphoreCreateMutex();
    s_lock = xSemaphoreCreateMutex();
    s_mutex = xSemaphoreCreateMutex();
    if (!s_fb || !s_tx || !s_panel_lock || !s_lock || !s_mutex) {
        ESP_LOGE(TAG, "status screen alloc failed");
        return false;
    }
    s_volume = -1;

    if (rlcd_spi_init() != ESP_OK || rlcd_init_panel() != ESP_OK) {
        ESP_LOGE(TAG, "ST7305 init failed");
        return false;
    }

    // Start from a clean white screen, then draw the boot status.
    xSemaphoreTake(s_panel_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_fb, 0x00, RLCD_FB_BYTES);
    esp_err_t err = rlcd_flush();
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_panel_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "first panel refresh failed: %s", esp_err_to_name(err));
        return false;
    }

    if (xTaskCreate(rlcd_task, "rlcd_status", 4096, NULL, 2, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the status task");
        return false;
    }
    s_ready = true;
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "LED status ready: Waveshare ESP32-S3-RLCD-4.2 ST7305 %dx%d mono",
             RLCD_W, RLCD_H);
    return true;
}

void led_status_set_state(led_state_t state) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = s_state != state;
    s_state = state;
    xSemaphoreGive(s_mutex);
    if (changed) xTaskNotifyGive(s_task);
}

void led_status_set_title(const char *title) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
    xSemaphoreGive(s_mutex);
    xTaskNotifyGive(s_task);
}

bool led_status_display_info(int *width, int *height) {
    if (!s_ready) return false;
    *width = RLCD_W;
    *height = RLCD_H;
    return true;
}

int led_status_display_bits(void) {
    return s_ready ? 1 : 0;
}

// Gray level of a native RGB565 pixel, 0 (black) to 255 (white).
static inline uint8_t luma565(uint16_t px) {
    int r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
    r = r << 3 | r >> 2;
    g = g << 2 | g >> 4;
    b = b << 3 | b >> 2;
    return (uint8_t)((77 * r + 150 * g + 29 * b + 128) >> 8);
}

bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
    if (!s_ready || !pixels || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > RLCD_W || y + h > RLCD_H) {
        return false;
    }
    const uint8_t *src = (const uint8_t *)pixels;  // RGB565, high byte first
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_image_mode) {
        s_image_mode = true;
        memset(s_fb, 0x00, RLCD_FB_BYTES);
    }
    for (int r = 0; r < h; r++) {
        for (int i = 0; i < w; i++, src += 2) {
            // Simple threshold; the ST7305 refreshes in milliseconds, so no
            // dithering is needed the way e-paper needs it.
            fb_px(x + i, y + r, luma565((uint16_t)(src[0] << 8 | src[1])) < 128);
        }
    }
    xSemaphoreGive(s_lock);
    return true;
}

void led_status_draw_done(void) {
    if (!s_ready) return;
    xSemaphoreTake(s_panel_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool show = s_image_mode;
    if (show) {
        esp_err_t err = rlcd_flush();
        if (err != ESP_OK) ESP_LOGE(TAG, "image refresh failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_panel_lock);
}

void led_status_show_animation(void) {
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_image = s_image_mode;
    s_image_mode = false;
    xSemaphoreGive(s_lock);
    if (was_image) xTaskNotifyGive(s_task);
}

void led_status_set_voice(led_voice_t voice) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = s_voice != voice;
    s_voice = voice;
    xSemaphoreGive(s_mutex);
    if (changed) xTaskNotifyGive(s_task);
}

void led_status_set_level(float level) {
    if (!s_ready) return;
    int q = (int)(level * LEVEL_STEPS + 0.5f);
    if (q < 0) q = 0;
    if (q > LEVEL_STEPS) q = LEVEL_STEPS;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = s_level_q != q && s_voice == LED_VOICE_LISTENING;
    s_level_q = q;
    xSemaphoreGive(s_mutex);
    if (changed) xTaskNotifyGive(s_task);
}

void led_status_show_volume(int percent) {
    if (!s_ready) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_volume = percent;
    s_volume_until = esp_timer_get_time() + VOLUME_MS * 1000LL;
    xSemaphoreGive(s_mutex);
    xTaskNotifyGive(s_task);
}
