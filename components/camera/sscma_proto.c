/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sscma_proto.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
/* Replies with images are big: keep them out of internal RAM. */
#define rx_realloc(p, n) heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define rx_realloc(p, n) realloc((p), (n))
#endif

#define PREFIX "\r{"
#define SUFFIX "}\n"

void sscma_packet(uint8_t out[SSCMA_PACKET], uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    memset(out, 0, SSCMA_PACKET);
    out[0] = 0x10;   /* the transport feature */
    out[1] = cmd;
    out[2] = len >> 8;
    out[3] = len & 0xff;
    size_t end = 4;
    if (payload) {
        memcpy(out + 4, payload, len);
        end += len;
    }
    out[end] = 0xff;   /* the checksum field, which the chip doesn't check */
    out[end + 1] = 0xff;
}

static void drop_used(sscma_rx_t *rx)
{
    if (!rx->used) {
        return;
    }
    memmove(rx->buf, rx->buf + rx->used, rx->len - rx->used);
    rx->len -= rx->used;
    rx->used = 0;
}

bool sscma_rx_feed(sscma_rx_t *rx, const uint8_t *data, size_t n)
{
    drop_used(rx);
    if (rx->len + n + 1 > rx->max) {
        rx->len = 0;
        return false;
    }
    if (rx->len + n + 1 > rx->cap) {
        size_t cap = rx->cap ? rx->cap : 1024;
        while (cap < rx->len + n + 1) {
            cap *= 2;
        }
        cap = cap < rx->max ? cap : rx->max;
        char *buf = rx_realloc(rx->buf, cap);
        if (!buf) {
            rx->len = 0;
            return false;
        }
        rx->buf = buf;
        rx->cap = cap;
    }
    for (size_t i = 0; i < n; i++) {
        if (data[i]) {
            rx->buf[rx->len++] = (char)data[i];
        }
    }
    return true;
}

static char *find(char *from, size_t n, const char *what)
{
    size_t k = strlen(what);
    for (size_t i = 0; i + k <= n; i++) {
        if (!memcmp(from + i, what, k)) {
            return from + i;
        }
    }
    return NULL;
}

char *sscma_rx_next(sscma_rx_t *rx)
{
    drop_used(rx);
    char *start = find(rx->buf, rx->len, PREFIX);
    if (!start) {
        /* Nothing that starts a reply: keep only a trailing "\r" that might. */
        bool cr = rx->len && rx->buf[rx->len - 1] == '\r';
        rx->used = rx->len - (cr ? 1 : 0);
        return NULL;
    }
    rx->used = start - rx->buf;   /* what came before it isn't a reply */
    char *end = find(start, rx->len - rx->used, SUFFIX);
    if (!end) {
        return NULL;
    }
    end[1] = '\0';   /* over the '\n' */
    rx->used = end + 2 - rx->buf;
    return start + 1;
}

void sscma_rx_free(sscma_rx_t *rx)
{
    free(rx->buf);
    *rx = (sscma_rx_t){ .max = rx->max };
}
