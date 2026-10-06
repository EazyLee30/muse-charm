"""Run the real official avatar adapter and check animation and clipping."""
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
class RlcdAvatarTest(unittest.TestCase):
    def test_animation_and_voice_states(self):
        source = (ROOT / "main/rlcd42_status.c").read_text()
        start = source.index("static inline void fb_px")
        end = source.index("// ---- Panel flush", start)
        prefix = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "led_status.h"
#include "muse_pixel.h"
#include "pixel_font.h"
typedef enum {CHARM_CALM,CHARM_PET,CHARM_DANCE,CHARM_WAVE,CHARM_SLEEP,CHARM_HOP,CHARM_PEEK,CHARM_STRETCH} charm_reaction_t;
typedef struct {bool dark,wifi,pad_connected,pad_pairing,key_pressed,boot_pressed;char clock[6];int battery_pct,offset_x;charm_reaction_t reaction;float reaction_t;} charm_view_t;
static bool test_dark,test_key;
static void charm_snapshot(charm_view_t *v) {*v=(charm_view_t){.dark=test_dark,.key_pressed=test_key,.wifi=true,.battery_pct=58,.clock="10:23"};}
#include "charm_font.h"
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static char s_caption[512];
static int64_t s_caption_until;
#define RLCD_W ((s_rotation&1)?300:400)
#define RLCD_H ((s_rotation&1)?400:300)
#define RLCD_ROW_BYTES ((RLCD_W+7)/8)
#define RLCD_FB_BYTES (RLCD_ROW_BYTES*RLCD_H)
#define LEVEL_STEPS 10
static int s_rotation;
static struct {uint64_t head;uint8_t fb[15200];uint64_t tail;} s_store;
#define s_fb s_store.fb
static int64_t fake_time;
static int64_t esp_timer_get_time(void) { return fake_time; }
'''
        suffix = r'''
int main(void) {
    assert(charm_font_init());
    uint8_t previous[15200];
    for(int dark=0;dark<2;dark++) for(s_rotation=0;s_rotation<4;s_rotation++) {
    test_dark=dark;test_key=false;s_caption[0]=0;
    s_store.head=s_store.tail=0xabcdef0123456789ULL;
    fake_time=1000000; render_status("Muse",LED_STATE_WS_CONNECTED,LED_VOICE_IDLE,0);
    memcpy(previous,s_fb,sizeof(previous));
    int changes=0;
    for(int i=1;i<30;i++) {
        fake_time+=200000; render_status("Muse",LED_STATE_WS_CONNECTED,LED_VOICE_IDLE,0);
        changes+=memcmp(previous,s_fb,sizeof(previous))!=0;
        /* Avatar motion never overwrites the compact header or key hints. */
        assert(memcmp(previous,s_fb,30*RLCD_ROW_BYTES)==0);
        memcpy(previous,s_fb,sizeof(previous));
    }
    assert(changes>20);
    render_status("Muse",LED_STATE_WS_CONNECTED,LED_VOICE_LISTENING,8);
    assert(memcmp(previous,s_fb,sizeof(previous))!=0);
    memcpy(previous,s_fb,sizeof(previous));
    render_status("Muse",LED_STATE_WS_CONNECTED,LED_VOICE_SPEAKING,5);
    assert(memcmp(previous,s_fb,sizeof(previous))!=0);
    strcpy(s_caption,"你好，今天陪我跳支舞。Hello!");s_caption_until=999999999;
    render_status("Muse",LED_STATE_WS_CONNECTED,LED_VOICE_SPEAKING,5);
    int x=156,y=2;
    if(s_rotation==1) {x=2;y=243;}
    if(s_rotation==2) {x=243;y=297;}
    if(s_rotation==3) {x=297;y=156;}
    assert(((s_fb[y*RLCD_ROW_BYTES+x/8]>>(7-x%8))&1)==0);
    test_key=true;render_status("Muse",LED_STATE_WS_CONNECTED,LED_VOICE_SPEAKING,5);
    assert(((s_fb[y*RLCD_ROW_BYTES+x/8]>>(7-x%8))&1)==1);
    assert(s_store.head==0xabcdef0123456789ULL && s_store.tail==s_store.head);
    }
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c, binary = Path(tmp)/"avatar.c", Path(tmp)/"avatar"
            c.write_text(prefix + source[start:end] + suffix)
            subprocess.run(["cc", "-std=c11", "-I", str(ROOT/"main"), "-I", str(ROOT/"components/muse"), str(c), str(ROOT/"avatar/muse_pixel.c"), str(ROOT/"main/pixel_font.c"), str(ROOT/"main/charm_font.c"), "-lm", "-lz", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
