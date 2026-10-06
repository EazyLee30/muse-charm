// Pins and sensor commands from Waveshare ESP32-S3-RLCD-4.2 examples:
// Arduino/06_SD_Card, 05_I2C_SHTC3, 04_I2C_PCF85063. Mount never formats.
#include "charm_hardware.h"
#include "charm_font.h"
#include <stdio.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include "driver/i2c_master.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static sdmmc_card_t *card;
static i2c_master_dev_handle_t sht,rtc;
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static float temperature,humidity;
static int64_t measured;
static bool rtc_valid;
static esp_err_t sd_error=ESP_ERR_INVALID_STATE;
static uint8_t crc(const uint8_t *p) {uint8_t c=255;for(int i=0;i<2;i++){c^=p[i];for(int b=0;b<8;b++) c=(c&128)?(c<<1)^0x31:c<<1;}return c;}
static bool sht_command(uint16_t command) {uint8_t d[]={command>>8,command};return i2c_master_transmit(sht,d,2,100)==ESP_OK;}
static int unbcd(uint8_t b) {return (b>>4)*10+(b&15);}
static uint8_t bcd(int v) {return ((v/10)<<4)|(v%10);}
static void rtc_update(void) {
    if(!rtc) return;
    time_t now=time(NULL);
    if(now<1704067200) {
        uint8_t reg=4,d[7];
        if(i2c_master_transmit_receive(rtc,&reg,1,d,7,100)!=ESP_OK || (d[0]&128)) return;
        struct tm t={.tm_sec=unbcd(d[0]&127),.tm_min=unbcd(d[1]&127),.tm_hour=unbcd(d[2]&63),
            .tm_mday=unbcd(d[3]&63),.tm_mon=unbcd(d[5]&31)-1,.tm_year=unbcd(d[6])+100};
        if(t.tm_year<124 || t.tm_mon<0 || t.tm_mon>11 || t.tm_mday<1 || t.tm_mday>31 || t.tm_hour>23 || t.tm_min>59 || t.tm_sec>59) return;
        struct timeval tv={.tv_sec=timegm(&t)};settimeofday(&tv,NULL);rtc_valid=true;
    } else {
        static int64_t written;
        if(esp_timer_get_time()-written<600000000 && rtc_valid) return;
        struct tm t;gmtime_r(&now,&t);
        uint8_t d[]={4,bcd(t.tm_sec),bcd(t.tm_min),bcd(t.tm_hour),bcd(t.tm_mday),t.tm_wday,bcd(t.tm_mon+1),bcd(t.tm_year-100)};
        rtc_valid=i2c_master_transmit(rtc,d,sizeof(d),100)==ESP_OK;written=esp_timer_get_time();
    }
}
static void hardware_task(void *arg) {
    (void)arg;
    ESP_LOGI("charm.font","Chinese captions: %s",charm_font_init()?"ready":"unavailable");
    sdmmc_host_t host=SDMMC_HOST_DEFAULT();host.max_freq_khz=SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot=SDMMC_SLOT_CONFIG_DEFAULT();slot.width=1;slot.clk=38;slot.cmd=21;slot.d0=39;
    esp_vfs_fat_sdmmc_mount_config_t mount={.format_if_mount_failed=false,.max_files=4,.allocation_unit_size=16*1024};
    sd_error=esp_vfs_fat_sdmmc_mount("/sdcard",&host,&slot,&mount,&card);
    ESP_LOGI("charm.sd","mount: %s",esp_err_to_name(sd_error));
    i2c_master_bus_handle_t bus=NULL;
    for(int i=0;i<30;i++) {if(i2c_master_get_bus_handle(0,&bus)==ESP_OK) break;vTaskDelay(pdMS_TO_TICKS(100));}
    if(bus) {
        i2c_device_config_t cfg={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=0x70,.scl_speed_hz=100000};
        if(i2c_master_probe(bus,0x70,100)==ESP_OK) i2c_master_bus_add_device(bus,&cfg,&sht);
        cfg.device_address=0x51;
        if(i2c_master_probe(bus,0x51,100)==ESP_OK) i2c_master_bus_add_device(bus,&cfg,&rtc);
    }
    ESP_LOGI("charm.sensor","SHTC3=%s RTC=%s",sht?"present":"missing",rtc?"present":"missing");
    for(;;) {
        if(sht && sht_command(0x3517)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            if(sht_command(0x7866)) {
                vTaskDelay(pdMS_TO_TICKS(20));uint8_t d[6];
                if(i2c_master_receive(sht,d,6,100)==ESP_OK && crc(d)==d[2] && crc(d+3)==d[5]) {
                    float t=-45+175.0f*((d[0]<<8)|d[1])/65536.0f,h=100.0f*((d[3]<<8)|d[4])/65536.0f;
                    portENTER_CRITICAL(&lock);temperature=t;humidity=h;measured=esp_timer_get_time();portEXIT_CRITICAL(&lock);
                }
            }
            sht_command(0xb098);
        }
        rtc_update();vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
void charm_hardware_start(void) {if(xTaskCreate(hardware_task,"charm_hw",6144,NULL,2,NULL)!=pdPASS) ESP_LOGE("charm.hw","task unavailable");}
cJSON *charm_sensors(void) {
    cJSON *out=cJSON_CreateObject();
    portENTER_CRITICAL(&lock);float t=temperature,h=humidity;int64_t at=measured;portEXIT_CRITICAL(&lock);
    bool fresh=at && esp_timer_get_time()-at<30000000;
    cJSON_AddBoolToObject(out,"ok",fresh);cJSON_AddStringToObject(out,"sensor","SHTC3");
    if(fresh) {cJSON_AddNumberToObject(out,"temperature_c",t);cJSON_AddNumberToObject(out,"humidity_percent",h);}
    else {cJSON_AddNullToObject(out,"temperature_c");cJSON_AddNullToObject(out,"humidity_percent");}
    cJSON_AddBoolToObject(out,"rtc_valid",rtc_valid);return out;
}
bool charm_storage_path(const char *relative,char *out,size_t size) {
    if(!card || !relative || strstr(relative,"..") || strchr(relative,'\\') || strlen(relative)>220) return false;
    while(*relative=='/') relative++;
    return snprintf(out,size,"/sdcard/%s",relative)<(int)size;
}
cJSON *charm_storage_command(const char *command,cJSON *params) {
    cJSON *out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"mounted",card!=NULL);
    if(!card) {cJSON_AddBoolToObject(out,"ok",false);cJSON_AddStringToObject(out,"error",esp_err_to_name(sd_error));return out;}
    const char *relative=cJSON_GetStringValue(cJSON_GetObjectItem(params,"path"));if(!relative) relative="";
    char path[256];
    if(!charm_storage_path(relative,path,sizeof(path))) {cJSON_AddBoolToObject(out,"ok",false);return out;}
    if(!strcmp(command,"charm.storage.read")) {
        char *data=heap_caps_calloc(1,2049,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        FILE *f=data?fopen(path,"rb"):NULL;size_t n=f?fread(data,1,2048,f):0;if(f) fclose(f);
        cJSON_AddBoolToObject(out,"ok",f!=NULL);cJSON_AddStringToObject(out,"text",data?data:"");cJSON_AddBoolToObject(out,"truncated",n==2048);free(data);return out;
    }
    DIR *dir=opendir(path);cJSON_AddBoolToObject(out,"ok",dir!=NULL);
    cJSON_AddNumberToObject(out,"capacity_mb",(double)card->csd.capacity*card->csd.sector_size/1048576);
    cJSON *files=cJSON_AddArrayToObject(out,"files");unsigned count=0;struct dirent *entry;
    while(dir && count<64 && (entry=readdir(dir))) {
        if(entry->d_name[0]=='.') continue;
        cJSON *f=cJSON_CreateObject();cJSON_AddStringToObject(f,"name",entry->d_name);
        char full[512];struct stat info;snprintf(full,sizeof(full),"%s/%s",path,entry->d_name);
        if(!stat(full,&info)) {cJSON_AddBoolToObject(f,"directory",S_ISDIR(info.st_mode));cJSON_AddNumberToObject(f,"bytes",info.st_size);}
        cJSON_AddItemToArray(files,f);count++;
    }
    if(dir) closedir(dir);
    cJSON_AddBoolToObject(out,"limit_reached",count==64);return out;
}
