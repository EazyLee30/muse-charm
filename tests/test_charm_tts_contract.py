"""Check native MiniMax/Qwen payloads, URLs, bounds and service errors."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class TTSContractTest(unittest.TestCase):
 def test_provider_payloads_and_response_validation(self):
  lib=ROOT/'managed_components/espressif__cjson/cJSON'
  if not (lib/'cJSON.c').exists():self.skipTest('Build fetches cJSON first')
  code=r'''
#include <assert.h>
#include <string.h>
#include "muse_tts_config.h"
int main(void) {
 muse_tts_config_t c;
 assert(muse_tts_options(&c,NULL,NULL,NULL));assert(!strcmp(c.provider,"qwen"));
 cJSON *r=muse_tts_request(&c,"你好");
 assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(r,"input"),"voice")),"longanhuan_v3.6"));cJSON_Delete(r);
 assert(!muse_tts_options(&c,"unknown",NULL,NULL));
 assert(!muse_tts_options(&c,"minimax-cn","made-up-model",NULL));
 assert(!muse_tts_options(&c,"minimax-cn",NULL,"bad\nvoice"));
 assert(muse_tts_options(&c,"minimax-cn",NULL,NULL));
 assert(!strcmp(muse_tts_endpoint(&c),"https://api.minimax.cn/v1/t2a_v2"));
 r=muse_tts_request(&c,"你好");
 assert(cJSON_IsFalse(cJSON_GetObjectItem(r,"stream")));
 assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(r,"output_format")),"url"));
 assert(cJSON_GetObjectItem(cJSON_GetObjectItem(r,"audio_setting"),"sample_rate")->valueint==16000);
 assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(r,"audio_setting"),"format")),"mp3"));cJSON_Delete(r);
 r=cJSON_Parse("{\"base_resp\":{\"status_code\":0},\"data\":{\"audio\":\"https://audio.example/song.mp3\"}}");
 assert(muse_tts_audio_url(&c,r));cJSON_Delete(r);
 r=cJSON_Parse("{\"base_resp\":{\"status_code\":1004},\"data\":{\"audio\":\"https://audio.example/song.mp3\"}}");
 assert(!muse_tts_audio_url(&c,r));cJSON_Delete(r);
 r=cJSON_Parse("{\"base_resp\":{\"status_code\":0},\"data\":{\"audio\":\"ffe300deadbeef\"}}");
 assert(!muse_tts_audio_url(&c,r));cJSON_Delete(r);
 assert(!muse_tts_audio_url(&c,NULL));
 assert(muse_tts_options(&c,"minimax-global","speech-2.8-hd","CustomVoiceID"));
 assert(!strcmp(muse_tts_endpoint(&c),"https://api.minimax.io/v1/t2a_v2"));
 assert(muse_tts_options(&c,"qwen",NULL,NULL));
 r=cJSON_Parse("{\"output\":{\"audio\":{\"url\":\"http://audio.example/song.mp3\"}}}");
 assert(muse_tts_audio_url(&c,r));cJSON_Delete(r);
}
'''
  with tempfile.TemporaryDirectory() as tmp:
   c=Path(tmp)/'tts.c';binary=Path(tmp)/'tts';c.write_text(code)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'components/muse'),'-I',str(lib),str(c),str(ROOT/'components/muse/muse_tts_config.c'),str(lib/'cJSON.c'),'-o',str(binary)],check=True)
   subprocess.run([str(binary)],check=True)
