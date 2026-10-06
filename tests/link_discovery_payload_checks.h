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

static size_t payload_allocations, payload_releases;
static int payload_fail_after = -1;
static void *payload_malloc(size_t n) {
    if (payload_fail_after == 0) { payload_fail_after = -1; return nullptr; }
    if (payload_fail_after > 0) payload_fail_after--;
    void *p = malloc(n);
    if (p) payload_allocations++;
    return p;
}
static void payload_free(void *p) {
    if (p) payload_releases++;
    free(p);
}
static void test_direct_payload(void) {
    cJSON_Hooks hooks = {payload_malloc, payload_free};
    cJSON_InitHooks(&hooks);
    for (int failure = -1; failure < 20; failure++) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON *payload = cJSON_AddObjectToObject(result, "payload");
        cJSON_AddBoolToObject(payload, "complete", false);
        cJSON_AddStringToObject(payload, "text", "quotes\"\\\n");
        cJSON_AddStringToObject(result, "payload_json", "{\"ignored\":true}");
        payload_fail_after = failure;
        char *wrapped = wrap_result_json("request-1", result);
        payload_fail_after = -1;
        if (wrapped) {
            cJSON *parsed = cJSON_Parse(wrapped); assert(parsed);
            cJSON *actual = cJSON_GetObjectItem(parsed, "payload");
            assert(actual && cJSON_IsFalse(cJSON_GetObjectItem(actual, "complete")));
            assert(!strcmp(cJSON_GetObjectItem(actual, "text")->valuestring, "quotes\"\\\n"));
            assert(!cJSON_GetObjectItem(actual, "ignored"));
            cJSON_Delete(parsed); cJSON_free(wrapped);
        } else assert(failure >= 0);
        assert(payload_allocations == payload_releases);
    }
    for (const char *legacy : {"{\"old\":true}", "invalid"}) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddStringToObject(r, "payload_json", legacy);
        char *wrapped = wrap_result_json("legacy", r); assert(wrapped);
        cJSON *parsed = cJSON_Parse(wrapped); assert(parsed);
        assert(cJSON_IsObject(cJSON_GetObjectItem(parsed,"payload")) == (legacy[0] == '{'));
        cJSON_Delete(parsed); cJSON_free(wrapped);
    }
    assert(payload_allocations == payload_releases);
    cJSON_InitHooks(nullptr);
    puts("PASS direct payload ownership, JSON escaping, allocation-failure cleanup and legacy string compatibility");
}
