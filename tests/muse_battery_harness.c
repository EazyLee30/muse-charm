/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Drives the battery meter (muse_battery.c) for test_muse_battery.py, one command a line:
 *   t <us>                 the clock
 *   dump ... end           the lines esp_pm_dump_locks() prints from now on
 *   p <usb> <pct> <mv>     a battery reading
 *   s <off> <resting>      the screen and CPU state
 *   w <us>                 a wake from light sleep, after <us> asleep
 *   x                      muse_battery_reset()
 *   j                      prints muse_battery_json()
 *   r                      prints muse_battery_read() as JSON
 *   k                      prints muse_battery_saved_json(), or null
 *   save <file>            writes the RTC memory, for a later run to boot with
 * and before any of those, a boot:
 *   load <file>            the RTC memory a run saved
 *   boot <reason>          esp_reset_reason(), ESP_RST_POWERON by default */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_pm.h"
#include "esp_system.h"

/* Built in, for its RTC memory (s_saved). */
#include "muse_battery.c"

static int64_t s_clock;
static esp_reset_reason_t s_boot_reason = ESP_RST_POWERON;

esp_reset_reason_t esp_reset_reason(void)
{
    return s_boot_reason;
}
static char s_dump_text[4096];
static esp_pm_light_sleep_cb_t s_wake_cb;

int64_t esp_timer_get_time(void)
{
    return s_clock;
}

esp_err_t esp_pm_light_sleep_register_cbs(esp_pm_sleep_cbs_register_config_t *cbs)
{
    s_wake_cb = cbs->exit_cb;
    return ESP_OK;
}

esp_err_t esp_pm_dump_locks(FILE *stream)
{
    fputs(s_dump_text, stream);
    return ESP_OK;
}

int main(void)
{
    char line[512];
    bool booted = false;
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        if (!booted && !strncmp(line, "load ", 5)) {
            FILE *f = fopen(line + 5, "rb");
            if (!f || fread(&s_saved, sizeof(s_saved), 1, f) != 1) {
                fprintf(stderr, "can't load %s\n", line + 5);
                return 2;
            }
            fclose(f);
            continue;
        }
        if (!booted && !strncmp(line, "boot ", 5)) {
            s_boot_reason = (esp_reset_reason_t)atoi(line + 5);
            continue;
        }
        if (!booted) {
            muse_battery_init();
            booted = true;
        }
        if (!strncmp(line, "t ", 2)) {
            s_clock = strtoll(line + 2, NULL, 10);
        } else if (!strcmp(line, "dump")) {
            s_dump_text[0] = '\0';
            while (fgets(line, sizeof(line), stdin) && strcmp(line, "end\n")) {
                strlcat(s_dump_text, line, sizeof(s_dump_text));
            }
        } else if (!strncmp(line, "p ", 2)) {
            int usb;
            muse_power_t p = { 0 };
            sscanf(line + 2, "%d %d %d", &usb, &p.battery_pct, &p.battery_mv);
            p.usb = usb;
            muse_battery_note_power(&p, !p.usb && p.battery_pct >= 0);
        } else if (!strncmp(line, "s ", 2)) {
            int off, resting;
            sscanf(line + 2, "%d %d", &off, &resting);
            muse_battery_note_state(off, resting);
        } else if (!strncmp(line, "w ", 2)) {
            s_wake_cb(strtoll(line + 2, NULL, 10), NULL);
        } else if (!strcmp(line, "x")) {
            muse_battery_reset();
        } else if (!strcmp(line, "j")) {
            static char json[2048];
            muse_battery_json(json, sizeof(json));
            puts(json);
        } else if (!strcmp(line, "r")) {
            muse_battery_t m;
            muse_battery_read(&m);
            int rate10 = -1, full_h = -1;
            muse_battery_drain(&m, &rate10, &full_h);
            printf("{\"rate10\":%d,\"full_h\":%d,\"started\":%d,\"running\":%d,\"secs\":%lld,\"pct\":[%d,%d],"
                   "\"mv\":[%d,%d],\"screen_off_pm\":%d,\"resting_pm\":%d,\"slept_pm\":%d,\"sleeps\":%lu,"
                   "\"busy_pm\":%d,\"awake\":\"%s\"}\n",
                   rate10, full_h, m.started, m.running, (long long)m.secs, m.pct_start, m.pct_now, m.mv_start,
                   m.mv_now, m.screen_off_pm, m.resting_pm, m.slept_pm, (unsigned long)m.sleeps, m.busy_pm, m.awake);
        } else if (!strcmp(line, "k")) {
            const char *saved = muse_battery_saved_json();
            puts(saved ? saved : "null");
        } else if (!strncmp(line, "save ", 5)) {
            FILE *f = fopen(line + 5, "wb");
            if (!f || fwrite(&s_saved, sizeof(s_saved), 1, f) != 1) {
                fprintf(stderr, "can't save %s\n", line + 5);
                return 2;
            }
            fclose(f);
        } else {
            fprintf(stderr, "bad command: %s\n", line);
            return 2;
        }
    }
    return 0;
}
