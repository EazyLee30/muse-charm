#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Configure from an internal-stack task. Credentials are never logged.
bool muse_tts_configure(const char *key);
bool muse_tts_init(void);
bool muse_tts_setup(const char *provider,const char *model,const char *voice,const char *key);
void muse_tts_load_config(void);
bool muse_tts_settings(char *provider,size_t pn,char *model,size_t mn,char *voice,size_t vn);
bool muse_tts_begin(const char *text);
size_t muse_tts_read(uint8_t *out, size_t capacity, bool *done);
void muse_tts_cancel(void);
// Ending a chat must not cancel audio started by a hardware command.
void muse_tts_cancel_session(void);
bool muse_tts_say(const char *text);
bool muse_tts_failed(void);
bool muse_tts_media(const char *source);
#ifdef __cplusplus
}
#endif
