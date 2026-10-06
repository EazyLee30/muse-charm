"""Verify repeated and partial frame transfers in both display polarities."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class RlcdRefreshTest(unittest.TestCase):
    def test_refresh_preserves_framebuffer(self):
        source = (ROOT / "main/rlcd42_status.c").read_text()
        start = source.index("static esp_err_t rlcd_flush(void)")
        end = source.index("// ---- Background task", start)
        prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define RLCD_W ((s_rotation&1)?300:400)
#define RLCD_H ((s_rotation&1)?400:300)
#define RLCD_ROW_BYTES ((RLCD_W+7)/8)
#define RLCD_FB_BYTES 15200
#define RLCD_TX_BYTES 15000
#define RLCD_ROW_UNITS 200
#define RLCD_COL_GROUPS 25
#define RLCD_CHUNK_BYTES 4096
#define RLCD_PIN_DC 5
#define RLCD_PIN_CS 40
#define ESP_OK 0
typedef int esp_err_t;
typedef struct { size_t length; const void *tx_buffer; } spi_transaction_t;
static int s_spi;
static int s_rotation;
#ifdef CONFIG_RLCD42_DARK_MODE
static bool s_dark=true;
#else
static bool s_dark=false;
#endif
static uint8_t s_fb[RLCD_FB_BYTES], s_tx[RLCD_TX_BYTES];
static int fail_transfer;
static esp_err_t rlcd_cmd_data(uint8_t cmd, const uint8_t *p, size_t n) {
    (void)cmd; (void)p; (void)n; return ESP_OK;
}
static esp_err_t rlcd_cmd(uint8_t cmd) { (void)cmd; return ESP_OK; }
static void gpio_set_level(int pin, int level) { (void)pin; (void)level; }
static esp_err_t spi_device_polling_transmit(int spi, const spi_transaction_t *t) {
    (void)spi; (void)t; return fail_transfer;
}
'''
        suffix = r'''
static unsigned panel_pixel(int x, int y) {
    int col = 299-y, row = x/2;
    int index = (row*25+col/12)*3+(col%12)/4;
    int shift = 6-2*(col%4)+(x%2 ? 0 : 1);
    return (s_tx[index] >> shift)&1;
}
int main(void) {
    uint8_t original[RLCD_FB_BYTES], transfer[RLCD_TX_BYTES];
    for (int i=0; i<RLCD_FB_BYTES; ++i) s_fb[i]=(uint8_t)(i*37);
    memcpy(original, s_fb, sizeof(s_fb));
    assert(rlcd_flush()==ESP_OK);
    assert(memcmp(original, s_fb, sizeof(s_fb))==0);
    for(s_rotation=0;s_rotation<4;s_rotation++) {
      assert(rlcd_flush()==ESP_OK);
      for (int y=0;y<RLCD_H;++y) for (int x=0;x<RLCD_W;++x) {
        unsigned expected=(s_fb[y*RLCD_ROW_BYTES+x/8]>>(7-x%8))&1;
        if(s_dark) expected^=1;
        int sx=x,sy=y;
        if(s_rotation==1) {sx=399-y;sy=x;}
        if(s_rotation==2) {sx=399-x;sy=299-y;}
        if(s_rotation==3) {sx=y;sy=299-x;}
        assert(panel_pixel(sx,sy)==expected);
      }
      assert(memcmp(original,s_fb,sizeof(s_fb))==0);
    }
    s_rotation=0;assert(rlcd_flush()==ESP_OK);
    memcpy(transfer, s_tx, sizeof(s_tx));
    assert(rlcd_flush()==ESP_OK);
    assert(memcmp(original, s_fb, sizeof(s_fb))==0);
    assert(memcmp(transfer, s_tx, sizeof(s_tx))==0);
    s_dark=!s_dark;
    assert(rlcd_flush()==ESP_OK);
    assert(memcmp(original,s_fb,sizeof(s_fb))==0);
    s_fb[0]^=0x80; /* A later partial image draw. */
    unsigned before=panel_pixel(0,0);
    assert(rlcd_flush()==ESP_OK);
    assert(panel_pixel(0,0)==(before^1));
    memcpy(original, s_fb, sizeof(s_fb));
    fail_transfer=1;
    assert(rlcd_flush()!=ESP_OK);
    assert(memcmp(original, s_fb, sizeof(s_fb))==0);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
            src.write_text(prefix + source[start:end] + suffix)
            for dark in (False, True):
                with self.subTest(dark=dark):
                    command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror"]
                    if dark:
                        command.append("-DCONFIG_RLCD42_DARK_MODE=1")
                    subprocess.run([*command, str(src), "-o", str(binary)], check=True)
                    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
