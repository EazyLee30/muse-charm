#pragma once
#include <stdbool.h>
#include <stddef.h>
// Small explicit zone set plus UTC offsets; no full IANA database on device.
bool charm_timezone_resolve(const char *name,char *posix,size_t size);
