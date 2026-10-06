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
#include <stdio.h>
#include <string.h>

// Construction and classification for the WebSocket upgrade, split out so host
// tests can exercise it without the ESP-IDF/TLS transport.

// How the session loop must treat an upgrade response.
typedef enum {
    NOISE_UPGRADE_OK = 0,
    NOISE_UPGRADE_FAILED,
    NOISE_UPGRADE_AUTH_REJECTED,
} noise_upgrade_result_t;

// Worst-case output size for noise_upgrade_query_escape(), including the NUL.
#define NOISE_UPGRADE_ESCAPED_CAP(n) (3 * (n) + 1)

// Percent-encode `in` into `out`, leaving unescaped exactly the set
// JavaScript's encodeURIComponent() leaves unescaped:
// A-Z a-z 0-9 - _ . ! ~ * ' ( )
//
// For the values Link actually sends — a UUID vm_id — every byte is already in
// that set, so this is a no-op on the wire. It is here for the one that is
// not: `vm_id` arrives from fetch_vms and is spliced into the request target,
// and an unescaped '&', '?', '#' or space there stops being payload and starts
// being query structure. The edge proxy uses vm_id for routing and token
// validation, so the value must not be able to introduce query parameters.
//
// Returns bytes written excluding the NUL, or -1 if `out` is too small.
static inline int noise_upgrade_query_escape(const char *in, char *out,
                                             size_t out_cap) {
    static const char kHex[] = "0123456789ABCDEF";
    if (!in || !out || out_cap == 0) return -1;
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                          || (c >= '0' && c <= '9')
                          || strchr("-_.!~*'()", (int)c) != NULL;
        if (unreserved) {
            if (w + 1 >= out_cap) return -1;
            out[w++] = (char)c;
        } else {
            if (w + 3 >= out_cap) return -1;
            out[w++] = '%';
            out[w++] = kHex[c >> 4];
            out[w++] = kHex[c & 0x0Fu];
        }
    }
    out[w] = '\0';
    return (int)w;
}

// Build the WebSocket upgrade request.
//
// The bearer goes in an Authorization header, not the request target, so it
// stays out of request URLs recorded by proxies and monitoring systems. The
// edge must receive the exact token bytes returned by fetch_vms for this VM;
// the client must not trim or re-encode them. Reject CR/LF before inserting
// the token into the header so a server-supplied value cannot inject headers.
//
// vm_id is escaped instead, because it lands in the request target that the
// edge routes on.
//
// Returns the request length, or -1 if anything is missing, contains CR/LF, or
// will not fit. Never writes a partial request.
static inline int noise_upgrade_build_request(const char *path,
                                              const char *vm_id,
                                              const char *host,
                                              const char *token,
                                              char *out, size_t out_cap) {
    if (!path || !vm_id || !host || !token || !out) return -1;
    if (!*vm_id || !*token) return -1;
    if (strpbrk(token, "\r\n") || strpbrk(vm_id, "\r\n")) return -1;
    if (strpbrk(host, "\r\n")) return -1;

    char vm_id_enc[NOISE_UPGRADE_ESCAPED_CAP(128)];
    if (noise_upgrade_query_escape(vm_id, vm_id_enc, sizeof(vm_id_enc)) < 0) {
        return -1;
    }

    int n = snprintf(out, out_cap,
        "GET %s?vm_id=%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n",
        path, vm_id_enc, host, token);
    if (n < 0 || (size_t)n >= out_cap) return -1;
    return n;
}

// Parse the 3-digit status out of an HTTP response head. Returns -1 when we do
// not hold enough bytes, the response is not HTTP, or the digits are not
// digits — callers must treat -1 as "unknown", never as a rejection.
static inline int noise_upgrade_status_code(const char *hdr, size_t have) {
    if (!hdr || have < 12 || memcmp(hdr, "HTTP/", 5) != 0) return -1;
    const char *s = hdr + 9;
    for (int i = 0; i < 3; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
    }
    return (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
}

// A 401/403 on the upgrade is the edge refusing our `vm_auth_token`, not a
// network fault. The edge proxy validates the JWT before forwarding the
// connection to the VM. An expired token or one issued for another VM is
// rejected at the upgrade, before any VM session can be established.
//
// The session loop has to call that an auth failure. Classifying it as a
// generic transport failure costs two things: the reconnect backoff stays at
// the 15s network cap instead of RECONNECT_AUTH_FAILED_MS, and no
// `ws_auth_failed` status fires, so app.c never re-fetches the token from
// fetch_vms. A device the edge persistently refuses would then retry a token
// it has already been told is bad, four times a minute, indefinitely — fleet
// multiplied, that is load on the pre-authentication surface this admission
// layer exists to shrink.
static inline bool noise_upgrade_status_is_auth_rejection(int status) {
    return status == 401 || status == 403;
}

// Classify an upgrade response head. `overflowed` is true when the response
// outgrew the caller's buffer before the header terminator arrived — the edge
// attaches a ~1.4 KB RFC 9209 Proxy-Status header to errors, so a 401 can do
// that. The status line arrived first either way, so it still classifies.
static inline noise_upgrade_result_t noise_upgrade_classify(const char *hdr,
                                                            size_t have,
                                                            bool overflowed) {
    int status = noise_upgrade_status_code(hdr, have);
    if (noise_upgrade_status_is_auth_rejection(status)) {
        return NOISE_UPGRADE_AUTH_REJECTED;
    }
    if (!overflowed && status == 101) return NOISE_UPGRADE_OK;
    return NOISE_UPGRADE_FAILED;
}
