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
#define RLCD_W 400
#define RLCD_H 300
#define RLCD_ROW_BYTES 50
#define RLCD_FB_BYTES 15000
#define LEVEL_STEPS 10
static uint8_t s_fb[15000];
static int64_t fake_time;
static int64_t esp_timer_get_time(void) { return fake_time; }
'''
        suffix = r'''
int main(void) {
    uint8_t previous[15000];
    fake_time=1000000; render_status("Muse",LED_STATE_WS_CONNECTED,LED_VOICE_IDLE,0);
    memcpy(previous,s_fb,sizeof(previous));
    int changes=0;
    for(int i=1;i<30;i++) {
        fake_time+=200000; render_status("Muse",LED_STATE_WS_CONNECTED,LED_VOICE_IDLE,0);
        changes+=memcmp(previous,s_fb,sizeof(previous))!=0;
        memcpy(previous,s_fb,sizeof(previous));
        /* Animation stays inside the avatar region, with empty side margins. */
        for(int y=108;y<284;y++) for(int x=0;x<112;x++)
            assert(((s_fb[y*50+x/8]>>(7-x%8))&1)==0);
    }
    assert(changes>20);
    render_status("Muse",LED_STATE_WS_CONNECTED,LED_VOICE_LISTENING,8);
    assert(memcmp(previous,s_fb,sizeof(previous))!=0);
    memcpy(previous,s_fb,sizeof(previous));
    render_status("Muse",LED_STATE_WS_CONNECTED,LED_VOICE_SPEAKING,5);
    assert(memcmp(previous,s_fb,sizeof(previous))!=0);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c, binary = Path(tmp)/"avatar.c", Path(tmp)/"avatar"
            c.write_text(prefix + source[start:end] + suffix)
            subprocess.run(["cc", "-std=c11", "-I", str(ROOT/"main"), "-I", str(ROOT/"components/muse"), str(c), str(ROOT/"avatar/muse_pixel.c"), str(ROOT/"main/pixel_font.c"), "-lm", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
