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

#include "sensecap_sensors.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stack_monitor.h"

// ---- Protocol (host-tested) -------------------------------------------------

// Packets from Seeed's stock RP2040 firmware (SenseCAP_Indicator_RP2040): the
// type byte and a little-endian float, COBS-framed and ended by 0x00. Seeed
// names 0xB3 and 0xB4 after the SHT41, but fills them from the Grove AHT20.
#define PKT_CO2         0xB2
#define PKT_TEMPERATURE 0xB3
#define PKT_HUMIDITY    0xB4
#define PKT_TVOC        0xB5
#define PKT_LEN         5

// The RP2040 sends about every 5 s. Older readings count as missing, so an
// unplugged AHT20 drops out.
#define STALE_US (30LL * 1000 * 1000)

enum { SENSOR_CO2, SENSOR_TVOC, SENSOR_TEMPERATURE, SENSOR_HUMIDITY, SENSOR_COUNT };

static const struct {
    const char *name;
    const char *unit;
    int decimals;
} SENSORS[SENSOR_COUNT] = {
    [SENSOR_CO2] = {"co2", "ppm", 0},
    [SENSOR_TVOC] = {"tvoc", "index", 0},
    [SENSOR_TEMPERATURE] = {"temperature", "celsius", 1},
    [SENSOR_HUMIDITY] = {"humidity", "percent_rh", 1},
};

typedef struct {
    bool valid;
    float value;
    int64_t at_us;
} reading_t;

typedef struct {
    uint8_t buf[16];
    size_t len;
    bool overflow;
} framer_t;

// Collect bytes up to the next 0x00. Returns true when `byte` ends a frame,
// which is then f->buf[0..*frame_len). Oversized frames are dropped.
static bool framer_push(framer_t *f, uint8_t byte, size_t *frame_len) {
    if (byte != 0) {
        if (f->len < sizeof(f->buf)) f->buf[f->len++] = byte;
        else f->overflow = true;
        return false;
    }
    bool complete = f->len > 0 && !f->overflow;
    *frame_len = f->len;
    f->len = 0;
    f->overflow = false;
    return complete;
}

// Decode one COBS frame (without its 0x00). Returns the decoded length, or -1.
static int cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        uint8_t code = in[i++];
        if (code == 0 || i + code - 1 > len) return -1;
        for (uint8_t k = 1; k < code; k++) {
            if (o >= cap) return -1;
            out[o++] = in[i++];
        }
        if (code < 0xFF && i < len) {
            if (o >= cap) return -1;
            out[o++] = 0;
        }
    }
    return (int)o;
}

static bool parse_packet(const uint8_t *pkt, int len, int *sensor, float *value) {
    if (len != PKT_LEN) return false;
    switch (pkt[0]) {
        case PKT_CO2:         *sensor = SENSOR_CO2; break;
        case PKT_TEMPERATURE: *sensor = SENSOR_TEMPERATURE; break;
        case PKT_HUMIDITY:    *sensor = SENSOR_HUMIDITY; break;
        case PKT_TVOC:        *sensor = SENSOR_TVOC; break;
        default:              return false;
    }
    memcpy(value, &pkt[1], sizeof(*value));  // both ends are little-endian
    return isfinite(*value);
}

static cJSON *readings_result(const reading_t *readings, int64_t now_us) {
    cJSON *result = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    if (!result || !payload) {
        cJSON_Delete(result);
        cJSON_Delete(payload);
        return NULL;
    }
    bool any = false;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        const reading_t *r = &readings[i];
        if (!r->valid || now_us - r->at_us > STALE_US) {
            cJSON_AddNullToObject(payload, SENSORS[i].name);
            continue;
        }
        cJSON *sensor = cJSON_AddObjectToObject(payload, SENSORS[i].name);
        double scale = pow(10, SENSORS[i].decimals);
        cJSON_AddNumberToObject(sensor, "value", round(r->value * scale) / scale);
        cJSON_AddStringToObject(sensor, "unit", SENSORS[i].unit);
        cJSON_AddNumberToObject(sensor, "age_s", (double)((now_us - r->at_us) / 1000000));
        any = true;
    }
    if (!any) {
        cJSON_Delete(payload);
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON *error = cJSON_AddObjectToObject(result, "error");
        cJSON_AddStringToObject(error, "code", "no_readings");
        cJSON_AddStringToObject(error, "message",
                                "no sensor readings from the RP2040; the D1 and "
                                "D1L have no sensors");
        return result;
    }
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "payload", payload);
    return result;
}

// ---- RP2040 UART ------------------------------------------------------------

static const char *TAG = "link.sensors";

#define RP2040_UART    UART_NUM_2
#define RP2040_TX_GPIO 19
#define RP2040_RX_GPIO 20
#define RP2040_BAUD    115200

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static reading_t s_readings[SENSOR_COUNT];

static void sensors_task(void *arg) {
    (void)arg;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    framer_t framer = {0};
    uint8_t data[64];
    uint8_t pkt[sizeof(framer.buf)];
    bool seen[SENSOR_COUNT] = {0};
    for (;;) {
        int n = uart_read_bytes(RP2040_UART, data, sizeof(data), pdMS_TO_TICKS(1000));
        for (int i = 0; i < n; i++) {
            size_t frame_len;
            if (!framer_push(&framer, data[i], &frame_len)) continue;
            int len = cobs_decode(framer.buf, frame_len, pkt, sizeof(pkt));
            int sensor;
            float value;
            if (!parse_packet(pkt, len, &sensor, &value)) continue;
            taskENTER_CRITICAL(&s_lock);
            s_readings[sensor] = (reading_t){true, value, esp_timer_get_time()};
            taskEXIT_CRITICAL(&s_lock);
            if (!seen[sensor]) {
                seen[sensor] = true;
                ESP_LOGI(TAG, "first %s reading: %.1f %s", SENSORS[sensor].name,
                         value, SENSORS[sensor].unit);
            }
        }
        stack_monitor_poll(&stack);
    }
}

void sensecap_sensors_init(void) {
    const uart_config_t cfg = {
        .baud_rate = RP2040_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(RP2040_UART, 256, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(RP2040_UART, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(RP2040_UART, RP2040_TX_GPIO, RP2040_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RP2040 UART setup failed: %s", esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(sensors_task, "sensors", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the sensor task");
        return;
    }
    ESP_LOGI(TAG, "reading the RP2040's sensors on UART%d (TX=%d RX=%d)",
             RP2040_UART, RP2040_TX_GPIO, RP2040_RX_GPIO);
}

cJSON *sensecap_sensors_command(void) {
    reading_t snapshot[SENSOR_COUNT];
    taskENTER_CRITICAL(&s_lock);
    memcpy(snapshot, s_readings, sizeof(snapshot));
    taskEXIT_CRITICAL(&s_lock);
    return readings_result(snapshot, esp_timer_get_time());
}
