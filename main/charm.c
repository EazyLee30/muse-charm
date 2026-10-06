// Board facts: Waveshare's ESP32-S3-RLCD-4.2 GPIO table, battery ADC on
// GPIO4 (ADC1 channel 3), divider x3. gameboy/fofo_power.cpp is a reference
// for sampling; voltage is an estimate, not a charge-controller reading.
#include "charm.h"
#include "charm_timezone.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config_store.h"
#include "wifi_mgr.h"
#include "voice.h"
#include "charm_hardware.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static charm_view_t s_view = {.battery_pct=-1};
static int64_t s_reaction_at,s_key_at,s_boot_at;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_sntp, s_started;
static uint16_t s_buttons;
static int s_hat = -1;
static volatile int s_pad_request;
static volatile int s_toggle_request;

void charm_buttons(bool key,bool boot) {
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if(key && !s_view.key_pressed) s_key_at=now;
    if(boot && !s_view.boot_pressed) s_boot_at=now;
    s_view.key_pressed=key;s_view.boot_pressed=boot;
    portEXIT_CRITICAL(&s_lock);
}

void charm_react(charm_reaction_t reaction) {
    portENTER_CRITICAL(&s_lock);
    s_view.reaction = reaction;
    s_reaction_at = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
}

void charm_snapshot(charm_view_t *out) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    *out = s_view;
    out->key_pressed |= s_key_at && now-s_key_at<250000;
    out->boot_pressed |= s_boot_at && now-s_boot_at<250000;
    out->reaction_t = (now-s_reaction_at)/1000000.0f;
    if (out->reaction != CHARM_SLEEP && out->reaction_t > 8) out->reaction = CHARM_CALM;
    portEXIT_CRITICAL(&s_lock);
}

void charm_pad_status(bool connected, bool pairing) {
    portENTER_CRITICAL(&s_lock);
    s_view.pad_connected=connected; s_view.pad_pairing=pairing;
    if (!connected) { s_buttons=0; s_hat=-1; }
    portEXIT_CRITICAL(&s_lock);
}

void charm_pad_input(uint16_t buttons, int hat) {
    portENTER_CRITICAL(&s_lock);
    uint16_t down = buttons & ~s_buttons;
    if (hat != s_hat) {
        if (hat >= 1 && hat <= 3) s_view.offset_x += 12;
        if (hat >= 5 && hat <= 7) s_view.offset_x -= 12;
        if (s_view.offset_x > 24) s_view.offset_x=24;
        if (s_view.offset_x < -24) s_view.offset_x=-24;
    }
    s_buttons=buttons; s_hat=hat;
    portEXIT_CRITICAL(&s_lock);
    if (down & 1) charm_react(CHARM_PET);
    if (down & 2) s_toggle_request=1;
    if (down & 4) charm_react(CHARM_DANCE);
    if (down & 8) charm_react(CHARM_WAVE);
}

static bool set_dark(bool dark) {
    if (!config_set_str("charm_dark", dark ? "1" : "0")) return false;
    portENTER_CRITICAL(&s_lock); s_view.dark=dark; portEXIT_CRITICAL(&s_lock);
    charm_ui_set_dark(dark);
    return true;
}

static cJSON *failure(const char *why) {
    cJSON *out=cJSON_CreateObject(); cJSON_AddBoolToObject(out,"ok",false);
    cJSON_AddStringToObject(out,"error",why); return out;
}

