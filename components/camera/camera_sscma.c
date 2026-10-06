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

/* The sequence and timings follow Seeed's sscma_client (SenseCAP-Watcher-Firmware,
 * Apache-2.0): reset held 100 ms then 200 ms to start, 2 ms between transfers,
 * 12 MHz SPI mode 0, and the image in the SAMPLE event after the response.
 * Unlike there, it doesn't wait for an INIT@STAT event, which the Watcher's
 * chip didn't send when tested. */

#include "camera_sscma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sscma_proto.h"

static const char *TAG = "camera_sscma";

#define SPI_HZ (12 * 1000 * 1000)
#define GAP_MS 2                 /* between transfers, as Seeed's client waits */
#define RESET_HOLD_MS 100
#define RESET_START_MS 200
#define BOOT_MS 5000             /* power-up to its first answer */
#define BOOT_RETRY_MS 250        /* asks again this often while it boots */
#define REPLY_MS 3000
#define SAMPLE_MS 8000           /* the SAMPLE event, with the image */
#define POLL_MS 10
#define RX_MAX (512 * 1024)      /* a reply with a 640x480 JPEG in base64 fits well under this */

static camera_sscma_config_t s_cfg;
static spi_device_handle_t s_dev;
static uint8_t *s_tx;            /* one packet, DMA-capable, while powered */
static uint8_t *s_rx;            /* one read, DMA-capable, while powered */
static sscma_rx_t s_replies = { .max = RX_MAX };

static void power_down(void);
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_awake;   /* light sleep would stop the bus mid-capture */
#endif

static esp_err_t transfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    vTaskDelay(pdMS_TO_TICKS(GAP_MS));
    spi_transaction_t t = {
        .length = n * 8,
        .rxlength = rx ? n * 8 : 0,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_transmit(s_dev, &t);
}

static esp_err_t send_packet(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    sscma_packet(s_tx, cmd, payload, len);
    return transfer(s_tx, NULL, SSCMA_PACKET);
}

static esp_err_t command(const char *at)
{
    size_t n = strlen(at);
    for (size_t off = 0; off < n; off += SSCMA_MAX_PAYLOAD) {
        size_t part = n - off < SSCMA_MAX_PAYLOAD ? n - off : SSCMA_MAX_PAYLOAD;
        ESP_RETURN_ON_ERROR(send_packet(SSCMA_CMD_WRITE, (const uint8_t *)at + off, part), TAG, "write");
    }
    return ESP_OK;
}

/* Moves what the chip has waiting into s_replies; *got is how much. */
static esp_err_t receive(size_t *got)
{
    *got = 0;
    if (!s_cfg.has_data()) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(send_packet(SSCMA_CMD_AVAILABLE, NULL, 0), TAG, "available");
    ESP_RETURN_ON_ERROR(transfer(NULL, s_rx, 2), TAG, "available length");
    size_t len = s_rx[0] << 8 | s_rx[1];
    if (len == 0xffff) {
        len = 0;
    }
    while (*got < len) {
        uint16_t part = len - *got < SSCMA_MAX_READ ? len - *got : SSCMA_MAX_READ;
        ESP_RETURN_ON_ERROR(send_packet(SSCMA_CMD_READ, NULL, part), TAG, "read");
        ESP_RETURN_ON_ERROR(transfer(NULL, s_rx, part), TAG, "read data");
        if (!sscma_rx_feed(&s_replies, s_rx, part)) {
            ESP_LOGW(TAG, "reply too big, dropped");
        }
        *got += part;
    }
    return ESP_OK;
}

static int int_field(const cJSON *j, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(j, key);
    return cJSON_IsNumber(v) ? v->valueint : -1;
}

/* The next reply of `type` whose name contains `name`, or NULL after timeout_ms.
 * Others (logs, earlier events) are dropped. The caller deletes it. */
static cJSON *wait_for(int type, const char *name, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    for (;;) {
        char *json;
        while ((json = sscma_rx_next(&s_replies))) {
            cJSON *j = cJSON_Parse(json);
            const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
            if (j && int_field(j, "type") == type && n && strstr(n, name)) {
                return j;
            }
            ESP_LOGD(TAG, "skipped: %.80s", json);
            cJSON_Delete(j);
        }
        if (esp_timer_get_time() >= deadline) {
            return NULL;
        }
        size_t got;
        if (receive(&got) != ESP_OK) {
            return NULL;
        }
        if (!got) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        }
    }
}

