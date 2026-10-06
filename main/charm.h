#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

typedef enum { CHARM_CALM, CHARM_PET, CHARM_DANCE, CHARM_WAVE, CHARM_SLEEP, CHARM_HOP, CHARM_PEEK, CHARM_STRETCH } charm_reaction_t;
typedef struct {
    bool dark, wifi, pad_connected, pad_pairing, key_pressed, boot_pressed;
    int battery_pct, battery_mv, offset_x, rotation;
    char clock[6], location[64], timezone[48];
    charm_reaction_t reaction;
    float reaction_t;
} charm_view_t;
void charm_start(void);
void charm_snapshot(charm_view_t *out);
cJSON *charm_command(const char *command, cJSON *params);
void charm_buttons(bool key,bool boot);
void charm_react(charm_reaction_t reaction);
void charm_pad_input(uint16_t buttons, int hat);
void charm_pad_status(bool connected, bool pairing);
void charm_ui_set_dark(bool dark);
void charm_controller_pair(void);
void charm_controller_disconnect(void);

void charm_controller_resume(void);

void charm_ui_rotate(int rotation);
void charm_ui_caption(const char *text);
