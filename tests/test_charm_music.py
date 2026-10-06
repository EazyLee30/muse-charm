"""A spoken tool request queues music until its own voice reply releases audio."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class CharmMusicTest(unittest.TestCase):
 def test_deferred_play_stop_and_source_validation(self):
  source=(ROOT/'main/voice.c').read_text();source=source[source.index('void voice_stop_music(void)'):source.index('bool voice_say(const char *text)')]
  prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdTRUE 1
typedef enum {EVT_MUSIC} voice_evt_t;
static char s_pending_music[1024];
static atomic_bool s_ready=true,s_media_stop;
static atomic_int s_audio_owner;
static int s_events,requests,cancels;
static bool muse_tts_media(const char *s) {(void)s;requests++;return true;}
static void muse_tts_cancel(void) {cancels++;}
static void voice_player_stop(void) {}
static int xQueueSend(int q,void *e,int t) {(void)q;(void)e;(void)t;return pdTRUE;}
'''
  suffix=r'''
int main(void) {
 atomic_store(&s_audio_owner,1);
 assert(voice_play_music("https://example.org/song.mp3"));assert(requests==0);
 assert(!strcmp(s_pending_music,"https://example.org/song.mp3"));
 assert(!voice_play_music("file:///private/file.mp3"));assert(requests==0);
 play_pending_music();assert(requests==1 && atomic_load(&s_audio_owner)==3);assert(!s_pending_music[0]);
 voice_stop_music();assert(atomic_load(&s_media_stop) && cancels==1);
 atomic_store(&s_audio_owner,1);assert(voice_play_music("/sdcard/music/song.mp3"));
 voice_stop_music();assert(!s_pending_music[0] && cancels==1);
 play_pending_music();assert(requests==1 && atomic_load(&s_audio_owner)==0);
 assert(voice_play_music("/sdcard/music/song.mp3"));assert(requests==2);
 assert(!voice_play_music("https://example.org/second.mp3"));
}
'''
  with tempfile.TemporaryDirectory() as tmp:
   c=Path(tmp)/'music.c';binary=Path(tmp)/'music';c.write_text(prefix+source+suffix)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True)
   subprocess.run([str(binary)],check=True)
