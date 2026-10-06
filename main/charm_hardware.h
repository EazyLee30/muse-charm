#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
void charm_hardware_start(void);
cJSON *charm_storage_command(const char *command,cJSON *params);
cJSON *charm_sensors(void);
bool charm_storage_path(const char *relative,char *out,size_t capacity);
