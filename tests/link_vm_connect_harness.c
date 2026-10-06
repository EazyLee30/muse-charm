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

// Exercise the three credential requirements against the connection decision:
//
//   1. use exactly the `vm_auth_token` value fetch_vms returned for a VM;
//   2. refuse missing tokens instead of substituting an account-wide credential;
//   3. never use a `vm_auth_token` value from another VM.
//
// vm_connect_params() is where Link decides which id and which credential a
// connection is built from, so it is where those rules either hold or do not.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "vm_connect.h"

#define UUID_A "4e024627-69ec-4c83-a7fa-9ff8b8d73c61"
#define UUID_B "68146a24-537d-4a0d-8727-c330445c62df"
#define TOKEN_A "s0:eyJ0eXAiOiJKV1QifQ.eyJlbnZfaWQiOiJBIn0.c2lnQQ"
#define TOKEN_B "s0:eyJ0eXAiOiJKV1QifQ.eyJlbnZfaWQiOiJCIn0.c2lnQg"

// Two entries as fetch_vms would return them, each with its own token.
static vm_info_t vm_a(void) {
    vm_info_t v = {0};
    v.vm_url = (char *)"wss://" UUID_A ".vm.example/";
    v.vm_auth_token = (char *)TOKEN_A;
    v.vm_name = (char *)"a";
    v.vm_id = (char *)UUID_A;
    v.is_default = true;
    return v;
}

static vm_info_t vm_b(void) {
    vm_info_t v = {0};
    v.vm_url = (char *)"wss://" UUID_B ".vm.example/";
    v.vm_auth_token = (char *)TOKEN_B;
    v.vm_name = (char *)"b";
    v.vm_id = (char *)UUID_B;
    return v;
}

// ---- requirement 1: use exactly this entry's token --------------------------

static void test_the_token_is_this_entrys_token_byte_for_byte(void) {
    vm_info_t a = vm_a();
    char vm_id[128] = {0};
    const char *token = NULL;

    assert(vm_connect_params(&a, vm_id, sizeof(vm_id), &token));
    assert(strcmp(vm_id, UUID_A) == 0);
    assert(strcmp(token, TOKEN_A) == 0);

    // Not a rewritten copy — the exact bytes the entry holds. A prefix strip
    // or re-encode anywhere in here would show up as a different pointer
    // holding different bytes.
    assert(token == a.vm_auth_token);
}

static void test_a_token_with_awkward_bytes_survives_unchanged(void) {
    // Nothing in this path may normalise, trim or re-encode the token.
    vm_info_t v = vm_a();
    char weird[] = "s0:AAAA+bbb/ccc==.dd-ee_ff.~gg";
    v.vm_auth_token = weird;

    char vm_id[128] = {0};
    const char *token = NULL;
    assert(vm_connect_params(&v, vm_id, sizeof(vm_id), &token));
    assert(strcmp(token, weird) == 0);
    assert(strlen(token) == strlen(weird));
}

// ---- requirement 2: no fallback ---------------------------------------------

static void test_an_entry_with_no_token_is_refused(void) {
    char vm_id[128];
    const char *token = (const char *)0x1;  // must not be written on refusal

    vm_info_t missing = vm_a();
    missing.vm_auth_token = NULL;
    assert(!vm_connect_params(&missing, vm_id, sizeof(vm_id), &token));
    assert(token == (const char *)0x1);

    vm_info_t empty = vm_a();
    empty.vm_auth_token = (char *)"";
    assert(!vm_connect_params(&empty, vm_id, sizeof(vm_id), &token));
    assert(token == (const char *)0x1);

    // Neither an account-wide credential, a device access token, nor a
    // previously used token can complete this entry. Refuse the connection.
}

static void test_an_entry_with_no_usable_id_is_refused(void) {
    char vm_id[128];
    const char *token = NULL;

    vm_info_t no_url = vm_a();
    no_url.vm_url = NULL;
    no_url.vm_id = NULL;
    assert(!vm_connect_params(&no_url, vm_id, sizeof(vm_id), &token));

    // No vm_id field, and a URL the id cannot be derived from.
    vm_info_t bad_url = vm_a();
    bad_url.vm_id = NULL;
    bad_url.vm_url = (char *)"not-a-url";
    assert(!vm_connect_params(&bad_url, vm_id, sizeof(vm_id), &token));

    assert(!vm_connect_params(NULL, vm_id, sizeof(vm_id), &token));
}

static void test_an_oversized_id_is_refused_not_clipped(void) {
    // A truncated id can name a different VM, and we would then present this
    // VM's token against it.
    char long_id[80];
    memset(long_id, 'a', sizeof(long_id) - 1);
    long_id[sizeof(long_id) - 1] = '\0';

    vm_info_t v = vm_a();
    v.vm_id = long_id;

    char vm_id[32] = {0};
    const char *token = NULL;
    assert(!vm_connect_params(&v, vm_id, sizeof(vm_id), &token));
    assert(vm_id[0] == '\0');
}

// ---- requirement 3: never another VM's token --------------------------------

