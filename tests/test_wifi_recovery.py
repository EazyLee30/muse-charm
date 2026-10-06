"""Exercise AP fallback against driver success and failure responses."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class WifiRecoveryTest(unittest.TestCase):
    def test_missing_ap_fallback(self):
        source = (ROOT / "main/wifi_mgr.c").read_text()
        start = source.index("static bool release_missing_bssid(")
        end = source.index("\n#if CONFIG_MUSE_ENABLED", start)
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define WIFI_REASON_NO_AP_FOUND 201
#define WIFI_IF_STA 0
#define WIFI_ALL_CHANNEL_SCAN 1
#define WIFI_CONNECT_AP_BY_SIGNAL 1
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
typedef struct { struct {
    bool bssid_set;
    uint8_t bssid[6];
    uint8_t channel;
    int scan_method, sort_method;
    char ssid[32], password[64];
} sta; } wifi_config_t;
static wifi_config_t saved;
static int get_error, set_error, writes;
static int esp_wifi_get_config(int iface, wifi_config_t *out) {
    (void)iface; *out = saved; return get_error;
}
static int esp_wifi_set_config(int iface, const wifi_config_t *in) {
    (void)iface; writes++;
    if (!set_error) saved = *in;
    return set_error;
}
'''
        harness += source[start:end]
        harness += r'''
int main(void) {
    strcpy(saved.sta.ssid, "mesh");
    strcpy(saved.sta.password, "secret");
    saved.sta.bssid_set = true;
    memset(saved.sta.bssid, 0xAB, 6);
    saved.sta.channel = 6;
    assert(!release_missing_bssid(202)); /* Auth failure must keep AP. */
    assert(writes == 0);
    get_error = 1;
    assert(!release_missing_bssid(201));
    assert(writes == 0);
    get_error = 0; set_error = 1;
    assert(!release_missing_bssid(201));
    assert(saved.sta.bssid_set);
    set_error = 0;
    assert(release_missing_bssid(201));
    assert(!saved.sta.bssid_set && saved.sta.channel == 0);
    const uint8_t zero[6] = {0};
    assert(memcmp(saved.sta.bssid, zero, 6) == 0);
    assert(saved.sta.scan_method == WIFI_ALL_CHANNEL_SCAN);
    assert(saved.sta.sort_method == WIFI_CONNECT_AP_BY_SIGNAL);
    assert(strcmp(saved.sta.ssid, "mesh") == 0);
    assert(strcmp(saved.sta.password, "secret") == 0);
    writes = 0;
    assert(!release_missing_bssid(201)); /* Already unpinned. */
    assert(writes == 0);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
            src.write_text(harness)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(src), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
