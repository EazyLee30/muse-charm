"""Exercise serial chat chunking, cancellation and allocation/size failures."""
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
class RlcdConsoleTest(unittest.TestCase):
    def test_chat_commands(self):
        s = (ROOT/'main/rlcd42_console.c').read_text()
        a,b = s.index('static void command('),s.index('static void console_task(')
        prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct {} cJSON;
static cJSON *cJSON_Parse(const char *s) {(void)s;return NULL;}
static cJSON *cJSON_GetObjectItem(cJSON *o,const char *s) {(void)o;(void)s;return NULL;}
static const char *cJSON_GetStringValue(cJSON *o) {(void)o;return NULL;}
static void cJSON_Delete(cJSON *o) {(void)o;}
static char *cJSON_PrintUnformatted(cJSON *o) {(void)o;return NULL;}
static bool config_set_str(const char *k,const char *v) {(void)k;(void)v;return false;}
static const char *identity_sdk_token(void) {return NULL;}
static bool identity_sdk_token_save(const char *s) {(void)s;return false;}
static bool config_get_str(const char *k,char *v,size_t n) {(void)k;(void)v;(void)n;return false;}
static void esp_restart(void) {}
static void vTaskDelay(int t) {(void)t;}
#define pdMS_TO_TICKS(x) (x)
static bool muse_tts_configure(const char *k) {(void)k;return false;}
static cJSON *charm_command(const char *c,cJSON *p) {(void)c;(void)p;return NULL;}
#define CHAT_MAX 32
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static char *s_message;
static size_t s_length;
static int errors, turns, cancels, acks, oom;
static char result[32];
static void status(void) {}
static bool voice_speaker_test(void) {return true;}
static void *heap_caps_malloc(size_t n,int cap) {(void)cap;return oom?NULL:malloc(n);}
static size_t muse_hatch_unescape(char *s) {return strlen(s);}
static void muse_hatch_text_cancel(void) {cancels++;}
static void muse_hatch_text_turn(char *s) {turns++;strcpy(result,s);free(s);}
static void muse_hatch_console(const char *type,const char *s,const char *fields,...) {
    (void)s;(void)fields;errors+=!strcmp(type,"error");acks+=!strcmp(type,"ack");
}
'''
        suffix = r'''
static void run(const char *text,bool whole) {char line[128];strcpy(line,text);command(line,whole);}
int main(void) {
 run("chat+=你好",true);run("chat= Muse",true);
 assert(turns==1 && !strcmp(result,"你好 Muse") && acks==2 && !s_message);
 run("chat+=discard",true);run("chat.cancel",true);
 assert(cancels==1 && !s_message && s_length==0);
 run("chat=ok",true);assert(turns==2 && !strcmp(result,"ok"));
 run("chat+=prefix",true);run("chat=truncated",false);
 assert(errors==1 && turns==2 && !s_message);
 run("chat=12345678901234567890123456789012",true);
 assert(errors==2 && !s_message);
 oom=1;run("chat=hello",true);assert(errors==3 && !s_message);
 oom=0;run("chat=recovered",true);assert(turns==3 && !strcmp(result,"recovered"));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c,binary=Path(tmp)/'console.c',Path(tmp)/'console'
            c.write_text(prefix+s[a:b]+suffix)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