/* Sends `at` and waits up to timeout_ms for its response; ESP_OK if the chip took it. */
static esp_err_t request(const char *at, const char *name, int timeout_ms)
{
    ESP_RETURN_ON_ERROR(command(at), TAG, "%s", name);
    cJSON *r = wait_for(SSCMA_TYPE_RESPONSE, name, timeout_ms);
    bool answered = r != NULL;
    int code = answered ? int_field(r, "code") : -1;
    cJSON_Delete(r);
    if (!answered) {
        return ESP_ERR_TIMEOUT;
    }
    if (code) {
        ESP_LOGW(TAG, "%s refused (code %d)", name, code);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t sscma_init(void)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = s_cfg.sclk,
        .mosi_io_num = s_cfg.mosi,
        .miso_io_num = s_cfg.miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SSCMA_MAX_READ + 1,
    };
    esp_err_t err = spi_bus_initialize(s_cfg.host, &bus, SPI_DMA_CH_AUTO);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "spi bus");
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_HZ,
        .mode = 0,
        .spics_io_num = s_cfg.cs,
        .queue_size = 1,
    };
    if (!s_dev) {
        ESP_RETURN_ON_ERROR(spi_bus_add_device(s_cfg.host, &dev, &s_dev), TAG, "spi device");
    }
#if CONFIG_PM_ENABLE
    if (!s_awake) {
        ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "camera", &s_awake), TAG, "pm lock");
    }
#endif
    return s_cfg.reset ? s_cfg.reset(true) : ESP_OK;
}

/* The image in a SAMPLE event, decoded into `out`. */
static esp_err_t take_image(const cJSON *event, camera_frame_t *out)
{
    const cJSON *data = cJSON_GetObjectItem(event, "data");
    const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(data, "image"));
    ESP_RETURN_ON_FALSE(b64 && *b64, ESP_ERR_INVALID_RESPONSE, TAG, "no image in the sample");
    size_t b64_len = strlen(b64), len = 0;
    uint8_t *jpeg = heap_caps_malloc(b64_len / 4 * 3 + 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(jpeg, ESP_ERR_NO_MEM, TAG, "image buffer");
    if (mbedtls_base64_decode(jpeg, b64_len / 4 * 3 + 3, &len, (const unsigned char *)b64, b64_len) || len < 4
        || jpeg[0] != 0xff || jpeg[1] != 0xd8) {
        free(jpeg);
        ESP_LOGW(TAG, "the sample isn't a JPEG");
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *res = cJSON_GetObjectItem(data, "resolution");
    out->jpeg = jpeg;
    out->len = len;
    out->width = cJSON_GetArraySize(res) == 2 ? cJSON_GetArrayItem(res, 0)->valueint : 0;
    out->height = cJSON_GetArraySize(res) == 2 ? cJSON_GetArrayItem(res, 1)->valueint : 0;
    return ESP_OK;
}

/* Powers the chip, waits for it to boot and sets `resolution`. On failure it's off again. */
static esp_err_t power_up(int resolution)
{
    int64_t t0 = esp_timer_get_time();
#if CONFIG_PM_ENABLE
    esp_pm_lock_acquire(s_awake);
#endif
    /* Internal DMA RAM is scarce, so these are only held while it's on. */
    s_tx = heap_caps_malloc(SSCMA_PACKET, MALLOC_CAP_DMA);
    s_rx = heap_caps_malloc(SSCMA_MAX_READ + 1, MALLOC_CAP_DMA);
    esp_err_t err = s_tx && s_rx ? s_cfg.power(true) : ESP_ERR_NO_MEM;
    if (err == ESP_OK && s_cfg.reset) {
        s_cfg.reset(true);
        vTaskDelay(pdMS_TO_TICKS(RESET_HOLD_MS));
        s_cfg.reset(false);
        vTaskDelay(pdMS_TO_TICKS(RESET_START_MS));
    }
    if (err == ESP_OK) {
        /* The Watcher's Himax doesn't announce itself (no INIT@STAT), so ask
         * until it answers: the first answer is the end of its boot. */
        char at[32];
        snprintf(at, sizeof(at), "AT+SENSOR=1,1,%d\r\n", resolution);
        int64_t booted_by = esp_timer_get_time() + BOOT_MS * 1000LL;
        do {
            err = request(at, "SENSOR", BOOT_RETRY_MS);
        } while (err == ESP_ERR_TIMEOUT && esp_timer_get_time() < booted_by);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "camera up in %lld ms", (esp_timer_get_time() - t0) / 1000);
    } else {
        ESP_LOGW(TAG, "camera didn't start: %s", esp_err_to_name(err));
        power_down();
    }
    return err;
}

