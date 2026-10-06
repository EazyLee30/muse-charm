// USB text turns use the official Muse session and tools/muse/chat.py protocol.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config_store.h"
#include "led_status.h"
#include "muse_chat.h"
#include "muse_console.h"
#include "wifi_mgr.h"
#include "voice.h"
#include "muse_tts.h"
#include "charm.h"
#include "driver/usb_serial_jtag_vfs.h"

#define CHAT_MAX (192 * 1024)
#define RLCD_CONSOLE_LINE_MAX 1024
static char *s_message;
static size_t s_length;

static void status(void) {
    muse_hatch_status_t hatch;
    muse_hatch_status(&hatch);
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "board", "waveshare-s3-rlcd42");
    cJSON_AddBoolToObject(root, "chat", true);
    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON *link = cJSON_AddObjectToObject(device, "link");
    cJSON_AddBoolToObject(link, "paired", config_setup_complete() && config_is_provisioned());
    cJSON *wifi = cJSON_AddObjectToObject(device, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", wifi_mgr_is_connected());
    cJSON *chat = cJSON_AddObjectToObject(device, "hatch");
    cJSON_AddStringToObject(chat, "state", muse_hatch_state_name(hatch.state));
    cJSON_AddStringToObject(chat, "detail", hatch.detail);
    cJSON_AddBoolToObject(chat, "token", false);
    char *json = cJSON_PrintUnformatted(root);
    if (json) { printf("@status %s\n", json); fflush(stdout); free(json); }
    cJSON_Delete(root);
}

static void command(char *line, bool whole) {
    if (whole && !strncmp(line,"tts.setup=",10)) {
        cJSON *root=cJSON_Parse(line+10);
        const char *key=cJSON_GetStringValue(cJSON_GetObjectItem(root,"key"));
        bool ok=key && strlen(key)<256 && config_set_str("tts_key",key) && muse_tts_configure(key);
        printf("@tts {\"configured\":%s}\n",ok?"true":"false");fflush(stdout);
        cJSON_Delete(root);memset(line,0,strlen(line));return;
    }
    if(whole && !strncmp(line,"charm.",6)) {
        char *eq=strchr(line,'=');cJSON *params=NULL;
        if(eq) {*eq=0;params=cJSON_Parse(eq+1);}
        cJSON *out=charm_command(line,params);char *json=cJSON_PrintUnformatted(out);
        if(json) {printf("@charm %s\n",json);fflush(stdout);free(json);}
        cJSON_Delete(out);cJSON_Delete(params);return;
    }
    if (!strcmp(line, "audio.test")) {
        printf("@audio {\"queued\":%s}\n", voice_speaker_test() ? "true" : "false");
        fflush(stdout); return;
    }
    if (!strcmp(line, "status")) { status(); return; }
    if (!strcmp(line, "chat.cancel")) {
        free(s_message); s_message = NULL; s_length = 0;
        muse_hatch_text_cancel(); return;
    }
    bool last = !strncmp(line, "chat=", 5);
    if (!last && strncmp(line, "chat+=", 6)) return;
    char *piece = line + (last ? 5 : 6);
    size_t n = muse_hatch_unescape(piece);
    if (!s_message) {
        s_message = heap_caps_malloc(CHAT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_length = 0;
    }
    const char *err = !whole ? "LINE TOO LONG" : !s_message ? "OUT OF MEMORY" :
                      s_length + n >= CHAT_MAX ? "TOO LONG" : NULL;
    if (err) {
        free(s_message); s_message = NULL; s_length = 0;
        muse_hatch_console("error", err, NULL); return;
    }
    memcpy(s_message + s_length, piece, n); s_length += n;
    s_message[s_length] = '\0';
    muse_hatch_console("ack", NULL, "\"bytes\":%u", (unsigned)s_length);
    if (last) {
        muse_hatch_text_turn(s_message); s_message = NULL; s_length = 0;
    }
}

static void console_task(void *arg) {
    (void)arg;
    if (muse_console_install(2048) != ESP_OK) {
        ESP_LOGE("rlcd.console", "USB console install failed");
        vTaskDelete(NULL); return;
    }
    usb_serial_jtag_vfs_use_driver();
    char *line = heap_caps_malloc(RLCD_CONSOLE_LINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line) { vTaskDelete(NULL); return; }
    for (;;) {
        uint8_t ch;
        if (!muse_console_getc(&ch) || ch != '>') continue;
        size_t len = 0; bool whole = true;
        while (muse_console_getc(&ch) && ch != '\n') {
            if (ch == '\r') continue;
            if (len < RLCD_CONSOLE_LINE_MAX - 1) line[len++] = (char)ch;
            else whole = false;
        }
        line[len] = '\0'; command(line, whole);
    }
}

void rlcd42_console_start(void) {
    if (xTaskCreate(console_task, "rlcd_console", 8192, NULL, 3, NULL) != pdPASS)
        ESP_LOGE("rlcd.console", "USB console task unavailable");
}
