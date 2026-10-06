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

#include <fcntl.h>

#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif

#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

static inline int lwip_fcntl(int fd, int cmd, int arg) {
    (void)fd;
    (void)cmd;
    (void)arg;
    return -1;
}

static inline int lwip_setsockopt(int socket, int level, int option_name,
                                  const void *option_value,
                                  unsigned option_len) {
    (void)socket;
    (void)level;
    (void)option_name;
    (void)option_value;
    (void)option_len;
    return 0;
}
