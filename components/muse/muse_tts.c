// Qwen and MiniMax native TTS APIs; the Muse SDK supplies the MP3 decoder.
#include "muse_tts.h"
#include "muse_tts_config.h"
#include "../../main/config_store.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#define TTS_KEY_MAX 1536
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static char secret[TTS_KEY_MAX];
static muse_tts_config_t options;
static atomic_bool configured;
static QueueHandle_t jobs;
static StreamBufferHandle_t audio;
static StaticStreamBuffer_t audio_state;
static atomic_uint generation;
static atomic_bool finished, busy, failed, session_owned;
typedef struct {unsigned gen; bool media; char text[1024];} job_t;
static bool current(unsigned gen) {return gen==atomic_load(&generation);}
static esp_http_client_handle_t client(const char *url) {
    esp_http_client_config_t cfg={.url=url,.timeout_ms=15000,.crt_bundle_attach=esp_crt_bundle_attach,
        .disable_auto_redirect=true,.buffer_size=2048,.buffer_size_tx=2048};
    return esp_http_client_init(&cfg);
}
static void synthesize(job_t *job) {
    char *auth=heap_caps_malloc(TTS_KEY_MAX+8,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!auth) {if(current(job->gen)) {atomic_store(&failed,true);atomic_store(&finished,true);}return;}
    muse_tts_config_t config;
    portENTER_CRITICAL(&lock); snprintf(auth,TTS_KEY_MAX+8,"Bearer %s",secret);config=options;portEXIT_CRITICAL(&lock);
    cJSON *root=muse_tts_request(&config,job->text);
    char *body=cJSON_PrintUnformatted(root);cJSON_Delete(root);
    esp_http_client_handle_t http=client(muse_tts_endpoint(&config));
    char *response=heap_caps_calloc(1,8192,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    char *url=NULL;
    bool success=false;
    if(!http || !response || !body) goto cleanup;
    esp_http_client_set_method(http,HTTP_METHOD_POST);
    esp_http_client_set_header(http,"Authorization",auth);
    esp_http_client_set_header(http,"Content-Type","application/json");
    if(esp_http_client_open(http,strlen(body))!=ESP_OK) goto cleanup;
    int written=0,total=strlen(body);
    while(written<total && current(job->gen)) {
        int n=esp_http_client_write(http,body+written,total-written);if(n<=0) goto cleanup;written+=n;
    }
    if(!current(job->gen)) goto cleanup;
    esp_http_client_fetch_headers(http);
    int status=esp_http_client_get_status_code(http);
    if(status!=200) {ESP_LOGW("muse.tts","synthesis HTTP %d",status);goto cleanup;}
    int length=0;
    for(;;) {
        int n=esp_http_client_read(http,response+length,8191-length);
        if(n<0 || !current(job->gen)) goto cleanup;
        if(!n) break;
        length+=n;if(length>=8191) goto cleanup;
    }
    root=cJSON_Parse(response);
    const char *found=muse_tts_audio_url(&config,root);
    if(found && strlen(found)<2048) {
        // Audio URL gets its own client: never forward the API authorization.
        url=malloc(strlen(found)+2);
        if(url) {if(!strncmp(found,"http://",7)) sprintf(url,"https://%s",found+7);else strcpy(url,found);}
    }
    cJSON_Delete(root);
    esp_http_client_cleanup(http);http=NULL;
    if(!url || strncmp(url,"https://",8) || !current(job->gen)) goto cleanup;
    http=client(url);if(!http) goto cleanup;
    for(int redirects=0;redirects<4;redirects++) {
        if(!current(job->gen) || esp_http_client_open(http,0)!=ESP_OK) goto cleanup;
        esp_http_client_fetch_headers(http);int code=esp_http_client_get_status_code(http);
        if(code==200) break;
        if(code<300 || code>=400 || redirects==3 || esp_http_client_set_redirection(http)!=ESP_OK) goto cleanup;
        if(esp_http_client_get_url(http,response,2048)!=ESP_OK || strncmp(response,"https://",8)) goto cleanup;
        esp_http_client_close(http);
    }
    size_t received=0;
    while(current(job->gen)) {
        int n=esp_http_client_read(http,response,2048);
        if(n<0) goto cleanup;
        if(!n) break;
        received+=n;if(received>1024*1024) goto cleanup;
        size_t offset=0;
        while(offset<(size_t)n && current(job->gen))
            offset+=xStreamBufferSend(audio,response+offset,n-offset,pdMS_TO_TICKS(100));
    }
    success=received>0 && current(job->gen);
    if(current(job->gen)) ESP_LOGI("muse.tts","downloaded %u MP3 bytes",(unsigned)received);
cleanup:
    memset(auth,0,TTS_KEY_MAX+8);free(auth);
    if(http) esp_http_client_cleanup(http);
    free(response);free(body);free(url);
    if(current(job->gen)) {atomic_store(&failed,!success);atomic_store(&finished,true);
        if(!success) ESP_LOGW("muse.tts","speech synthesis/download failed");}
}
static void stream_media(job_t *job) {
    esp_http_client_handle_t http=NULL;FILE *file=NULL;bool success=false;
    uint8_t *buffer=heap_caps_malloc(2048,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!buffer) goto finish;
    if(!strncmp(job->text,"/sdcard/",8)) {file=fopen(job->text,"rb");if(!file) goto finish;}
    else {
        http=client(job->text);if(!http) goto finish;
        for(int redirects=0;redirects<4;redirects++) {
            if(esp_http_client_open(http,0)!=ESP_OK) goto finish;
            esp_http_client_fetch_headers(http);int status=esp_http_client_get_status_code(http);
            if(status==200) break;
            if(status<300 || status>=400 || redirects==3 || esp_http_client_set_redirection(http)!=ESP_OK) goto finish;
            esp_http_client_close(http);
        }
    }
    size_t total=0;
    while(current(job->gen)) {
        int n=file?(int)fread(buffer,1,2048,file):esp_http_client_read(http,(char *)buffer,2048);
        if(n<0) goto finish;
        if(!n) {success=total>0;break;}
        total+=n;if(total>64*1024*1024) goto finish;
        size_t off=0;
        while(off<(size_t)n && current(job->gen)) off+=xStreamBufferSend(audio,buffer+off,n-off,pdMS_TO_TICKS(100));
    }
finish:
    if(file) fclose(file);
    if(http) esp_http_client_cleanup(http);
    free(buffer);
    if(current(job->gen)) {atomic_store(&failed,!success);atomic_store(&finished,true);}
}
static void worker(void *unused) {
    (void)unused;job_t job;
    for(;;) if(xQueueReceive(jobs,&job,portMAX_DELAY)==pdTRUE) {
        if(current(job.gen)) {if(job.media) stream_media(&job);else synthesize(&job);}
        memset(&job,0,sizeof(job));
        atomic_store(&busy,false);
    }
}
static bool key_valid(const char *key) {
    if(!key || strlen(key)<8 || strlen(key)>=sizeof(secret)) return false;
    for(const unsigned char *p=(const unsigned char *)key;*p;p++) if(*p<=32 || *p>126) return false;
    return true;
}
static void apply(const muse_tts_config_t *config,const char *key) {
    portENTER_CRITICAL(&lock);options=*config;strcpy(secret,key);portEXIT_CRITICAL(&lock);
    atomic_store(&configured,true);
}
bool muse_tts_configure(const char *key) {
    muse_tts_config_t config;
    if(!key_valid(key) || !muse_tts_options(&config,NULL,NULL,NULL) || !muse_tts_init()) return false;
    apply(&config,key);return true;
}
bool muse_tts_setup(const char *provider,const char *model,const char *voice,const char *key) {
    muse_tts_config_t config;
    if(!key_valid(key) || !muse_tts_options(&config,provider,model,voice) || !muse_tts_init()) return false;
    cJSON *root=cJSON_CreateObject();if(!root) return false;
    cJSON_AddStringToObject(root,"provider",config.provider);cJSON_AddStringToObject(root,"model",config.model);
    cJSON_AddStringToObject(root,"voice",config.voice);cJSON_AddStringToObject(root,"key",key);
    char *json=cJSON_PrintUnformatted(root);
    bool ok=json && cJSON_GetArraySize(root)==4 && config_set_str("tts_config",json);
    if(ok) apply(&config,key);
    if(json) {memset(json,0,strlen(json));free(json);}
    char *copy=(char *)cJSON_GetStringValue(cJSON_GetObjectItem(root,"key"));
    if(copy) memset(copy,0,strlen(copy));
    cJSON_Delete(root);return ok;
}
void muse_tts_load_config(void) {
    char *stored=heap_caps_calloc(1,2048,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!stored) return;
    if(config_get_str("tts_config",stored,2048)) {
        cJSON *root=cJSON_Parse(stored);muse_tts_config_t config;
        const char *key=cJSON_GetStringValue(cJSON_GetObjectItem(root,"key"));
        if(key_valid(key) && muse_tts_options(&config,
            cJSON_GetStringValue(cJSON_GetObjectItem(root,"provider")),
            cJSON_GetStringValue(cJSON_GetObjectItem(root,"model")),
            cJSON_GetStringValue(cJSON_GetObjectItem(root,"voice")))) apply(&config,key);
        if(key) memset((char *)key,0,strlen(key));
        cJSON_Delete(root);
    } else if(config_get_str("tts_key",stored,2048)) muse_tts_configure(stored);
    memset(stored,0,2048);free(stored);
}
bool muse_tts_settings(char *provider,size_t pn,char *model,size_t mn,char *voice,size_t vn) {
    portENTER_CRITICAL(&lock);
    snprintf(provider,pn,"%s",options.provider[0]?options.provider:"qwen");
    snprintf(model,mn,"%s",options.model);snprintf(voice,vn,"%s",options.voice);
    portEXIT_CRITICAL(&lock);return atomic_load(&configured);
}
bool muse_tts_init(void) {
    if(jobs) return true;
    uint8_t *buffer=heap_caps_malloc(65537,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!buffer) return false;
    audio=xStreamBufferCreateStatic(65536,1,buffer,&audio_state);
    jobs=xQueueCreate(1,sizeof(job_t));
    if(!audio || !jobs || xTaskCreate(worker,"muse_tts",8192,NULL,3,NULL)!=pdPASS) {
        if(jobs) vQueueDelete(jobs);
        jobs=NULL;
        if(audio) vStreamBufferDelete(audio);
        audio=NULL;free(buffer);return false;
    }
    return true;
}
void muse_tts_cancel(void) {atomic_fetch_add(&generation,1);}
void muse_tts_cancel_session(void) {if(atomic_load(&session_owned)) muse_tts_cancel();}
static bool begin(const char *text,bool media,bool session) {
    if((!media && !atomic_load(&configured)) || !jobs || !text || !text[0] || strlen(text)>=1024) return false;
    // Do not reset a stream while its writer is active: consume stale bytes
    // before a new job and use generation to stop the old writer.
    bool expected=false;
    if(!atomic_compare_exchange_strong(&busy,&expected,true)) return false;
    atomic_store(&session_owned,session);
    muse_tts_cancel();
    uint8_t discard[256];while(xStreamBufferReceive(audio,discard,sizeof(discard),0));
    job_t job={.gen=atomic_load(&generation),.media=media};strcpy(job.text,text);
    atomic_store(&finished,false);atomic_store(&failed,false);
    xQueueOverwrite(jobs,&job);return true;
}
size_t muse_tts_read(uint8_t *out,size_t capacity,bool *done) {
    size_t n=audio?xStreamBufferReceive(audio,out,capacity,0):0;
    *done=atomic_load(&finished) && (!audio || !xStreamBufferBytesAvailable(audio));return n;
}

bool muse_tts_failed(void) {return atomic_load(&finished) && atomic_load(&failed);}

bool muse_tts_begin(const char *text) {return begin(text,false,true);}
bool muse_tts_say(const char *text) {return begin(text,false,false);}
bool muse_tts_media(const char *source) {return source && (!strncmp(source,"/sdcard/",8) || !strncmp(source,"https://",8) || !strncmp(source,"http://",7)) && begin(source,true,false);}
