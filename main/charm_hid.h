#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {uint16_t bit; uint8_t size,id,button; bool hat; int32_t minimum;} charm_hid_field_t;
typedef struct {charm_hid_field_t fields[32]; unsigned count;} charm_hid_map_t;
bool charm_hid_parse(charm_hid_map_t *map,const uint8_t *data,size_t length);
bool charm_hid_input(const charm_hid_map_t *map,uint8_t id,const uint8_t *data,size_t length,uint16_t *buttons,int *hat);
