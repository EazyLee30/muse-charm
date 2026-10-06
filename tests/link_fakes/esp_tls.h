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

#include <stddef.h>
#include <sys/types.h>

#define ESP_OK 0
#define ESP_TLS_ERR_SSL_WANT_READ (-6900)
#define ESP_TLS_ERR_SSL_WANT_WRITE (-6901)

typedef int esp_err_t;

typedef struct esp_tls {
    int unused;
} esp_tls_t;

typedef struct {
    void *crt_bundle_attach;
    int timeout_ms;
} esp_tls_cfg_t;

static inline esp_tls_t *esp_tls_init(void) {
    return NULL;
}

static inline int esp_tls_conn_new_sync(const char *host, int hostlen, int port,
                                        const esp_tls_cfg_t *cfg,
                                        esp_tls_t *tls) {
    (void)host;
    (void)hostlen;
    (void)port;
    (void)cfg;
    (void)tls;
    return -1;
}

static inline ssize_t esp_tls_conn_write(esp_tls_t *tls, const void *data,
                                         size_t datalen) {
    (void)tls;
    (void)data;
    (void)datalen;
    return -1;
}

static inline ssize_t esp_tls_conn_read(esp_tls_t *tls, void *data,
                                        size_t datalen) {
    (void)tls;
    (void)data;
    (void)datalen;
    return -1;
}

static inline void esp_tls_conn_destroy(esp_tls_t *tls) {
    (void)tls;
}

static inline esp_err_t esp_tls_get_conn_sockfd(esp_tls_t *tls, int *sockfd) {
    (void)tls;
    if (sockfd) *sockfd = -1;
    return -1;
}
