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
#include <stdio.h>
#include <string.h>

#include "noise_upgrade.h"

// ---- query escaping ---------------------------------------------------------

static void expect_escape(const char *in, const char *want) {
    char out[512];
    int n = noise_upgrade_query_escape(in, out, sizeof(out));
    assert(n == (int)strlen(want));
    assert(strcmp(out, want) == 0);
}

static void test_a_uuid_vm_id_is_unchanged(void) {
    // The values Link actually sends are already fully unreserved, so the
    // escape must not alter a single byte of a real vm_id.
    expect_escape("4e024627-69ec-4c83-a7fa-9ff8b8d73c61",
                  "4e024627-69ec-4c83-a7fa-9ff8b8d73c61");
    expect_escape("", "");
    expect_escape("abcXYZ019-_.!~*'()", "abcXYZ019-_.!~*'()");
}

static void test_query_structure_cannot_escape_the_value(void) {
    expect_escape("a&vm_id=b", "a%26vm_id%3Db");
    expect_escape("a b", "a%20b");
    expect_escape("a?b", "a%3Fb");
    expect_escape("a#b", "a%23b");
    expect_escape("a/b", "a%2Fb");
    expect_escape("a+b", "a%2Bb");
    expect_escape("a%b", "a%25b");
    expect_escape("a\r\nX: y", "a%0D%0AX%3A%20y");
}

static void test_high_bytes_are_encoded_not_truncated(void) {
    expect_escape("\xc3\xa9", "%C3%A9");
    expect_escape("\x7f", "%7F");
    expect_escape("\x01", "%01");
}

static void test_a_too_small_buffer_fails_rather_than_truncating(void) {
    char out[4];

    assert(noise_upgrade_query_escape("abc", out, sizeof(out)) == 3);
    assert(strcmp(out, "abc") == 0);
    assert(noise_upgrade_query_escape("abcd", out, sizeof(out)) == -1);

    // A single escaped byte needs 3 + NUL, so it fits; two do not. A partial
    // escape sequence on the wire would be worse than no request at all.
    assert(noise_upgrade_query_escape("&", out, sizeof(out)) == 3);
    assert(strcmp(out, "%26") == 0);
    assert(noise_upgrade_query_escape("&&", out, sizeof(out)) == -1);

    assert(noise_upgrade_query_escape("a", out, 0) == -1);
    assert(noise_upgrade_query_escape(NULL, out, sizeof(out)) == -1);
    assert(noise_upgrade_query_escape("a", NULL, sizeof(out)) == -1);
}

static void test_the_worst_case_macro_is_actually_the_worst_case(void) {
    char in[16];
    memset(in, '&', sizeof(in) - 1);
    in[sizeof(in) - 1] = '\0';

    char out[NOISE_UPGRADE_ESCAPED_CAP(sizeof(in))];
    int n = noise_upgrade_query_escape(in, out, sizeof(out));
    assert(n == 3 * (int)(sizeof(in) - 1));
}

// ---- request construction ---------------------------------------------------

#define UUID "4e024627-69ec-4c83-a7fa-9ff8b8d73c61"
#define TOKEN "s0:eyJ0eXAiOiJKV1QifQ.eyJvdCI6ImFiYyJ9.c2ln"

static int build(const char *vm_id, const char *token, char *out,
                 size_t out_cap) {
    return noise_upgrade_build_request("/v1/noise", vm_id,
                                       "muse-gadget.vm.example", token,
                                       out, out_cap);
}

static void test_the_request_carries_the_exact_token_in_a_header(void) {
    char req[1024];
    int n = build(UUID, TOKEN, req, sizeof(req));
    assert(n > 0);
    assert((size_t)n == strlen(req));

    // The bearer arrives byte-for-byte as fetch_vms handed it over. This is
    // required because the edge proxy validates the token for the selected VM.
    assert(strstr(req, "Authorization: Bearer " TOKEN "\r\n") != NULL);

    // ...and nowhere near the request target, where it would land in access
    // logs. Check the request line specifically, not just the whole buffer.
    char line[512];
    const char *eol = strstr(req, "\r\n");
    assert(eol != NULL);
    size_t line_len = (size_t)(eol - req);
    assert(line_len < sizeof(line));
    memcpy(line, req, line_len);
    line[line_len] = '\0';

    assert(strcmp(line, "GET /v1/noise?vm_id=" UUID " HTTP/1.1") == 0);
    assert(strstr(line, "auth_token") == NULL);
    assert(strstr(line, TOKEN) == NULL);
    assert(strstr(line, "Bearer") == NULL);
}

