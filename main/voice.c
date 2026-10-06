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

#include "voice.h"

#include <stdatomic.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button.h"
#include "config_store.h"
#include "led_status.h"
#include "muse_chat.h"
#include "voice_board.h"
#include "voice_muse_chat.h"
#include "voice_player.h"
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_RLCD42_ST7305
#include "muse_tts.h"
#include "charm.h"
#include "minimp3.h"
#endif

static const char *TAG = "link.voice";

#define KEY_VOLUME          "voice_volume"
#define DEFAULT_VOLUME      60
// Volume change per detent of the dial.
#define VOLUME_STEP         5
// Store a dialled volume once the dial has rested this long.
#define VOLUME_SAVE_MS      2000
#define DIAL_POLL_MS        20

#define CAPTURE_MAX_MS      15000
#define CAPTURE_MIN_MS      300
#define CAPTURE_CHUNK       (VOICE_MIC_RATE / 50)      // 20 ms
#define CAPTURE_MAX         (VOICE_MIC_RATE * CAPTURE_MAX_MS / 1000)
// Keep listening briefly after release so the last word is not clipped.
#define RELEASE_TAIL_MS     250
#define REPLY_CHUNK         (VOICE_PLAYER_RATE / 50)   // 20 ms

typedef enum { EVT_PRESS, EVT_RELEASE, EVT_SPEAKER_TEST, EVT_SAY, EVT_MUSIC } voice_evt_t;

static QueueHandle_t s_events;
static atomic_bool s_ready;
static atomic_int s_audio_owner; // 0 idle, 1 voice turn, 2 speaker diagnostic
static atomic_int s_volume;
static atomic_bool s_media_stop;
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_RLCD42_ST7305
static portMUX_TYPE s_music_lock=portMUX_INITIALIZER_UNLOCKED;
static char s_pending_music[1024];
#endif

static int load_volume(void) {
    char buf[8];
    if (!config_get_str(KEY_VOLUME, buf, sizeof(buf)) || !buf[0]) return DEFAULT_VOLUME;
    int v = atoi(buf);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static bool store_volume(int volume) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", volume);
    return config_set_str(KEY_VOLUME, buf);
}

// Set the speaker volume and show it on the ring.
static void apply_volume(int volume) {
    atomic_store(&s_volume, volume);
    voice_board_set_volume(volume);
    led_status_show_volume(volume);
}

// Turns the volume with the dial. On an internal-RAM stack: storing the volume
// writes NVS.
static void dial_task(void *arg) {
    int64_t save_at = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DIAL_POLL_MS));
        int steps = voice_board_dial_steps();
        if (steps) {
            int volume = atomic_load(&s_volume) + steps * VOLUME_STEP;
            apply_volume(volume < 0 ? 0 : volume > 100 ? 100 : volume);
            save_at = esp_timer_get_time() + VOLUME_SAVE_MS * 1000LL;
        } else if (save_at && esp_timer_get_time() >= save_at) {
            save_at = 0;
            int volume = atomic_load(&s_volume);
            if (store_volume(volume)) ESP_LOGI(TAG, "volume %d", volume);
            else ESP_LOGW(TAG, "volume could not be stored");
        }
    }
}

// Did a press arrive? Releases are dropped.
static bool pressed_again(TickType_t wait) {
    voice_evt_t evt;
    while (xQueueReceive(s_events, &evt, wait) == pdTRUE) {
        if (evt == EVT_PRESS) return true;
        wait = 0;
    }
    return false;
}

// Stream the microphone into the turn until release (plus a tail) or the
// limit. Returns the number of samples, 0 if the capture failed.
static size_t record(void) {
    static int16_t chunk[CAPTURE_CHUNK];
    if (voice_board_mic_start() != ESP_OK) return 0;
    size_t samples = 0;
    int maximum = 0;
    uint64_t energy = 0;
    int64_t stop_at = 0;
    while (samples < CAPTURE_MAX) {
        voice_evt_t evt;
        if (!stop_at && xQueueReceive(s_events, &evt, 0) == pdTRUE && evt == EVT_RELEASE) {
            stop_at = esp_timer_get_time() + RELEASE_TAIL_MS * 1000LL;
        }
        if (stop_at && esp_timer_get_time() >= stop_at) break;
        int peak = 0;
        size_t got = voice_board_mic_read(chunk, CAPTURE_CHUNK, &peak);
        if (!got) break;
        if (peak > maximum) maximum = peak;
        for (size_t i = 0; i < got; i++) energy += (int64_t)chunk[i] * chunk[i];
        muse_hatch_turn_audio(chunk, got);
        samples += got;
        led_status_set_level(peak / 12000.0f);
    }
    voice_board_mic_stop();
    ESP_LOGI(TAG, "microphone samples=%u peak=%d rms=%.0f", (unsigned)samples, maximum,
             samples ? sqrt((double)energy / samples) : 0.0);
    if (samples >= CAPTURE_MAX) ESP_LOGI(TAG, "capture limit reached");
    return samples;
}

