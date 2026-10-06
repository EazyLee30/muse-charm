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

// Host SenseCAP sensor harness: RP2040 frames in, sensors.read result out.
// The runner extracts the production protocol code into sensors_protocol.inc.
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

#include "sensors_protocol.inc"

#define S(x) (x * 1000000LL)

// COBS-encode a packet the way the RP2040's PacketSerial does, 0x00 included.
static size_t encode(const uint8_t *in, size_t len, uint8_t *out) {
    size_t code_at = 0, o = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == 0) {
            out[code_at] = code;
            code_at = o++;
            code = 1;
        } else {
            out[o++] = in[i];
            code++;
        }
    }
    out[code_at] = code;
    out[o++] = 0;
    return o;
}

static size_t frame(uint8_t type, float value, uint8_t *out) {
    uint8_t pkt[PKT_LEN] = {type};
    memcpy(&pkt[1], &value, sizeof(value));
    return encode(pkt, sizeof(pkt), out);
}

// Feed bytes through the production path, as sensors_task does.
static void feed(framer_t *f, reading_t *readings, const uint8_t *data, size_t n,
                 int64_t now_us) {
    for (size_t i = 0; i < n; i++) {
        size_t frame_len;
        if (!framer_push(f, data[i], &frame_len)) continue;
        uint8_t pkt[sizeof(f->buf)];
        int len = cobs_decode(f->buf, frame_len, pkt, sizeof(pkt));
        int sensor;
        float value;
        if (parse_packet(pkt, len, &sensor, &value)) {
            readings[sensor] = (reading_t){true, value, now_us};
        }
    }
}

static void check_decode(void) {
    // 400 ppm is 0x43C80000: two zero bytes inside the packet.
    uint8_t wire[16], pkt[16];
    size_t n = frame(PKT_CO2, 400.0f, wire);
    assert(wire[n - 1] == 0);
    int len = cobs_decode(wire, n - 1, pkt, sizeof(pkt));
    assert(len == PKT_LEN && pkt[0] == PKT_CO2);
    int sensor;
    float value;
    assert(parse_packet(pkt, len, &sensor, &value));
    assert(sensor == SENSOR_CO2 && value == 400.0f);

    // Malformed codes, short output buffers and wrong lengths are rejected.
    const uint8_t bad_code[] = {0x07, PKT_CO2, 1, 2};
    assert(cobs_decode(bad_code, sizeof(bad_code), pkt, sizeof(pkt)) == -1);
    assert(cobs_decode(wire, n - 1, pkt, 3) == -1);
    assert(!parse_packet(pkt, 4, &sensor, &value));
    assert(!parse_packet(pkt, -1, &sensor, &value));

    // Unknown types (Seeed's commands) and NaN are ignored.
    uint8_t other[PKT_LEN] = {0xA1};
    assert(!parse_packet(other, PKT_LEN, &sensor, &value));
    float nan_value = NAN;
    uint8_t nan_pkt[PKT_LEN] = {PKT_TVOC};
    memcpy(&nan_pkt[1], &nan_value, sizeof(nan_value));
    assert(!parse_packet(nan_pkt, PKT_LEN, &sensor, &value));
}

static void check_framing(void) {
    framer_t f = {0};
    reading_t r[SENSOR_COUNT] = {0};
    uint8_t wire[64];
    size_t n = 0;
    n += frame(PKT_TEMPERATURE, 23.46f, wire + n);
    n += frame(PKT_HUMIDITY, 41.04f, wire + n);

    // Split mid-frame across reads, with stray delimiters in between.
    const uint8_t zeros[] = {0, 0};
    feed(&f, r, zeros, sizeof(zeros), S(1));
    feed(&f, r, wire, 3, S(1));
    feed(&f, r, wire + 3, n - 3, S(1));
    assert(r[SENSOR_TEMPERATURE].valid && r[SENSOR_TEMPERATURE].value == 23.46f);
    assert(r[SENSOR_HUMIDITY].valid && r[SENSOR_HUMIDITY].value == 41.04f);

    // An oversized frame (line noise) is dropped and the next one still parses.
    uint8_t noise[41];
    memset(noise, 0x55, sizeof(noise) - 1);
    noise[sizeof(noise) - 1] = 0;
    feed(&f, r, noise, sizeof(noise), S(2));
    n = frame(PKT_TVOC, 104.0f, wire);
    feed(&f, r, wire, n, S(2));
    assert(!r[SENSOR_CO2].valid);
    assert(r[SENSOR_TVOC].valid && r[SENSOR_TVOC].value == 104.0f);
}

static void check_result(void) {
    reading_t r[SENSOR_COUNT] = {0};

    // Nothing yet (or a D1): an error, not an all-null payload.
    cJSON *result = readings_result(r, S(10));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result, "ok")));
    cJSON *error = cJSON_GetObjectItem(result, "error");
    assert(!strcmp(cJSON_GetObjectItem(error, "code")->valuestring, "no_readings"));
    assert(!cJSON_GetObjectItem(result, "payload"));
    cJSON_Delete(result);

    // A D1S without the AHT20: CO2 and tVOC, temperature and humidity null.
    r[SENSOR_CO2] = (reading_t){true, 612.0f, S(7)};
    r[SENSOR_TVOC] = (reading_t){true, 103.6f, S(9)};
    r[SENSOR_TEMPERATURE] = (reading_t){true, 23.46f, S(10) - STALE_US - 1};
    result = readings_result(r, S(10));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "ok")));
    // wrap_result_json forwards only ok, payload and error.
    int keys = 0;
    for (cJSON *item = result->child; item; item = item->next) keys++;
    assert(keys == 2);
    cJSON *payload = cJSON_GetObjectItem(result, "payload");
    cJSON *co2 = cJSON_GetObjectItem(payload, "co2");
    assert(cJSON_GetObjectItem(co2, "value")->valuedouble == 612);
    assert(!strcmp(cJSON_GetObjectItem(co2, "unit")->valuestring, "ppm"));
    assert(cJSON_GetObjectItem(co2, "age_s")->valuedouble == 3);
    cJSON *tvoc = cJSON_GetObjectItem(payload, "tvoc");
    assert(cJSON_GetObjectItem(tvoc, "value")->valuedouble == 104);
    assert(cJSON_IsNull(cJSON_GetObjectItem(payload, "temperature")));
    assert(cJSON_IsNull(cJSON_GetObjectItem(payload, "humidity")));
    cJSON_Delete(result);

    // Temperature and humidity round to one decimal and print cleanly.
    r[SENSOR_TEMPERATURE] = (reading_t){true, 23.46f, S(10)};
    r[SENSOR_HUMIDITY] = (reading_t){true, 41.04f, S(10)};
    result = readings_result(r, S(10));
    char *json = cJSON_PrintUnformatted(cJSON_GetObjectItem(result, "payload"));
    assert(strstr(json, "\"temperature\":{\"value\":23.5,\"unit\":\"celsius\",\"age_s\":0}"));
    assert(strstr(json, "\"humidity\":{\"value\":41,\"unit\":\"percent_rh\",\"age_s\":0}"));
    cJSON_free(json);
    cJSON_Delete(result);
}

int main(void) {
    check_decode(); check_framing(); check_result();
    puts("PASS sensecap sensors: COBS frames with zero bytes, split and noisy reads, bad frames and types, stale and missing sensors, rounding, result envelope");
    return 0;
}