cJSON *charm_command(const char *command, cJSON *params) {
    if(!strcmp(command,"charm.music")) {
        const char *action=cJSON_GetStringValue(cJSON_GetObjectItem(params,"action"));
        if(action && strcmp(action,"play") && strcmp(action,"stop")) return failure("action must be play or stop");
        if(action && !strcmp(action,"stop")) voice_stop_music();
        else {
            const char *url=cJSON_GetStringValue(cJSON_GetObjectItem(params,"url"));
            const char *relative=cJSON_GetStringValue(cJSON_GetObjectItem(params,"path"));char path[256];
            if(relative && charm_storage_path(relative,path,sizeof(path))) url=path;
            if(!url || !voice_play_music(url)) return failure("audio busy, invalid source or provider not initialized");
            charm_ui_caption("Music playing");
        }
        cJSON *out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"ok",true);cJSON_AddBoolToObject(out,"queued",true);return out;
    }
    if(!strcmp(command,"charm.sensors")) return charm_sensors();
    if(!strncmp(command,"charm.storage",13)) return charm_storage_command(command,params);
    if(!strcmp(command,"charm.speak")) {
        const char *text=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params,"text"));
        if(!text || !text[0] || strlen(text)>=1024) return failure("text must contain 1 to 1023 UTF-8 bytes");
        if(!voice_say(text)) return failure("audio busy or TTS not configured");
    } else if (!strcmp(command,"charm.configure")) {
        cJSON *rotate=cJSON_GetObjectItemCaseSensitive(params,"rotate");
        cJSON *zone=cJSON_GetObjectItemCaseSensitive(params,"timezone");
        char tz_rule[80];
        if(zone && (!cJSON_IsString(zone) || strlen(zone->valuestring)>=48 || !charm_timezone_resolve(zone->valuestring,tz_rule,sizeof(tz_rule)))) return failure("unsupported timezone; use a supported IANA name or UTC+08:00 offset");
        cJSON *mode=cJSON_GetObjectItemCaseSensitive(params,"mode");
        cJSON *reaction=cJSON_GetObjectItemCaseSensitive(params,"reaction");
        cJSON *location=cJSON_GetObjectItemCaseSensitive(params,"location");
        charm_view_t initial;charm_snapshot(&initial);
        bool dark=initial.dark;
        if (mode) {
            if (!cJSON_IsString(mode)) return failure("mode must be dark, light or toggle");
            if (!strcmp(mode->valuestring,"dark")) dark=true;
            else if (!strcmp(mode->valuestring,"light")) dark=false;
            else if (!strcmp(mode->valuestring,"toggle")) dark=!dark;
            else return failure("mode must be dark, light or toggle");
        }
        charm_reaction_t r=CHARM_CALM;
        if (reaction) {
            if (!cJSON_IsString(reaction)) return failure("invalid reaction");
            const char *names[]={"calm","pet","dance","wave","sleep","hop","peek","stretch"};
            int found=-1;
            for(int i=0;i<8;i++) if(!strcmp(reaction->valuestring,names[i])) found=i;
            if(found<0) return failure("reaction must be calm, pet, dance, wave, sleep, hop, peek or stretch");
            r=(charm_reaction_t)found;
        }
        if (location && (!cJSON_IsString(location) || strlen(location->valuestring)>=64))
            return failure("location must be a user supplied label under 64 bytes; no GPS");
        if(rotate && (!cJSON_IsString(rotate) || (strcmp(rotate->valuestring,"left") && strcmp(rotate->valuestring,"right") && strcmp(rotate->valuestring,"reset")))) return failure("rotate must be left, right or reset");
        if(zone) {
            if(!config_set_str("charm_timezone",zone->valuestring)) return failure("storage failed");
            portENTER_CRITICAL(&s_lock);snprintf(s_view.timezone,sizeof(s_view.timezone),"%s",zone->valuestring);portEXIT_CRITICAL(&s_lock);
        }
        if(rotate) {
            int rotation=!strcmp(rotate->valuestring,"reset")?0:(initial.rotation+(!strcmp(rotate->valuestring,"right")?1:3))%4;
            char value[4];snprintf(value,sizeof(value),"%d",rotation);
            if(!config_set_str("charm_rotation",value)) return failure("storage failed");
            portENTER_CRITICAL(&s_lock);s_view.rotation=rotation;portEXIT_CRITICAL(&s_lock);
            charm_ui_rotate(rotation);
        }
        if (mode && !set_dark(dark)) return failure("storage failed");
        if (reaction) charm_react(r);
        if (location) {
            if (!config_set_str("charm_location",location->valuestring)) return failure("storage failed");
            portENTER_CRITICAL(&s_lock);
            snprintf(s_view.location,sizeof(s_view.location),"%s",location->valuestring);
            portEXIT_CRITICAL(&s_lock);
        }
    } else if (!strcmp(command,"charm.controller")) {
        cJSON *action=cJSON_GetObjectItemCaseSensitive(params,"action");
        if(!config_setup_complete() || !config_is_provisioned()) return failure("finish Muse pairing first");
        if (!cJSON_IsString(action)) return failure("action must be pair or disconnect");
        if (!strcmp(action->valuestring,"pair")) s_pad_request=1;
        else if (!strcmp(action->valuestring,"disconnect")) s_pad_request=2;
        else return failure("action must be pair or disconnect");
    } else if (strcmp(command,"charm.status")) return failure("unknown charm command");
    charm_view_t v; charm_snapshot(&v);
    cJSON *out=cJSON_CreateObject(); cJSON_AddBoolToObject(out,"ok",true);
    cJSON_AddStringToObject(out,"model","Muse Charm / Waveshare ESP32-S3-RLCD-4.2");
    cJSON_AddStringToObject(out,"mode",v.dark?"dark":"light");
    cJSON_AddStringToObject(out,"location",v.location);
    cJSON_AddNumberToObject(out,"rotation_degrees",v.rotation*90);
    cJSON_AddBoolToObject(out,"gps",false);
    cJSON_AddBoolToObject(out,"wifi",v.wifi);
    cJSON_AddStringToObject(out,"time",v.clock);
    cJSON_AddStringToObject(out,"timezone",v.timezone);
    if(v.battery_pct>=0) cJSON_AddNumberToObject(out,"battery_percent_estimate",v.battery_pct);
    else cJSON_AddNullToObject(out,"battery_percent_estimate");
    cJSON_AddNumberToObject(out,"battery_mv",v.battery_mv);
    cJSON_AddBoolToObject(out,"controller_connected",v.pad_connected);
    cJSON_AddBoolToObject(out,"controller_pairing",v.pad_pairing);
    cJSON_AddStringToObject(out,"controller_buttons","A pet; B theme; X dance; Y wave; D-pad move");
    return out;
}