static void test_a_real_uuid_produces_a_byte_identical_target(void) {
    // Escaping must not change what the edge sees for the values it actually
    // receives — otherwise this would need validating against the edge.
    char req[1024];
    assert(build(UUID, TOKEN, req, sizeof(req)) > 0);
    assert(strncmp(req, "GET /v1/noise?vm_id=" UUID " HTTP/1.1\r\n",
                   strlen("GET /v1/noise?vm_id=" UUID " HTTP/1.1\r\n")) == 0);
}

static void test_a_hostile_vm_id_cannot_add_query_parameters(void) {
    char req[1024];
    assert(build("abc&vm_id=victim", TOKEN, req, sizeof(req)) > 0);

    assert(strstr(req, "?vm_id=abc%26vm_id%3Dvictim HTTP/1.1\r\n") != NULL);
    // Exactly one vm_id parameter reaches the edge that routes on it.
    assert(strstr(req, "vm_id=victim") == NULL);

    // A space would otherwise end the request target early.
    assert(build("abc def", TOKEN, req, sizeof(req)) > 0);
    assert(strstr(req, "?vm_id=abc%20def HTTP/1.1\r\n") != NULL);
}

static void test_crlf_is_refused_outright(void) {
    char req[1024];

    // The token is not escaped — it has to arrive verbatim — so this check is
    // the only thing between a server-supplied token and header injection.
    assert(build(UUID, "tok\r\nX-Evil: 1", req, sizeof(req)) == -1);
    assert(build(UUID, "tok\r", req, sizeof(req)) == -1);
    assert(build(UUID, "tok\n", req, sizeof(req)) == -1);

    // vm_id would be escaped anyway; failing closed is the clearer signal.
    assert(build("id\r\nX-Evil: 1", TOKEN, req, sizeof(req)) == -1);
}

static void test_a_missing_token_is_refused_not_defaulted(void) {
    char req[1024];

    // There is no second credential to reach for. Refusing is the only
    // correct answer.
    assert(build(UUID, "", req, sizeof(req)) == -1);
    assert(build(UUID, NULL, req, sizeof(req)) == -1);
    assert(build("", TOKEN, req, sizeof(req)) == -1);
    assert(build(NULL, TOKEN, req, sizeof(req)) == -1);
}

static void test_an_undersized_buffer_writes_no_partial_request(void) {
    char req[64];
    assert(build(UUID, TOKEN, req, sizeof(req)) == -1);

    // And the caller's own sizing formula is big enough for the worst case.
    char vm_id[128];
    memset(vm_id, '&', sizeof(vm_id) - 1);
    vm_id[sizeof(vm_id) - 1] = '\0';

    char big[512 + NOISE_UPGRADE_ESCAPED_CAP(128) + sizeof(TOKEN)];
    assert(build(vm_id, TOKEN, big, sizeof(big)) > 0);
}

// ---- response classification ------------------------------------------------

static int status_of(const char *response) {
    return noise_upgrade_status_code(response, strlen(response));
}

static void test_status_is_parsed_from_the_status_line(void) {
    assert(status_of("HTTP/1.1 101 Switching Protocols\r\n\r\n") == 101);
    assert(status_of("HTTP/1.1 401 Unauthorized\r\n\r\n") == 401);
    assert(status_of("HTTP/1.1 403 Forbidden\r\n\r\n") == 403);
    assert(status_of("HTTP/1.1 502 Bad Gateway\r\n\r\n") == 502);
    assert(status_of("HTTP/1.0 200 OK\r\n\r\n") == 200);
}

static void test_unparseable_heads_are_unknown_not_rejections(void) {
    assert(noise_upgrade_status_code(NULL, 0) == -1);
    assert(status_of("") == -1);
    assert(status_of("HTTP/1.1 40") == -1);          // 11 bytes, one short
    assert(status_of("HTTP/1.1 4x1 Nope\r\n\r\n") == -1);
    assert(status_of("<html>401</html>\r\n\r\n") == -1);
    assert(status_of("\x16\x03\x01 401 not tls\r\n") == -1);

    assert(!noise_upgrade_status_is_auth_rejection(-1));
}