static bool fail(const char *why) {
    ESP_LOGW(TAG, "turn failed: %s", why);
    led_status_set_voice(LED_VOICE_ERROR);
    return false;
}

// Play the reply as it arrives. Returns true if a new press interrupted it.
static bool reply(void) {
    static int16_t pcm[REPLY_CHUNK];
    char text[192];
    int64_t caption_at=0;
    bool done = false;
    size_t played = 0;
    int64_t t0 = esp_timer_get_time();
    voice_player_begin();
    for (;;) {
        if (pressed_again(0)) {
            muse_hatch_turn_cancel();
            voice_player_stop();
            return true;
        }
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            switch (ev) {
            case MUSE_HATCH_EV_HEARD:
                ESP_LOGI(TAG, "heard: %s", text);
                led_status_set_voice(LED_VOICE_THINKING);
                break;
            case MUSE_HATCH_EV_REPLY:
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_RLCD42_ST7305
                atomic_store(&s_media_stop,false);
                charm_ui_caption(text);
#endif
                if (!voice_player_started()) led_status_set_voice(LED_VOICE_BUFFERING);
                break;
            case MUSE_HATCH_EV_DONE:
                done = true;
                break;
            case MUSE_HATCH_EV_ERROR:
                voice_player_stop();
                return fail(text);
            default:
                break;
            }
        }
        // After DONE the reply's audio is all decoded; drain what's left.
        size_t n = muse_hatch_turn_read(pcm, REPLY_CHUNK, done ? 0 : 20);
        if (n) {
            voice_player_write(pcm, n);
            played += n;
        } else if (done) {
            break;
        }
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_RLCD42_ST7305
        if(esp_timer_get_time()-caption_at>500000 && muse_hatch_turn_caption(played,text,sizeof(text))) {
            charm_ui_caption(text);caption_at=esp_timer_get_time();
        }
#endif
    }
    voice_player_end();
    while (!voice_player_wait(0)) {
        if (pressed_again(pdMS_TO_TICKS(40))) {
            voice_player_stop();
            return true;
        }
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_RLCD42_ST7305
        if(esp_timer_get_time()-caption_at>500000 && muse_hatch_turn_caption(played,text,sizeof(text))) {
            charm_ui_caption(text);caption_at=esp_timer_get_time();
        }
#endif
    }
    ESP_LOGI(TAG, "reply: %.1fs of speech, %.1fs total", (double)played / VOICE_PLAYER_RATE,
             (esp_timer_get_time() - t0) / 1e6);
    led_status_set_voice(LED_VOICE_IDLE);
    return false;
}

// Returns true if a new press interrupted the turn.
static bool run_turn(void) {
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_RLCD42_ST7305
    portENTER_CRITICAL(&s_music_lock);s_pending_music[0]=0;portEXIT_CRITICAL(&s_music_lock);
#endif
    voice_player_stop();
    led_status_set_level(0);
    led_status_set_voice(LED_VOICE_LISTENING);
    muse_hatch_turn_begin();
    size_t samples = record();
    if (samples < VOICE_MIC_RATE * CAPTURE_MIN_MS / 1000) {
        muse_hatch_turn_cancel();
        if (!samples) return fail("microphone unavailable");
        ESP_LOGI(TAG, "press too short");
        led_status_set_voice(LED_VOICE_IDLE);
        return false;
    }
    ESP_LOGI(TAG, "recorded %.1fs", (double)samples / VOICE_MIC_RATE);
    led_status_set_voice(LED_VOICE_TRANSCRIBING);
    muse_hatch_turn_end();
    return reply();
}

// Runs on the button task. Claims the press only when a turn can run, so the
// button keeps its setup role otherwise.
static bool on_press(bool pressed) {
    if (pressed) {
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_RLCD42_ST7305
        if(atomic_load(&s_audio_owner)==3) {voice_stop_music();return true;}
#endif
        if (!atomic_load(&s_ready) || atomic_load(&s_audio_owner) == 2 || voice_board_muted()) return false;
        voice_hatch_refresh();
        if (!muse_hatch_ready()) return false;
        int expected = 0;
        if (!atomic_compare_exchange_strong(&s_audio_owner, &expected, 1) && expected != 1) return false;
    }
    voice_evt_t evt = pressed ? EVT_PRESS : EVT_RELEASE;
    if (xQueueSend(s_events, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "voice event queue full");
        return false;
    }
    return true;
}

