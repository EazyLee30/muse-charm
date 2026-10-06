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

/* components/camera/sscma_proto.c for test_camera.py: SSCMA packets byte for
 * byte as Seeed's sscma_client sends them, and replies put back together from
 * reads that split them, pad them with NULs and interleave junk. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sscma_proto.h"

static void feed_str(sscma_rx_t *rx, const char *s)
{
    assert(sscma_rx_feed(rx, (const uint8_t *)s, strlen(s)));
}

static void packets(void)
{
    uint8_t p[SSCMA_PACKET];
    const char *at = "AT+SAMPLE=1\r\n";
    sscma_packet(p, SSCMA_CMD_WRITE, (const uint8_t *)at, 13);
    const uint8_t head[] = { 0x10, 0x02, 0x00, 13 };
    assert(!memcmp(p, head, 4) && !memcmp(p + 4, at, 13));
    assert(p[17] == 0xff && p[18] == 0xff);
    for (int i = 19; i < SSCMA_PACKET; i++) {
        assert(p[i] == 0);
    }

    uint8_t full[SSCMA_MAX_PAYLOAD];
    memset(full, 'x', sizeof(full));
    sscma_packet(p, SSCMA_CMD_WRITE, full, SSCMA_MAX_PAYLOAD);
    assert(p[3] == 250 && p[254] == 0xff && p[255] == 0xff);   /* ends exactly at the packet's end */

    sscma_packet(p, SSCMA_CMD_READ, NULL, SSCMA_MAX_READ);
    const uint8_t read[] = { 0x10, 0x01, 0x0f, 0xff, 0xff, 0xff, 0x00 };
    assert(!memcmp(p, read, sizeof(read)));

    sscma_packet(p, SSCMA_CMD_AVAILABLE, NULL, 0);
    const uint8_t avail[] = { 0x10, 0x03, 0x00, 0x00, 0xff, 0xff, 0x00 };
    assert(!memcmp(p, avail, sizeof(avail)));
}

static void replies(void)
{
    sscma_rx_t rx = { .max = 4096 };
    assert(!sscma_rx_next(&rx));   /* nothing yet */

    /* NUL padding, junk before a reply, and a reply split over three reads. */
    const uint8_t padded[] = { 0, 0, 'j', 'u', 'n', 'k', '\r', '{', '"', 't', 0, 0 };
    assert(sscma_rx_feed(&rx, padded, sizeof(padded)));
    assert(!sscma_rx_next(&rx));
    feed_str(&rx, "ype\":0,\"name\":\"SENSOR\",\"code\":0}");
    assert(!sscma_rx_next(&rx));   /* no "}\n" yet */
    feed_str(&rx, "\n");
    char *j = sscma_rx_next(&rx);
    assert(j && !strcmp(j, "{\"type\":0,\"name\":\"SENSOR\",\"code\":0}"));
    assert(!sscma_rx_next(&rx));

    /* Two replies in one read, nested braces, and a "\r" split from its "{". */
    feed_str(&rx, "\r{\"type\":0,\"name\":\"SAMPLE\",\"code\":0}\nlog line\r{\"type\":1,\"data\":{\"image\":\"/9j/\"}}\n\r");
    assert(!strcmp(sscma_rx_next(&rx), "{\"type\":0,\"name\":\"SAMPLE\",\"code\":0}"));
    assert(!strcmp(sscma_rx_next(&rx), "{\"type\":1,\"data\":{\"image\":\"/9j/\"}}"));
    assert(!sscma_rx_next(&rx));
    feed_str(&rx, "{\"type\":2}\n");
    assert(!strcmp(sscma_rx_next(&rx), "{\"type\":2}"));

    /* A reply past max is dropped whole, and the next one comes through. */
    char *big = malloc(5000);
    memset(big, 'A', 4999);
    big[4999] = '\0';
    feed_str(&rx, "\r{\"image\":\"");
    assert(!sscma_rx_feed(&rx, (const uint8_t *)big, 4999));
    feed_str(&rx, "\"}\n\r{\"ok\":1}\n");
    assert(!strcmp(sscma_rx_next(&rx), "{\"ok\":1}"));
    free(big);

    /* Grows past its first allocation for a reply up to max. */
    sscma_rx_t grow = { .max = 64 * 1024 };
    feed_str(&grow, "\r{\"image\":\"");
    char *img = malloc(40000);
    memset(img, 'B', 39999);
    img[39999] = '\0';
    feed_str(&grow, img);
    feed_str(&grow, "\"}\n");
    j = sscma_rx_next(&grow);
    assert(j && strlen(j) == 39999 + strlen("{\"image\":\"\"}"));
    free(img);

    sscma_rx_free(&rx);
    sscma_rx_free(&grow);
    assert(grow.max == 64 * 1024 && !grow.buf && !grow.len);
}

int main(void)
{
    packets();
    replies();
    puts("ok");
    return 0;
}
