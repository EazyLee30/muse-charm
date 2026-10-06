"""Verify the separate KEY emits one voice press/release and ignores unready holds."""
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class RlcdKeyTest(unittest.TestCase):
    def test_key_edges(self):
        s=(ROOT/'main/button.c').read_text()
        a=s.index('static void voice_key_poll(void)')
        b=s.index('\n#endif',a)
        prefix=r'''
#include <stdbool.h>
#include <assert.h>
#define VOICE_KEY_GPIO 18
#define ESP_LOGI(...) ((void)0)
typedef bool (*button_press_cb)(bool);
static button_press_cb s_press_cb;
static int level=1, presses, releases;
static bool ready;
static int gpio_get_level(int pin) { assert(pin==18);return level; }
static bool callback(bool down) {if(down) {presses++;return ready;} releases++;return true;}
'''
        suffix=r'''
int main(void) {
 voice_key_poll();level=0;voice_key_poll();level=1;voice_key_poll();
 assert(presses==0 && releases==0);
 s_press_cb=callback;
 level=0;voice_key_poll();voice_key_poll();level=1;voice_key_poll();
 assert(presses==1 && releases==0);
 ready=true;level=0;voice_key_poll();
 for(int i=0;i<150;i++) voice_key_poll();
 assert(presses==2 && releases==0);
 level=1;voice_key_poll();voice_key_poll();assert(releases==1);
 level=0;voice_key_poll();level=1;voice_key_poll();assert(presses==3 && releases==2);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c,binary=Path(tmp)/'key.c',Path(tmp)/'key'
            c.write_text(prefix+s[a:b]+suffix)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
