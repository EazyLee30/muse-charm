"""Validate commands before changing persistent hardware state."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class CharmCommandsTest(unittest.TestCase):
 def test_validation_theme_and_location(self):
  src=(ROOT/'main/charm.c').read_text();src=src[src.index('void charm_react'):src.index('static void task(')]
  prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "charm.h"
#include "charm_timezone.h"
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static charm_view_t s_view={.battery_pct=-1};
static int64_t s_reaction_at,s_key_at,s_boot_at;
static uint16_t s_buttons;
static int s_hat=-1,s_pad_request,s_toggle_request;
static int writes,fail_store;
static int64_t esp_timer_get_time(void) {return 1000000;}
static bool config_set_str(const char *k,const char *v) {(void)k;(void)v;writes++;return !fail_store;}
static bool config_setup_complete(void) {return true;}
static bool config_is_provisioned(void) {return true;}
static bool voice_say(const char *t) {(void)t;return true;}
static void voice_stop_music(void) {}
static bool voice_play_music(const char *s) {(void)s;return true;}
static bool charm_storage_path(const char *r,char *o,size_t n) {(void)r;(void)o;(void)n;return false;}
static cJSON *charm_sensors(void) {return cJSON_CreateObject();}
static cJSON *charm_storage_command(const char *c,cJSON *p) {(void)c;(void)p;return cJSON_CreateObject();}
void charm_ui_caption(const char *s) {(void)s;}
void charm_ui_rotate(int r) {(void)r;}
void charm_ui_set_dark(bool d) {(void)d;}
'''
  suffix=r'''
static cJSON *run(const char *cmd,const char *params) {cJSON *p=cJSON_Parse(params),*out=charm_command(cmd,p);cJSON_Delete(p);return out;}
static bool ok(cJSON *o) {bool success=cJSON_IsTrue(cJSON_GetObjectItem(o,"ok"));cJSON_Delete(o);return success;}
int main(void) {
 assert(ok(run("charm.configure","{\"mode\":\"dark\"}")));assert(s_view.dark && writes==1);
 assert(!ok(run("charm.configure","{\"mode\":\"light\",\"reaction\":\"invalid\"}")));assert(s_view.dark && writes==1);
 assert(ok(run("charm.configure","{\"mode\":\"toggle\",\"reaction\":\"dance\",\"location\":\"desk\"}")));
 assert(!s_view.dark && s_view.reaction==CHARM_DANCE && !strcmp(s_view.location,"desk"));
 assert(ok(run("charm.configure","{\"rotate\":\"left\"}")));assert(s_view.rotation==3);
 assert(ok(run("charm.configure","{\"rotate\":\"right\"}")));assert(s_view.rotation==0);
 assert(!ok(run("charm.configure","{\"rotate\":\"diagonal\"}")));assert(s_view.rotation==0);
 assert(!ok(run("charm.music","{\"action\":\"delete\"}")));
 assert(ok(run("charm.music","{\"action\":\"stop\"}")));
 assert(!ok(run("charm.configure","{\"mode\":\"dark\",\"timezone\":\"Unknown\"}")));assert(!s_view.dark);
 assert(ok(run("charm.configure","{\"timezone\":\"Asia/Shanghai\"}")));assert(!strcmp(s_view.timezone,"Asia/Shanghai"));
 fail_store=1;assert(!ok(run("charm.configure","{\"mode\":\"dark\"}")));assert(!s_view.dark);
 assert(!ok(run("charm.usage","{}")));
 cJSON *o=run("charm.status","{}");assert(cJSON_IsFalse(cJSON_GetObjectItem(o,"gps")));assert(!cJSON_GetObjectItem(o,"muse_quota_used_percent"));cJSON_Delete(o);
 assert(!ok(run("charm.speak","{\"text\":\"\"}")));assert(ok(run("charm.speak","{\"text\":\"你好\"}")));
}
'''
  lib=ROOT/'managed_components/espressif__cjson/cJSON'
  if not (lib/'cJSON.c').exists():self.skipTest('ESP-IDF build fetches cJSON first')
  with tempfile.TemporaryDirectory() as tmp:
   c=Path(tmp)/'test.c';b=Path(tmp)/'test';c.write_text(prefix+src+suffix)
   subprocess.run(['cc','-std=c11','-I',str(ROOT/'main'),'-I',str(lib),str(c),str(ROOT/'main/charm_timezone.c'),str(lib/'cJSON.c'),'-o',str(b)],check=True)
   subprocess.run([str(b)],check=True)
