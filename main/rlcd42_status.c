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
#include "muse_pixel.h"
#include "stack_monitor.h"
#include "charm.h"
#include "charm_font.h"

static volatile bool s_dark = true;
static int s_rotation;
static volatile int s_requested_rotation;
void charm_ui_rotate(int rotation) {s_requested_rotation=rotation&3;}
static portMUX_TYPE s_caption_lock=portMUX_INITIALIZER_UNLOCKED;
static char s_caption[512];
static int64_t s_caption_until;
void charm_ui_caption(const char *text) {
    portENTER_CRITICAL(&s_caption_lock);
    snprintf(s_caption,sizeof(s_caption),"%s",text?text:"");
    s_caption_until=esp_timer_get_time()+15000000;
    portEXIT_CRITICAL(&s_caption_lock);
}
void charm_ui_set_dark(bool dark) {s_dark=dark;}

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
#define RLCD_W         ((s_rotation&1)?300:400)
#define RLCD_H         ((s_rotation&1)?400:300)
#define RLCD_ROW_BYTES ((RLCD_W + 7) / 8)
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

// ---- Layout shared by every orientation ----------------------------------
#define FRAME_MS 200
#define AVATAR_MAX 240
static struct {int size,x,y,cx,cy,bubble_y;} s_layout;
static bool caption_visible(void) {return s_caption[0] && esp_timer_get_time()<s_caption_until;}
static void layout_frame(void) {
    bool portrait=RLCD_H>RLCD_W,caption=caption_visible();
    int bottom=s_rotation==2?30:10;
    s_layout.bubble_y=RLCD_H-bottom-70;
    s_layout.size=portrait?240:caption?(s_rotation==2?184:200):232;
    s_layout.x=(RLCD_W-s_layout.size)/2;
    s_layout.y=portrait?72:caption?28:38;
    s_layout.cx=RLCD_W/2;s_layout.cy=s_layout.y+s_layout.size/2;
}