static void test_no_read_past_the_bytes_we_hold(void) {
    char buf[64];
    memset(buf, 'A', sizeof(buf));
    memcpy(buf, "HTTP/1.1 401", 12);
    assert(noise_upgrade_status_code(buf, 11) == -1);
    assert(noise_upgrade_status_code(buf, 12) == 401);
}

static noise_upgrade_result_t classify(const char *response) {
    return noise_upgrade_classify(response, strlen(response), false);
}

static void test_the_edge_refusing_the_bearer_is_an_auth_rejection(void) {
    // These two are the edge proxy saying vm_auth_token is not good for this
    // VM. Only a re-fetch from fetch_vms can fix it, so they must not be
    // filed under generic transport failure.
    assert(classify("HTTP/1.1 401 Unauthorized\r\n\r\n")
           == NOISE_UPGRADE_AUTH_REJECTED);
    assert(classify("HTTP/1.1 403 Forbidden\r\n\r\n")
           == NOISE_UPGRADE_AUTH_REJECTED);
}

static void test_everything_else_is_success_or_plain_failure(void) {
    assert(classify("HTTP/1.1 101 Switching Protocols\r\n\r\n")
           == NOISE_UPGRADE_OK);

    // The edge or the VM being unwell. Re-fetching would not help and the 60s
    // auth backoff would slow a recovery that is already coming.
    const char *transport[] = {
        "HTTP/1.1 200 OK\r\n\r\n",
        "HTTP/1.1 400 Bad Request\r\n\r\n",
        "HTTP/1.1 404 Not Found\r\n\r\n",
        "HTTP/1.1 408 Request Timeout\r\n\r\n",
        "HTTP/1.1 429 Too Many Requests\r\n\r\n",
        "HTTP/1.1 500 Internal Server Error\r\n\r\n",
        "HTTP/1.1 502 Bad Gateway\r\n\r\n",
        "HTTP/1.1 503 Service Unavailable\r\n\r\n",
        "not http at all\r\n\r\n",
        "",
    };
    for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); i++) {
        assert(classify(transport[i]) == NOISE_UPGRADE_FAILED);
    }
}

static void test_an_oversized_rejection_still_classifies(void) {
    // The edge attaches a ~1.4 KB RFC 9209 Proxy-Status header to errors, so
    // a 401 can outgrow the 4 KB buffer before the terminator arrives. The
    // status line came first and still decides.
    char buf[4096];
    memset(buf, 'x', sizeof(buf));
    memcpy(buf, "HTTP/1.1 401 Unauthorized\r\nProxy-Status: ", 40);
    assert(noise_upgrade_classify(buf, sizeof(buf), true)
           == NOISE_UPGRADE_AUTH_REJECTED);

    // An overflowed 101 is not a success: we never saw the header terminator,
    // so there is no framing boundary to start the Noise handshake from.
    memcpy(buf, "HTTP/1.1 101 Switching Protocols\r\nX-Pad: ", 40);
    assert(noise_upgrade_classify(buf, sizeof(buf), true)
           == NOISE_UPGRADE_FAILED);
}

int main(void) {
    test_a_uuid_vm_id_is_unchanged();
    test_query_structure_cannot_escape_the_value();
    test_high_bytes_are_encoded_not_truncated();
    test_a_too_small_buffer_fails_rather_than_truncating();
    test_the_worst_case_macro_is_actually_the_worst_case();

    test_the_request_carries_the_exact_token_in_a_header();
    test_a_real_uuid_produces_a_byte_identical_target();
    test_a_hostile_vm_id_cannot_add_query_parameters();
    test_crlf_is_refused_outright();
    test_a_missing_token_is_refused_not_defaulted();
    test_an_undersized_buffer_writes_no_partial_request();

    test_status_is_parsed_from_the_status_line();
    test_unparseable_heads_are_unknown_not_rejections();
    test_no_read_past_the_bytes_we_hold();
    test_the_edge_refusing_the_bearer_is_an_auth_rejection();
    test_everything_else_is_success_or_plain_failure();
    test_an_oversized_rejection_still_classifies();

    printf("link_noise_upgrade_harness: all tests passed\n");
    return 0;
}
