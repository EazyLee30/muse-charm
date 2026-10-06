"""Check the real diagnostic emits nonzero PCM and restores the saved volume."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class SpeakerDiagnosticTest(unittest.TestCase):
 def test_pcm(self):
  source=(ROOT/'main/voice.c').read_text();a=source.index('static void speaker_test(void)');b=source.index('\nbool voice_speaker_test',a)
  prefix=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <assert.h>
#include <math.h>
#define CAPTURE_CHUNK 320
#define VOICE_PLAYER_RATE 16000
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
static atomic_int s_volume=37,s_audio_owner=2;
static int volume_calls,notes[2],frames,maximum,positive,negative,begun,ended,stopped;
static void voice_board_set_volume(int value) {notes[volume_calls++]=value;}
static void voice_player_begin(void) {begun++;}
static esp_err_t voice_player_write(const int16_t *pcm,size_t n) {
 frames+=n;
 for(size_t i=0;i<n;i++) {int x=pcm[i];positive+=x>0;negative+=x<0;if(abs(x)>maximum)maximum=abs(x);}
 return ESP_OK;
}
static void voice_player_end(void) {ended++;}
static bool voice_player_wait(int ms) {assert(ms==5000);return true;}
static void voice_player_stop(void) {stopped++;}
'''
  suffix=r'''
int main(void) {
 speaker_test();
 assert(frames==16000 && maximum>2400 && maximum<=2500);
 assert(positive>7000 && negative>7000);
 assert(begun==1 && ended==1 && stopped==0);
 assert(volume_calls==2 && notes[0]==60 && notes[1]==37);
 assert(atomic_load(&s_audio_owner)==0);
}
'''
  with tempfile.TemporaryDirectory() as tmp:
   c,binary=Path(tmp)/'tone.c',Path(tmp)/'tone'
   c.write_text('#include <stdlib.h>\n'+prefix+source[a:b]+suffix)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-lm','-o',str(binary)],check=True)
   subprocess.run([str(binary)],check=True)