// Test the same player, DAC and amplifier path used by replies, without a network.
static void speaker_test(void) {
    voice_board_set_volume(60); // known test level, independent of saved mute/volume
    int16_t pcm[CAPTURE_CHUNK];
    voice_player_begin();
    esp_err_t err = ESP_OK;
    for (int offset = 0; offset < VOICE_PLAYER_RATE && err == ESP_OK; offset += CAPTURE_CHUNK) {
        for (int i = 0; i < CAPTURE_CHUNK; i++) {
            int sample = offset + i;
            float hz = sample < VOICE_PLAYER_RATE / 2 ? 440.0f : 660.0f;
            // Fade the edges of each half-second note to prevent clicks.
            int local = sample % (VOICE_PLAYER_RATE / 2);
            float gain = fminf(1.0f, fminf(local, VOICE_PLAYER_RATE / 2 - 1 - local) / 160.0f);
            pcm[i] = (int16_t)(2500.0f * gain * sinf(6.2831853f * hz * sample / VOICE_PLAYER_RATE));
        }
        err = voice_player_write(pcm, CAPTURE_CHUNK);
    }
    voice_player_end();
    bool drained = voice_player_wait(5000);
    if (err != ESP_OK || !drained) voice_player_stop();
    ESP_LOGI(TAG, "speaker test: write=%s drained=%s", esp_err_to_name(err), drained ? "yes" : "no");
    voice_board_set_volume(atomic_load(&s_volume));
    atomic_store(&s_audio_owner, 0);
}

bool voice_speaker_test(void) {
    if (!atomic_load(&s_ready) || !muse_hatch_ready()) return false;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&s_audio_owner, &expected, 2)) return false;
    voice_evt_t evt = EVT_SPEAKER_TEST;
    if (xQueueSend(s_events, &evt, 0) == pdTRUE) return true;
    atomic_store(&s_audio_owner, 0);
    return false;
}

