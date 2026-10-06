// Native TTS contracts. MiniMax uses non-streaming URL output to bound RAM.
#include "muse_tts_config.h"
#include <string.h>
#include <stdio.h>
static bool identifier(const char *s,size_t max) {
    if(!s || !*s || strlen(s)>=max) return false;
    for(const unsigned char *p=(const unsigned char *)s;*p;p++) if(*p<32 || *p>126) return false;
    return true;
}
bool muse_tts_options(muse_tts_config_t *out,const char *provider,const char *model,const char *voice) {
    if(!provider) provider="qwen";
    bool qwen=!strcmp(provider,"qwen");
    if(!qwen && strcmp(provider,"minimax-cn") && strcmp(provider,"minimax-global")) return false;
    if(!model || !*model) model=qwen?"qwen-audio-3.0-tts-plus":"speech-2.8-turbo";
    if(!voice || !*voice) voice=qwen?"longanhuan_v3.6":"male-qn-qingse";
    if(!identifier(model,sizeof(out->model)) || !identifier(voice,sizeof(out->voice))) return false;
    if(qwen && strcmp(model,"qwen-audio-3.0-tts-plus")) return false;
    if(!qwen) {
        const char *models[]={"speech-2.8-turbo","speech-2.8-hd","speech-2.6-turbo","speech-2.6-hd","speech-02-turbo","speech-02-hd","speech-01-turbo","speech-01-hd"};
        bool found=false;for(size_t i=0;i<sizeof(models)/sizeof(models[0]);i++) if(!strcmp(model,models[i])) found=true;
        if(!found) return false;
    }
    snprintf(out->provider,sizeof(out->provider),"%s",provider);
    snprintf(out->model,sizeof(out->model),"%s",model);snprintf(out->voice,sizeof(out->voice),"%s",voice);return true;
}
const char *muse_tts_endpoint(const muse_tts_config_t *config) {
    if(!strcmp(config->provider,"minimax-cn")) return "https://api.minimax.cn/v1/t2a_v2";
    if(!strcmp(config->provider,"minimax-global")) return "https://api.minimax.io/v1/t2a_v2";
    return "https://token-plan.cn-beijing.maas.aliyuncs.com/api/v1/services/audio/tts/SpeechSynthesizer";
}
cJSON *muse_tts_request(const muse_tts_config_t *config,const char *text) {
    cJSON *root=cJSON_CreateObject();if(!root) return NULL;
    cJSON_AddStringToObject(root,"model",config->model);
    if(!strcmp(config->provider,"qwen")) {
        cJSON *input=cJSON_AddObjectToObject(root,"input");
        cJSON_AddStringToObject(input,"text",text);cJSON_AddStringToObject(input,"voice",config->voice);
        cJSON_AddStringToObject(input,"format","mp3");cJSON_AddNumberToObject(input,"sample_rate",16000);
    } else {
        cJSON_AddStringToObject(root,"text",text);cJSON_AddBoolToObject(root,"stream",false);
        cJSON_AddStringToObject(root,"output_format","url");cJSON_AddStringToObject(root,"language_boost","auto");
        cJSON *voice=cJSON_AddObjectToObject(root,"voice_setting");
        cJSON_AddStringToObject(voice,"voice_id",config->voice);cJSON_AddNumberToObject(voice,"speed",1);
        cJSON_AddNumberToObject(voice,"vol",1);cJSON_AddNumberToObject(voice,"pitch",0);
        cJSON *audio=cJSON_AddObjectToObject(root,"audio_setting");
        cJSON_AddStringToObject(audio,"format","mp3");cJSON_AddNumberToObject(audio,"sample_rate",16000);
        cJSON_AddNumberToObject(audio,"bitrate",128000);cJSON_AddNumberToObject(audio,"channel",1);
    }
    return root;
}
const char *muse_tts_audio_url(const muse_tts_config_t *config,cJSON *response) {
    const char *url=NULL;
    if(!strcmp(config->provider,"qwen")) {
        cJSON *audio=cJSON_GetObjectItem(cJSON_GetObjectItem(response,"output"),"audio");
        url=cJSON_GetStringValue(cJSON_GetObjectItem(audio,"url"));
    } else {
        cJSON *code=cJSON_GetObjectItem(cJSON_GetObjectItem(response,"base_resp"),"status_code");
        if(!cJSON_IsNumber(code) || code->valuedouble!=0) return NULL;
        url=cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(response,"data"),"audio"));
    }
    return url && (!strncmp(url,"https://",8) || !strncmp(url,"http://",7)) && strlen(url)<2048?url:NULL;
}