static void power_down(void)
{
    if (s_cfg.reset) {
        s_cfg.reset(true);
    }
    s_cfg.power(false);
    sscma_rx_free(&s_replies);
    free(s_tx);
    free(s_rx);
    s_tx = s_rx = NULL;
#if CONFIG_PM_ENABLE
    esp_pm_lock_release(s_awake);
#endif
}

static esp_err_t sscma_capture(camera_frame_t *out)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = power_up(s_cfg.resolution);
    if (err != ESP_OK) {
        return err;
    }
    err = request("AT+SAMPLE=1\r\n", "SAMPLE", REPLY_MS);
    if (err == ESP_OK) {
        cJSON *event = wait_for(SSCMA_TYPE_EVENT, "SAMPLE", SAMPLE_MS);
        err = event ? take_image(event, out) : ESP_ERR_TIMEOUT;
        cJSON_Delete(event);
    }
    power_down();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%dx%d JPEG, %u bytes in %lld ms", out->width, out->height, (unsigned)out->len,
                 (esp_timer_get_time() - t0) / 1000);
    } else {
        ESP_LOGW(TAG, "capture failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void sscma_release(camera_frame_t *frame)
{
    free((void *)frame->jpeg);
}

/* ---- Stream ---- */

#define STREAM_STACK 8192
#define STREAM_FRAME_MS 1000     /* looks for a stop this often when no frame comes */
#define STREAM_STOP_MS 5000

static camera_frame_cb_t s_on_frame;
static void *s_frame_ctx;
static volatile bool s_stop;
static SemaphoreHandle_t s_stopped;

/* Sampling continuously (AT+SAMPLE=-1): each SAMPLE event to s_on_frame, until s_stop. */
static void stream_task(void *arg)
{
    (void)arg;
    while (!s_stop) {
        cJSON *event = wait_for(SSCMA_TYPE_EVENT, "SAMPLE", STREAM_FRAME_MS);
        camera_frame_t f = { 0 };
        if (event && take_image(event, &f) == ESP_OK) {
            s_on_frame(&f, s_frame_ctx);
            free((void *)f.jpeg);
        }
        cJSON_Delete(event);
    }
    request("AT+BREAK\r\n", "BREAK", REPLY_MS);
    power_down();
    ESP_LOGI(TAG, "stream stopped");
    xSemaphoreGive(s_stopped);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t sscma_stream_start(camera_frame_cb_t on_frame, void *ctx)
{
    if (!s_stopped) {
        s_stopped = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_stopped, ESP_ERR_NO_MEM, TAG, "stream semaphore");
    }
    ESP_RETURN_ON_ERROR(power_up(s_cfg.stream_resolution), TAG, "stream power up");
    esp_err_t err = request("AT+SAMPLE=-1\r\n", "SAMPLE", REPLY_MS);
    if (err == ESP_OK) {
        s_on_frame = on_frame;
        s_frame_ctx = ctx;
        s_stop = false;
        /* Its stack can be in PSRAM: the SPI buffers it hands the bus are internal. */
        if (xTaskCreateWithCaps(stream_task, "camera_stream", STREAM_STACK, NULL, 4, NULL,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) {
        power_down();
    }
    return err;
}

static void sscma_stream_stop(void)
{
    s_stop = true;
    /* Waits for the task however long it takes: until it powers down, it still
     * owns the chip and the SPI buffers, and every step it takes has a timeout. */
    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(STREAM_STOP_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "stream slow to stop, still waiting");
        xSemaphoreTake(s_stopped, portMAX_DELAY);
    }
}

static const int RES_W[] = { 240, 416, 480, 640 };
static const int RES_H[] = { 240, 416, 480, 480 };
static camera_driver_t s_driver;

static int clamp_resolution(int r)
{
    return r >= 0 && r <= 3 ? r : 3;
}

const camera_driver_t *camera_sscma(const camera_sscma_config_t *config)
{
    s_cfg = *config;
    s_cfg.resolution = clamp_resolution(s_cfg.resolution);
    s_cfg.stream_resolution = clamp_resolution(s_cfg.stream_resolution);
    s_driver = (camera_driver_t){
        .name = s_cfg.name,
        .max_width = RES_W[s_cfg.resolution],
        .max_height = RES_H[s_cfg.resolution],
        .init = sscma_init,
        .capture = sscma_capture,
        .release = sscma_release,
        .stream_start = sscma_stream_start,
        .stream_stop = sscma_stream_stop,
    };
    return &s_driver;
}