#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_RLCD42_ST7305
static void say_reply(bool music) {
    uint8_t *mp3=heap_caps_malloc(32768,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    mp3dec_t *dec=heap_caps_malloc(sizeof(*dec),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    int16_t *pcm=heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME*sizeof(int16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    int16_t *resampled=heap_caps_malloc(4616*sizeof(int16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    uint32_t phase=0;int16_t previous=0;
    if(!mp3 || !dec || !pcm || !resampled) goto finish;
    mp3dec_init(dec);voice_player_begin();led_status_set_voice(LED_VOICE_BUFFERING);
    size_t length=0,played=0;bool done=false;
    int64_t deadline=esp_timer_get_time()+(music?1800000000LL:60000000);
    while(esp_timer_get_time()<deadline && !atomic_load(&s_media_stop)) {
        length+=muse_tts_read(mp3+length,32768-length,&done);
        size_t offset=0;
        while(length-offset>(done?0:2048)) {
            mp3dec_frame_info_t info;
            int samples=mp3dec_decode_frame(dec,mp3+offset,length-offset,pcm,&info);
            if(!info.frame_bytes) {if(done) offset=length;break;}
            offset+=info.frame_bytes;
            if(samples) {
                if(info.hz<8000 || info.hz>48000) goto stop;
                if(info.channels==2) for(int i=0;i<samples;i++) pcm[i]=(pcm[2*i]+pcm[2*i+1])/2;
                size_t n=0;uint32_t step=((uint64_t)info.hz<<16)/16000;
                while((phase>>16)<(unsigned)samples) {
                    unsigned i=phase>>16;int32_t a=i?pcm[i-1]:previous,b=pcm[i];
                    resampled[n++]=a+(((int64_t)(b-a)*(phase&65535))>>16);phase+=step;
                }
                phase-=(uint32_t)samples<<16;previous=pcm[samples-1];
                if(voice_player_write(resampled,n)!=ESP_OK) goto stop;
                played+=n;led_status_set_voice(LED_VOICE_SPEAKING);
            }
        }
        if(offset) {memmove(mp3,mp3+offset,length-offset);length-=offset;}
        if(done && !length) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    voice_player_end();
    if(!voice_player_wait(20000)) voice_player_stop();
    ESP_LOGI(TAG,"%s played %u PCM samples",music?"music":"TTS",(unsigned)played);
stop:
    muse_tts_cancel();voice_player_stop();
finish:
    free(mp3);free(dec);free(pcm);free(resampled);led_status_set_voice(LED_VOICE_IDLE);
    atomic_store(&s_audio_owner,0);
}
void voice_stop_music(void) {
    portENTER_CRITICAL(&s_music_lock);s_pending_music[0]=0;portEXIT_CRITICAL(&s_music_lock);
    if(atomic_load(&s_audio_owner)!=3) return;
    atomic_store(&s_media_stop,true);muse_tts_cancel();voice_player_stop();
}
bool voice_play_music(const char *source) {
    if(!atomic_load(&s_ready) || !source || strlen(source)>=sizeof(s_pending_music)
       || (strncmp(source,"/sdcard/",8) && strncmp(source,"http://",7) && strncmp(source,"https://",8))) return false;
    // A tool invoked by a spoken request must wait for Muse's reply to drain.
    // It cannot claim the speaker while that same voice turn owns it.
    portENTER_CRITICAL(&s_music_lock);
    if(atomic_load(&s_audio_owner)==1) {
        strcpy(s_pending_music,source);
        portEXIT_CRITICAL(&s_music_lock);return true;
    }
    portEXIT_CRITICAL(&s_music_lock);
    int expected=0;if(!atomic_compare_exchange_strong(&s_audio_owner,&expected,3)) return false;
    atomic_store(&s_media_stop,false);
    if(!muse_tts_media(source)) {atomic_store(&s_audio_owner,0);return false;}
    voice_evt_t event=EVT_MUSIC;
    if(xQueueSend(s_events,&event,0)==pdTRUE) return true;
    muse_tts_cancel();atomic_store(&s_audio_owner,0);return false;
}
static void play_pending_music(void) {
    char pending[1024];
    portENTER_CRITICAL(&s_music_lock);
    atomic_store(&s_audio_owner,0);
    strcpy(pending,s_pending_music);s_pending_music[0]=0;
    portEXIT_CRITICAL(&s_music_lock);
    if(pending[0] && !voice_play_music(pending)) ESP_LOGW(TAG,"queued song could not start");
}
bool voice_say(const char *text) {
    if(!atomic_load(&s_ready) || !muse_hatch_ready()) return false;
    int expected=0;
    if(!atomic_compare_exchange_strong(&s_audio_owner,&expected,2)) return false;
    atomic_store(&s_media_stop,false);
    charm_ui_caption(text);
    if(!muse_tts_say(text)) {atomic_store(&s_audio_owner,0);return false;}
    voice_evt_t event=EVT_SAY;
    if(xQueueSend(s_events,&event,0)==pdTRUE) return true;
    muse_tts_cancel();atomic_store(&s_audio_owner,0);return false;
}
#endif

static void voice_task(void *arg) {
    if (voice_board_init() != ESP_OK || voice_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio hardware unavailable; voice chat disabled");
        vTaskSuspend(NULL);
        return;
    }
    voice_board_set_volume(atomic_load(&s_volume));
    atomic_store(&s_ready, true);
    if (xTaskCreate(dial_task, "dial", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the dial");
    }
    ESP_LOGI(TAG, "ready");

    for (;;) {
        voice_evt_t event;
        if (xQueueReceive(s_events, &event, portMAX_DELAY) != pdTRUE) continue;
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_RLCD42_ST7305
        if(event==EVT_SAY || event==EVT_MUSIC) {say_reply(event==EVT_MUSIC);continue;}
#endif
        if (event == EVT_SPEAKER_TEST) { speaker_test(); continue; }
        if (event != EVT_PRESS) continue;
        while (run_turn()) {
        }
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_RLCD42_ST7305
        play_pending_music();
#else
        atomic_store(&s_audio_owner, 0);
#endif
    }
}

void voice_init(void) {
    s_events = xQueueCreate(8, sizeof(voice_evt_t));
    if (!s_events) {
        ESP_LOGE(TAG, "no memory for voice chat");
        return;
    }
    atomic_store(&s_volume, load_volume());
    voice_hatch_refresh();
    muse_hatch_start();
    // The stack is in PSRAM, so the task must not touch flash (NVS): pairing
    // needs an 8 KB internal block for its TLS task.
    if (xTaskCreateWithCaps(voice_task, "voice",
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_RLCD42_ST7305
                                32*1024, // minimp3 needs about 16 KB of scratch stack
#else
                                4096,
#endif
                                NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to start voice chat");
        return;
    }
    button_set_press_cb(on_press);
}

cJSON *voice_configure_command(cJSON *params) {
    cJSON *volume = cJSON_GetObjectItem(params, "volume");
    const char *why = NULL;
    if (volume) {
        if (!cJSON_IsNumber(volume) || volume->valueint < 0 || volume->valueint > 100) {
            why = "volume must be 0-100";
        } else {
            if (!store_volume(volume->valueint)) why = "volume could not be stored";
            else if (atomic_load(&s_ready)) apply_volume(volume->valueint);
            else atomic_store(&s_volume, volume->valueint);
        }
    }

    cJSON *result = cJSON_CreateObject();
    if (why) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "code", "invalid_params");
        cJSON_AddStringToObject(error, "message", why);
        cJSON_AddItemToObject(result, "error", error);
        return result;
    }
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "volume", atomic_load(&s_volume));
    return result;
}