static void test_each_entry_yields_its_own_pair(void) {
    vm_info_t a = vm_a();
    vm_info_t b = vm_b();

    char id_a[128] = {0};
    char id_b[128] = {0};
    const char *tok_a = NULL;
    const char *tok_b = NULL;

    assert(vm_connect_params(&a, id_a, sizeof(id_a), &tok_a));
    assert(vm_connect_params(&b, id_b, sizeof(id_b), &tok_b));

    assert(strcmp(id_a, UUID_A) == 0 && strcmp(tok_a, TOKEN_A) == 0);
    assert(strcmp(id_b, UUID_B) == 0 && strcmp(tok_b, TOKEN_B) == 0);

    // The tokens are VM-specific now: they carry the env_id and cannot be
    // mixed and matched.
    assert(strcmp(tok_a, tok_b) != 0);
    assert(strcmp(tok_a, TOKEN_B) != 0);
    assert(strcmp(tok_b, TOKEN_A) != 0);
}

static void test_resolving_b_does_not_disturb_the_pair_for_a(void) {
    // A VM switch resolves a new entry. Nothing may be carried over: no
    // static buffer, no cached token, no id left from the previous call.
    vm_info_t a = vm_a();
    vm_info_t b = vm_b();

    char id[128] = {0};
    const char *token = NULL;

    assert(vm_connect_params(&a, id, sizeof(id), &token));
    assert(strcmp(id, UUID_A) == 0 && strcmp(token, TOKEN_A) == 0);

    assert(vm_connect_params(&b, id, sizeof(id), &token));
    assert(strcmp(id, UUID_B) == 0 && strcmp(token, TOKEN_B) == 0);
    assert(strstr(id, UUID_A) == NULL);

    assert(vm_connect_params(&a, id, sizeof(id), &token));
    assert(strcmp(id, UUID_A) == 0 && strcmp(token, TOKEN_A) == 0);
}

static void test_the_pair_survives_a_full_vm_list(void) {
    // Walk a list the way the selection helpers hand entries over, and check
    // every id lines up with the token from the same entry.
    vm_info_t vms[3];
    char urls[3][64];
    char ids[3][64];
    char tokens[3][64];
    for (int i = 0; i < 3; i++) {
        snprintf(ids[i], sizeof(ids[i]), "vm-%d", i);
        snprintf(urls[i], sizeof(urls[i]), "wss://vm-%d.vm.example/", i);
        snprintf(tokens[i], sizeof(tokens[i]), "s0:token-for-vm-%d", i);
        vms[i] = (vm_info_t){0};
        vms[i].vm_url = urls[i];
        vms[i].vm_id = ids[i];
        vms[i].vm_auth_token = tokens[i];
        vms[i].vm_name = ids[i];
    }

    for (int i = 0; i < 3; i++) {
        char id[128] = {0};
        const char *token = NULL;
        assert(vm_connect_params(&vms[i], id, sizeof(id), &token));

        // Sized for a full-length id: GCC's -Wformat-truncation reasons from
        // sizeof(id), not from the short ids this test actually uses.
        char want_token[sizeof("s0:token-for-") + sizeof(id)];
        snprintf(want_token, sizeof(want_token), "s0:token-for-%s", id);
        assert(strcmp(token, want_token) == 0);
    }
}

// ---- the id derivation fallback ---------------------------------------------

static void test_the_id_is_derived_from_this_entrys_url(void) {
    vm_info_t v = vm_a();
    v.vm_id = NULL;  // backend did not send one

    char vm_id[128] = {0};
    const char *token = NULL;
    assert(vm_connect_params(&v, vm_id, sizeof(vm_id), &token));

    // Derived from this entry's URL, and still paired with this entry's
    // token.
    assert(strcmp(vm_id, UUID_A) == 0);
    assert(token == v.vm_auth_token);
}

static void test_derive_rejects_ids_illegal_in_a_hostname_label(void) {
    char out[128];

    assert(vm_derive_id("wss://" UUID_A ".vm.example/", out, sizeof(out))
           == strlen(UUID_A));
    assert(strcmp(out, UUID_A) == 0);

    // The result feeds the WSS request target.
    assert(vm_derive_id("wss://a_b.vm.example/", out, sizeof(out)) == 0);
    assert(vm_derive_id("wss://a&b.vm.example/", out, sizeof(out)) == 0);
    assert(vm_derive_id("wss://a b.vm.example/", out, sizeof(out)) == 0);
    assert(vm_derive_id("wss://a%2eb.vm.example/", out, sizeof(out)) == 0);

    assert(vm_derive_id("no-scheme.vm.example/", out, sizeof(out)) == 0);
    assert(vm_derive_id("wss://nodot", out, sizeof(out)) == 0);
    assert(vm_derive_id("wss://.vm.example/", out, sizeof(out)) == 0);
    assert(vm_derive_id(NULL, out, sizeof(out)) == 0);
    assert(vm_derive_id("wss://" UUID_A ".vm.example/", out, 4) == 0);
}

int main(void) {
    test_the_token_is_this_entrys_token_byte_for_byte();
    test_a_token_with_awkward_bytes_survives_unchanged();

    test_an_entry_with_no_token_is_refused();
    test_an_entry_with_no_usable_id_is_refused();
    test_an_oversized_id_is_refused_not_clipped();

    test_each_entry_yields_its_own_pair();
    test_resolving_b_does_not_disturb_the_pair_for_a();
    test_the_pair_survives_a_full_vm_list();

    test_the_id_is_derived_from_this_entrys_url();
    test_derive_rejects_ids_illegal_in_a_hostname_label();

    printf("link_vm_connect_harness: all tests passed\n");
    return 0;
}