// Official Muse renderer, with room to wander and a palette for paper.
static void draw_muse(led_state_t state, led_voice_t voice, int level_q) {
    muse_mode_t mode = MUSE_MODE_IDLE;
    if (state == LED_STATE_BOOT) mode = MUSE_MODE_BOOT;
    if (state == LED_STATE_ERROR || voice == LED_VOICE_ERROR) mode = MUSE_MODE_ERROR;
    else if (voice == LED_VOICE_LISTENING) mode = MUSE_MODE_LISTENING;
    else if (voice == LED_VOICE_THINKING || voice == LED_VOICE_TRANSCRIBING) mode = MUSE_MODE_THINKING;
    else if (voice == LED_VOICE_SPEAKING || voice == LED_VOICE_BUFFERING) mode = MUSE_MODE_SPEAKING;
    static muse_mode_t last = MUSE_MODE_COUNT;
    static int64_t changed_at;
    int64_t now = esp_timer_get_time();
    if (mode != last) { last = mode; changed_at = now; }
    muse_pose_t pose = {.mode=mode,.t=now/1000000.0f,.mode_t=(now-changed_at)/1000000.0f,.level=level_q/(float)LEVEL_STEPS};
    charm_view_t view;charm_snapshot(&view);
    pose.happy=view.reaction==CHARM_PET;
    int size=s_layout.size;
    float wander=view.reaction==CHARM_CALM && voice==LED_VOICE_IDLE?sinf(pose.t*.55f):0;
    int dx=view.offset_x+(int)(wander*(RLCD_W>RLCD_H?32:14));
    if(view.reaction==CHARM_DANCE) dx+=(int)(15*sinf(pose.t*3));
    if(dx>28 && RLCD_H>RLCD_W) dx=28;
    if(dx< -28 && RLCD_H>RLCD_W) dx=-28;
    if(view.reaction==CHARM_PEEK) dx+=(int)((RLCD_W>RLCD_H?58:24)*sinf(pose.t*1.3f));
    int dy=view.reaction==CHARM_DANCE?(int)(8*sinf(pose.t*7)):(int)(2*sinf(pose.t*2));
    if(view.reaction==CHARM_HOP) dy-=(int)(22*fabsf(sinf(pose.t*4)));
    float stretch=view.reaction==CHARM_STRETCH?1+.10f*sinf(pose.t*2):1;
    muse_pixel_set_size(size);muse_pixel_render(&pose);
    static const uint8_t bayer[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    uint16_t row[AVATAR_MAX];uint8_t paper[AVATAR_MAX];
    for(int y=0;y<size;y++) {
        muse_pixel_scale(row,size,0,size-1,y,y);
        if(!view.dark) muse_pixel_scale_paper(paper,size,y);
        for(int x=0;x<size;x++) {
            uint16_t c=row[x];unsigned r=((c>>11)&31)*255/31,g=((c>>5)&63)*255/63,b=(c&31)*255/31;
            unsigned lum=(77*r+150*g+29*b)>>8;
            fb_px(s_layout.x+x+dx,s_layout.y+size/2+(int)((y-size/2)*stretch)+dy,(view.dark?lum:paper[x])>(unsigned)bayer[y&3][x&3]*16+8);
        }
    }
}

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
static void fb_round_rect(int x,int y,int w,int h,int radius,bool ink) {
    fb_rect(x+radius,y,w-2*radius,h,ink);fb_rect(x,y+radius,w,h-2*radius,ink);
    fb_circle(x+radius,y+radius,radius,ink);fb_circle(x+w-radius-1,y+radius,radius,ink);
    fb_circle(x+radius,y+h-radius-1,radius,ink);fb_circle(x+w-radius-1,y+h-radius-1,radius,ink);
}
static void draw_caption(void) {
    char text[512];int64_t until;
    portENTER_CRITICAL(&s_caption_lock);memcpy(text,s_caption,sizeof(text));until=s_caption_until;portEXIT_CRITICAL(&s_caption_lock);
    if(!text[0] || esp_timer_get_time()>until) return;
    int bx=RLCD_H>RLCD_W?25:20,by=s_layout.bubble_y,bw=RLCD_W-2*bx;
    // Soft rounded outline, breathing room and a small conversational tail.
    fb_round_rect(bx+2,by+2,bw,68,10,true);
    fb_round_rect(bx,by,bw,68,10,true);fb_round_rect(bx+1,by+1,bw-2,66,9,false);
    for(int i=0;i<7;i++) fb_hline(bx+24+i,by-6+i,2,true);
    fb_rect(bx+31,by,8,2,false);
    int x=bx+12,y=by+9;size_t pos=0;
    while(text[pos]) {
        uint32_t cp=(unsigned char)text[pos++];
        if(cp>=0xc0) {int extra=cp<0xe0?1:cp<0xf0?2:3;cp&=(1u<<(6-extra))-1;
            for(int i=0;i<extra;i++) {if(((unsigned char)text[pos]&0xc0)!=0x80) {cp='?';break;}cp=(cp<<6)|((unsigned char)text[pos++]&63);}}
        int width=charm_font_advance(cp);
        if(cp=='\n' || x+width>bx+bw-12) {x=bx+12;y+=24;if(cp=='\n') continue;}
        if(y+CHARM_FONT_H>by+61) {fb_text("...",bx+bw-28,by+56,1);break;}
        const uint8_t *g=charm_font_glyph(cp);
        if(g) {for(int yy=0;yy<CHARM_FONT_H;yy++) for(int xx=0;xx<CHARM_FONT_W;xx++)
            if(g[yy*CHARM_FONT_STRIDE+xx/8]&(128>>(xx%8))) fb_px(x+xx,y+yy,true);}
        else {fb_rect(x+2,y+4,12,14,true);fb_rect(x+3,y+5,10,12,false);}
        x+=width;
    }
}

// Hardware button positions viewed from the front: KEY left of PWR, BOOT right.
// Keep the cue on the same physical edge as the key as the screen turns.
static void draw_button_cue(int physical_x,const char *label,bool pressed) {
    int width=text_width(label,1)+12,x=physical_x,y=0;
    if(s_rotation==1) {x=0;y=399-physical_x;}
    if(s_rotation==2) {x=399-physical_x;y=RLCD_H-1;}
    if(s_rotation==3) {x=RLCD_W-1;y=physical_x;}
    if(s_rotation==0 || s_rotation==2) {
        int by=s_rotation==0?6:RLCD_H-23;
        fb_round_rect(x-width/2,by,width,17,5,true);
        if(!pressed) fb_round_rect(x-width/2+1,by+1,width-2,15,4,false);
        // Text stays solid; a filled rail and bouncing dot communicate pressure.
        if(pressed) fb_rect(x-width/2+5,by+4,width-10,9,false);
        fb_text(label,x-width/2+6,by+5,1);
        fb_rect(x-12,s_rotation==0?0:RLCD_H-(pressed?3:1),24,pressed?3:1,true);
    } else {
        int bx=s_rotation==1?4:RLCD_W-width-4,by=y-8;
        fb_round_rect(bx,by,width,17,5,true);
        if(!pressed) fb_round_rect(bx+1,by+1,width-2,15,4,false);
        if(pressed) fb_rect(bx+5,by+4,width-10,9,false);
        // Keep letters upright in logical space; panel rotation turns them too.
        fb_text(label,bx+6,by+5,1);
        fb_rect(s_rotation==1?0:RLCD_W-(pressed?3:1),y-12,pressed?3:1,24,true);
    }
}

static void draw_header(charm_view_t *v,led_state_t state) {
    fb_text(v->clock[0]?v->clock:"--:--",8,10,1);
    char battery[8];snprintf(battery,sizeof(battery),v->battery_pct>=0?"%d%%":"--%%",v->battery_pct);
    int bw=text_width(battery,1),right=RLCD_W-8;
    fb_rect(right-14,9,12,9,true);fb_rect(right-13,10,10,7,false);fb_rect(right-2,12,2,3,true);
    if(v->battery_pct>=0) fb_rect(right-12,11,8*v->battery_pct/100,5,true);
    fb_text(battery,right-19-bw,10,1);
    int signal=right-27-bw;
    for(int i=0;i<3;i++) fb_rect(signal-12+i*4,16-i*3,3,2+i*3,v->wifi);
    fb_circle(signal-20,13,2,state==LED_STATE_WS_CONNECTED);
    if(v->pad_connected || v->pad_pairing) fb_text(v->pad_connected?"B":"?",signal-31,10,1);
}

static void render_status(const char *title,led_state_t state,led_voice_t voice,int level_q) {
    (void)title;memset(s_fb,0,RLCD_FB_BYTES);layout_frame();
    charm_view_t v;charm_snapshot(&v);float t=esp_timer_get_time()/1000000.0f;
    draw_header(&v,state);
    const char *vl=voice_label(voice);
    const char *reaction=v.reaction==CHARM_HOP?"Boing!":v.reaction==CHARM_PEEK?"Peek-a-boo":v.reaction==CHARM_STRETCH?"Big stretch":v.reaction==CHARM_DANCE?"Dance with me":v.reaction==CHARM_SLEEP?"Sweet dreams":NULL;
    draw_muse(state,voice,level_q);
    fb_text_centered(vl?vl:reaction?reaction:conn_label(state),34,1);
    int cx=s_layout.cx,cy=s_layout.cy;
    if(voice==LED_VOICE_THINKING || voice==LED_VOICE_TRANSCRIBING) {
        for(int i=0;i<3;i++) {float a=t*2+i*2.0944f;int x=cx+(int)((s_layout.size*.43f)*cosf(a)),y=cy+(int)(52*sinf(a));
            fb_rect(x-2,y-5,4,10,true);fb_rect(x-5,y-2,10,4,true);}
    }
    if(voice==LED_VOICE_SPEAKING || v.reaction==CHARM_DANCE) {
        int spread=RLCD_W>RLCD_H?110:96,dy=(int)(5*sinf(t*5));
        fb_rect(cx-spread,cy+dy,2,24,true);fb_circle(cx-spread-4,cy+25+dy,4,true);fb_rect(cx-spread,cy+dy,12,3,true);
        fb_rect(cx+spread,cy+12-dy,2,22,true);fb_circle(cx+spread-4,cy+35-dy,4,true);
    }
    if(v.reaction==CHARM_PET) for(int i=0;i<3;i++) {
        int spread=RLCD_W>RLCD_H?102:84,x=cx-spread+i*spread,y=cy-38-(int)(v.reaction_t*10)%30;
        fb_circle(x-3,y,4,true);fb_circle(x+3,y,4,true);for(int j=0;j<8;j++) fb_hline(x-7+j,y+j,15-2*j,true);
    }
    if(v.reaction==CHARM_WAVE) {
        int x=cx+(RLCD_W>RLCD_H?104:92),y=cy+(int)(8*sinf(t*8));fb_circle(x,y,7,true);
        fb_rect(x-6,y-24,2,19,true);fb_rect(x,y-27,2,22,true);fb_rect(x+6,y-22,2,17,true);
    }
    if(v.reaction==CHARM_SLEEP) fb_text("Z z",cx+68,cy-58+(int)(3*sinf(t)),2);
    if(voice==LED_VOICE_LISTENING) {
        int base=s_layout.bubble_y-8;
        for(int i=0;i<LEVEL_STEPS;i++) {int h=i<level_q?5+(int)(5*fabsf(sinf(t*8+i))):2;fb_rect(cx-34+i*7,base-h,4,h,true);}
    }
    draw_caption();
    draw_button_cue(156,voice==LED_VOICE_LISTENING?"KEY REC":"KEY HOLD",v.key_pressed);
    draw_button_cue(244,"BOOT",v.boot_pressed);
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
    const bool dark=s_dark;
    for (int ru = 0; ru < RLCD_ROW_UNITS; ru++) {
        for (int cg = 0; cg < RLCD_COL_GROUPS; cg++) {
            uint8_t *out = s_tx + ((size_t)ru * RLCD_COL_GROUPS + cg) * 3;
            for (int b = 0; b < 3; b++) {
                unsigned v = 0;
                for (int p = 0; p < 4; p++) {
                    int bx = cg * 12 + b * 4 + p;  // panel column 0..299
                    // R1: panel (bx, by) shows fb (x, y) = (by, 299 - bx).
                    int even=0,odd=0;
                    for(int pair=0;pair<2;pair++) {
                        int px=ru*2+pair,py=299-bx,x=px,y=py;
                        if(s_rotation==1) {x=py;y=399-px;}
                        if(s_rotation==2) {x=399-px;y=299-py;}
                        if(s_rotation==3) {x=299-py;y=px;}
                        int ink=(s_fb[(size_t)y*RLCD_ROW_BYTES+x/8]>>(7-x%8))&1;
                        if(pair) odd=ink;else even=ink;
                    }
                    v = (v << 2) | (unsigned)(even ? 2 : 0) | (unsigned)(odd ? 1 : 0);
                }
                out[b] = (uint8_t)(dark ? v ^ 0xFF : v);
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
        TickType_t wait = pdMS_TO_TICKS(FRAME_MS);
        int64_t now = esp_timer_get_time();
        if (until > now && until - now < FRAME_MS * 1000LL)
            wait = pdMS_TO_TICKS((uint32_t)((until - now) / 1000) + 5);
        ulTaskNotifyTake(pdTRUE, wait);

        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if(s_rotation!=s_requested_rotation) {s_rotation=s_requested_rotation;s_image_mode=false;}
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
    s_fb = heap_caps_malloc(15200, MALLOC_CAP_SPIRAM);
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
