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

#include "muse_adpcm.h"

static const int16_t STEPS[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
    97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static const int8_t INDEX_STEP[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* Applies a code to the state; the encoder and decoder share this so they never drift apart. */
static int16_t step(muse_adpcm_t *s, uint8_t code)
{
    int st = STEPS[s->index];
    int diff = st >> 3;
    if (code & 4) {
        diff += st;
    }
    if (code & 2) {
        diff += st >> 1;
    }
    if (code & 1) {
        diff += st >> 2;
    }
    int pred = s->pred + (code & 8 ? -diff : diff);
    s->pred = (int16_t)(pred > 32767 ? 32767 : (pred < -32768 ? -32768 : pred));
    int index = s->index + INDEX_STEP[code & 7];
    s->index = (int8_t)(index < 0 ? 0 : (index > 88 ? 88 : index));
    return s->pred;
}

static uint8_t encode(muse_adpcm_t *s, int16_t sample)
{
    int diff = sample - s->pred;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    int st = STEPS[s->index];
    if (diff >= st) {
        code |= 4;
        diff -= st;
    }
    if (diff >= st >> 1) {
        code |= 2;
        diff -= st >> 1;
    }
    if (diff >= st >> 2) {
        code |= 1;
    }
    step(s, code);
    return code;
}

void muse_adpcm_encode_block(muse_adpcm_t *s, const int16_t *pcm, size_t n, uint8_t *out)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint8_t lo = encode(s, pcm[i]);
        out[i / 2] = lo | encode(s, pcm[i + 1]) << 4;
    }
}

void muse_adpcm_decode_block(muse_adpcm_t *s, const uint8_t *in, size_t n, int16_t *pcm)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        pcm[i] = step(s, in[i / 2] & 0x0f);
        pcm[i + 1] = step(s, in[i / 2] >> 4);
    }
}
