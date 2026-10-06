#pragma once
#include <stdbool.h>
#include "cJSON.h"
typedef struct {char provider[24],model[48],voice[128];} muse_tts_config_t;
bool muse_tts_options(muse_tts_config_t *out,const char *provider,const char *model,const char *voice);
const char *muse_tts_endpoint(const muse_tts_config_t *config);
cJSON *muse_tts_request(const muse_tts_config_t *config,const char *text);
const char *muse_tts_audio_url(const muse_tts_config_t *config,cJSON *response);