static void task(void *arg) {
    (void)arg;
    adc_oneshot_unit_init_cfg_t unit={.unit_id=ADC_UNIT_1};
    adc_oneshot_chan_cfg_t channel={.atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_DEFAULT};
    if(adc_oneshot_new_unit(&unit,&s_adc)==ESP_OK) {
        adc_oneshot_config_channel(s_adc,ADC_CHANNEL_3,&channel);
        adc_cali_curve_fitting_config_t cal={.unit_id=ADC_UNIT_1,.chan=ADC_CHANNEL_3,
            .atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_DEFAULT};
        adc_cali_create_scheme_curve_fitting(&cal,&s_cali);
    }
    // Gamepad work is optional; only start scanning after an explicit request.
    char active_zone[48]="",rule[80];
    for (;;) {
        charm_view_t snapshot;charm_snapshot(&snapshot);
        if(strcmp(active_zone,snapshot.timezone) && charm_timezone_resolve(snapshot.timezone,rule,sizeof(rule))) {
            setenv("TZ",rule,1);tzset();snprintf(active_zone,sizeof(active_zone),"%s",snapshot.timezone);
        }
        bool wifi=wifi_mgr_is_connected();
        if(wifi && !s_sntp) {
            esp_sntp_config_t cfg=ESP_NETIF_SNTP_DEFAULT_CONFIG("time.cloudflare.com");
            s_sntp=esp_netif_sntp_init(&cfg)==ESP_OK;
        }
        int mv=0,raw=0,reading=0,good=0;
        if(s_adc && s_cali) for(int i=0;i<6;i++) {
            if(adc_oneshot_read(s_adc,ADC_CHANNEL_3,&raw)==ESP_OK
               && adc_cali_raw_to_voltage(s_cali,raw,&reading)==ESP_OK) {mv+=reading;good++;}
        }
        mv=good?mv*3/good:0;
        time_t now=time(NULL); struct tm local; localtime_r(&now,&local);
        char clock[6]="--:--";
        if(now>1700000000) strftime(clock,sizeof(clock),"%H:%M",&local);
        portENTER_CRITICAL(&s_lock);
        s_view.wifi=wifi;
        if(mv>=2500 && mv<=4350) {
            s_view.battery_mv=s_view.battery_mv?(s_view.battery_mv*3+mv)/4:mv;
            int pct=(s_view.battery_mv-3000)*100/1120;
            s_view.battery_pct=pct<0?0:pct>100?100:pct;
        } else {s_view.battery_mv=0;s_view.battery_pct=-1;}
        memcpy(s_view.clock,clock,sizeof(clock));
        portEXIT_CRITICAL(&s_lock);
        if(s_toggle_request) {s_toggle_request=0;set_dark(!s_view.dark);charm_react(CHARM_PET);}
        int req=s_pad_request; s_pad_request=0;
        if(req==1) charm_controller_pair();
        if(req==2) charm_controller_disconnect();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void charm_start(void) {
    if(s_started) return;
    charm_hardware_start();
    char saved[64];
    bool dark=true;
    if(config_get_str("charm_dark",saved,sizeof(saved))) dark=!strcmp(saved,"1");
    snprintf(s_view.timezone,sizeof(s_view.timezone),"America/Los_Angeles");
    char rule[80];
    if(config_get_str("charm_timezone",saved,sizeof(saved)) && charm_timezone_resolve(saved,rule,sizeof(rule))) snprintf(s_view.timezone,sizeof(s_view.timezone),"%.47s",saved);
    s_view.dark=dark; charm_ui_set_dark(dark);
    if(config_get_str("charm_rotation",saved,sizeof(saved))) s_view.rotation=atoi(saved)%4;
    charm_ui_rotate(s_view.rotation);
    if(config_get_str("charm_location",saved,sizeof(saved))) snprintf(s_view.location,64,"%s",saved);
    s_started=xTaskCreate(task,"charm",4096,NULL,2,NULL)==pdPASS;
}
