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

#include <assert.h>
#include <stdint.h>

#include "noise_control.h"

int main(void) {
    assert(noise_ctrl_next_session_generation(0) == 1);
    assert(noise_ctrl_next_session_generation(1) == 2);
    assert(noise_ctrl_next_session_generation(UINT64_MAX) == 1);

    assert(!noise_ctrl_session_is_current(0, 1));
    assert(!noise_ctrl_session_is_current(1, 0));
    assert(!noise_ctrl_session_is_current(6, 7));
    assert(noise_ctrl_session_is_current(7, 7));
    const uint64_t start = 9000000000ULL;
    assert(!noise_ctrl_heartbeat_due(false, false, start, 0));
    assert(noise_ctrl_heartbeat_due(true, false, start, 0));
    assert(!noise_ctrl_heartbeat_due(true, true, start, start));
    assert(!noise_ctrl_heartbeat_due(true, true, start + 1800000000ULL, start));
    assert(!noise_ctrl_heartbeat_due(true, true, start + 86399999999ULL, start));
    assert(noise_ctrl_heartbeat_due(true, true, start + 86400000000ULL, start));
    assert(noise_ctrl_heartbeat_due(true, true, start + 86400000001ULL, start));
    assert(!noise_ctrl_heartbeat_due(false, true, start + 86400000000ULL, start));
    // Reconnecting requires a fresh ACK, then sends promptly rather than waiting.
    assert(!noise_ctrl_heartbeat_due(false, false, start + 1, 0));
    assert(noise_ctrl_heartbeat_due(true, false, start + 1, 0));
    return 0;
}
