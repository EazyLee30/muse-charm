"""Runtime SDK setup is validated, persisted, and only applied at boot."""
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class SDKSetupTest(unittest.TestCase):
    def test_validation_persistence_and_boot_precedence(self):
        source=(ROOT/'main/identity.c').read_text()
        load=source[source.index('    // Load once'):source.index('    uint8_t mac')]
        functions=source[source.index('const char *identity_sdk_token(void)'):]
        prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#define CONFIG_GADGET_SDK_TOKEN "mgst_compile_time_test_only"
static char s_sdk_token[64],stored[64];
static bool fail_write;
bool identity_sdk_token_valid(const char *);
static bool config_get_str(const char *key,char *out,size_t n) {
 (void)key;if(!stored[0])return false;snprintf(out,n,"%s",stored);return true;
}
static bool config_set_str(const char *key,const char *value) {
 assert(!strcmp(key,"sdk_token"));if(fail_write)return false;strcpy(stored,value);return true;
}
'''
        suffix=r'''
int main(void) {
 assert(!identity_sdk_token_valid(NULL));assert(!identity_sdk_token_valid("mgst_short"));
 assert(!identity_sdk_token_valid("mgst_bad whitespace"));
 assert(!identity_sdk_token_valid("mgst_bad\nnewlines"));
 char long_key[65];memset(long_key,'a',64);memcpy(long_key,"mgst_",5);long_key[64]=0;
 assert(!identity_sdk_token_valid(long_key));
 assert(identity_sdk_token_valid("mgst_runtime_test_only"));
 boot_load();assert(!strcmp(identity_sdk_token(),CONFIG_GADGET_SDK_TOKEN));
 assert(!identity_sdk_token_save("invalid"));assert(!stored[0]);
 fail_write=true;assert(!identity_sdk_token_save("mgst_runtime_test_only"));
 fail_write=false;assert(identity_sdk_token_save("mgst_runtime_test_only"));
 assert(!strcmp(identity_sdk_token(),CONFIG_GADGET_SDK_TOKEN));
 boot_load();assert(!strcmp(identity_sdk_token(),"mgst_runtime_test_only"));
 assert(identity_sdk_token_save("mgst_replacement_test_only"));
 assert(!strcmp(identity_sdk_token(),"mgst_runtime_test_only"));
 boot_load();assert(!strcmp(identity_sdk_token(),"mgst_replacement_test_only"));
 strcpy(stored,"invalid");boot_load();assert(!strcmp(identity_sdk_token(),CONFIG_GADGET_SDK_TOKEN));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/'sdk.c';binary=Path(tmp)/'sdk'
            c.write_text(prefix+functions+'\nstatic void boot_load(void) {\n'+load+'}\n'+suffix)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
