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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The SPI transport of Seeed's SSCMA protocol (the SenseCAP Watcher's Himax
 * camera, the Grove Vision AI V2), as Seeed's sscma_client speaks it: every
 * transfer from the host starts with one fixed-size packet
 *   0x10, command, length (big-endian), payload, 0xFF 0xFF, zeros
 * Writes carry up to SSCMA_MAX_PAYLOAD bytes of an AT command each; a read or
 * an "available" query carries no payload, just the length it asks for, and
 * the chip's bytes follow in a separate transfer. Replies are JSON objects
 * framed "\r{...}\n", padded with NULs.
 */

#define SSCMA_PACKET 256
#define SSCMA_MAX_PAYLOAD 250
#define SSCMA_MAX_READ 4095

#define SSCMA_CMD_READ 0x01
#define SSCMA_CMD_WRITE 0x02
#define SSCMA_CMD_AVAILABLE 0x03

#define SSCMA_TYPE_RESPONSE 0
#define SSCMA_TYPE_EVENT 1
#define SSCMA_TYPE_LOG 2

/* One packet: `payload` (len <= SSCMA_MAX_PAYLOAD) for a write, or NULL and
 * the length asked for (<= SSCMA_MAX_READ) for a read or an available query. */
void sscma_packet(uint8_t out[SSCMA_PACKET], uint8_t cmd, const uint8_t *payload, uint16_t len);

/* Replies put back together from what the chip sends. */
typedef struct {
    char *buf;
    size_t len, cap;
    size_t max;       /* the most it holds: a reply with an image is tens of KB */
    size_t used;      /* the reply sscma_rx_next() returned last, dropped on the next call */
} sscma_rx_t;

/* Appends the chip's bytes, dropping its NUL padding. False if that would pass
 * `max` or memory ran out: what's held is dropped, so the next reply starts clean. */
bool sscma_rx_feed(sscma_rx_t *rx, const uint8_t *data, size_t n);

/* The next whole reply, as NUL-terminated JSON ("{...}") inside the buffer,
 * or NULL. Valid until the next call to either function. */
char *sscma_rx_next(sscma_rx_t *rx);

void sscma_rx_free(sscma_rx_t *rx);

#ifdef __cplusplus
}
#endif
