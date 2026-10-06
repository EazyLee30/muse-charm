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

#include "net_discovery.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcpip.h"

#include "wifi_mgr.h"

static const char *TAG = "link.discover";

// Keep printable ASCII (0x20-0x7E) and tab, and turn newlines into spaces.
// Other hosts on the LAN write these strings, so they must not carry line
// breaks into the agent's input.
// Operates in-place, returns the new length.
static int sanitize_ascii(char *s) {
    char *r = s, *w = s;
    while (*r) {
        unsigned char c = (unsigned char)*r;
        if ((c >= 0x20 && c <= 0x7E) || c == '\t') *w++ = *r;
        else if (c == '\n') *w++ = ' ';
        r++;
    }
    *w = '\0';
    return (int)(w - s);
}

#define SRC_ARP     0x01
#define SRC_MDNS    0x02
#define SRC_SSDP    0x04
#define SRC_WSD     0x08
#define SRC_SNMP    0x10
#define SRC_NETBIOS 0x20
#define SRC_ALL     0x3F

#define ARP_BATCH_SIZE      8
#define ARP_BATCH_DELAY_MS  150
#define ARP_SETTLE_MS       300
#define MDNS_LISTEN_MS      3000
#define SSDP_LISTEN_MS      3000
#define WSD_LISTEN_MS       3000
#define SNMP_LISTEN_MS      2000
#define NETBIOS_LISTEN_MS   2000
#define RECV_BUF            1460
// Includes NUL, matching SNMP's value buffer. Log shortened fields without
// marking source truncation: that flag tracks omitted observations/coverage.
#define DISCOVERY_VALUE_BUF 256

#define MDNS_PORT   5353
#define SSDP_PORT   1900
#define WSD_PORT    3702

#define MAX_ARP     1024
#define MAX_MDNS    256
#define MAX_SSDP    256
#define MAX_WSD     64
#define MAX_SNMP    128
#define MAX_NETBIOS 256

#define DISCOVERY_MAX_JSON_BYTES (256 * 1024)
// Covers network/source metadata and the link.result envelope, including the
// escaped 63-byte request ID. The daemon caps the entire JSON at 256 KiB.
#define DISCOVERY_METADATA_BYTES 4096

// ---- helpers ---------------------------------------------------------------

static void ip_str(uint32_t net_addr, char *buf, int len) {
    uint8_t *b = (uint8_t *)&net_addr;
    snprintf(buf, len, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
}

static uint32_t parse_sources(cJSON *params) {
    cJSON *arr = params ? cJSON_GetObjectItem(params, "sources") : NULL;
    if (!cJSON_IsArray(arr)) return SRC_ALL;
    uint32_t m = 0;
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *s = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsString(s)) continue;
        const char *v = s->valuestring;
        if (!strcmp(v, "arp"))          m |= SRC_ARP;
        else if (!strcmp(v, "mdns"))    m |= SRC_MDNS;
        else if (!strcmp(v, "ssdp"))    m |= SRC_SSDP;
        else if (!strcmp(v, "wsd"))     m |= SRC_WSD;
        else if (!strcmp(v, "snmp"))    m |= SRC_SNMP;
        else if (!strcmp(v, "netbios")) m |= SRC_NETBIOS;
    }
    return m ? m : SRC_ALL;
}

typedef struct {
    int found;
    int cap;
    bool truncated;
    const char *error;
    uint32_t swept_hosts;
    uint32_t subnet_hosts;
} discovery_source_t;

typedef struct {
    cJSON *obs;
    size_t estimated_bytes;
    bool exhausted;
} discovery_budget_t;

static size_t json_string_bound(const char *s) {
    size_t bytes = 2;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        bytes += *p < 0x20 ? 6 : (*p == '"' || *p == '\\' ? 2 : 1);
    return bytes;
}

static void observation_string(cJSON **entry, const char *key, const char *value) {
    if (*entry && !cJSON_AddStringToObject(*entry, key, value)) {
        cJSON_Delete(*entry);
        *entry = NULL;
    }
}

static bool append_observation(discovery_budget_t *budget,
                               discovery_source_t *source, cJSON *entry) {
    if (!entry) { source->error = "out_of_memory"; return false; }
    // Observations contain only strings and numbers; 26 bounds a JSON double.
    size_t bytes = 3;
    for (cJSON *field = entry->child; field; field = field->next) {
        bytes += json_string_bound(field->string) + 2;
        bytes += cJSON_IsString(field) ? json_string_bound(field->valuestring) : 26;
    }
    if (budget->exhausted || bytes > DISCOVERY_MAX_JSON_BYTES - budget->estimated_bytes) {
        budget->exhausted = true;
        source->truncated = true;
        cJSON_Delete(entry);
        return false;
    }
    if (!cJSON_AddItemToArray(budget->obs, entry)) {
        cJSON_Delete(entry);
        source->error = "out_of_memory";
        return false;
    }
    budget->estimated_bytes += bytes;
    source->found++;
    return true;
}

// ---- ARP -------------------------------------------------------------------

static void arp_harvest(struct netif *lwip_nif, discovery_budget_t *budget,
                        uint32_t *seen, discovery_source_t *source, int max) {
    LOCK_TCPIP_CORE();
    for (int i = 0; i < ARP_TABLE_SIZE && source->found < max; i++) {
        ip4_addr_t *eip; struct netif *enif; struct eth_addr *emac;
        if (!etharp_get_entry(i, &eip, &enif, &emac)) continue;
        if (enif != lwip_nif || !eip || !emac) continue;
        bool dup = false;
        for (int j = 0; j < source->found; j++)
            if (seen[j] == eip->addr) { dup = true; break; }
        if (dup) continue;
        seen[source->found] = eip->addr;
        cJSON *e = cJSON_CreateObject();
        observation_string(&e, "source", "arp");
        char ib[16]; snprintf(ib, sizeof(ib), IPSTR, IP2STR(eip));
        observation_string(&e, "ip", ib);
        char mb[18];
        snprintf(mb, sizeof(mb), "%02x:%02x:%02x:%02x:%02x:%02x",
                 emac->addr[0], emac->addr[1], emac->addr[2],
                 emac->addr[3], emac->addr[4], emac->addr[5]);
        observation_string(&e, "mac", mb);
        if (!append_observation(budget, source, e)) break;
    }
    UNLOCK_TCPIP_CORE();
}

static void arp_probe(discovery_budget_t *budget, esp_netif_t *netif,
                      discovery_source_t *source) {
    struct netif *lwip_nif = esp_netif_get_netif_impl(netif);
    if (!lwip_nif) { source->error = "no_interface"; return; }

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        source->error = "ip_info_failed";
        return;
    }
    uint32_t own = ntohl(info.ip.addr);
    uint32_t net = own & ntohl(info.netmask.addr);
    uint32_t bcast = net | ~ntohl(info.netmask.addr);
    source->subnet_hosts = bcast > net ? bcast - net - 1 : 0;
    int hc = source->subnet_hosts > MAX_ARP ? MAX_ARP : (int)source->subnet_hosts;
    if (hc == 0) return;
    source->truncated = source->subnet_hosts > (uint32_t)hc;
    uint32_t first = net + 1;
    if (source->truncated) {
        first = own - net > (uint32_t)hc / 2 ? own - hc / 2 : net + 1;
        if (first > bcast - hc) first = bcast - hc;
    }

    uint32_t *seen = malloc(hc * sizeof(uint32_t));
    if (!seen) { source->error = "out_of_memory"; return; }

    ESP_LOGI(TAG, "ARP sweep: %d hosts", hc);
    for (int off = 0; off < hc && source->found < hc
         && !budget->exhausted && !source->error; off += ARP_BATCH_SIZE) {
        int cnt = hc - off;
        if (cnt > ARP_BATCH_SIZE) cnt = ARP_BATCH_SIZE;
        // Keep the batch synchronous under the core lock: queued callbacks can
        // outlive the delay and otherwise reuse overwritten batch coordinates.
        LOCK_TCPIP_CORE();
        for (int i = 0; i < cnt; i++) {
            ip4_addr_t target = { .addr = htonl(first + off + i) };
            err_t err = etharp_request(lwip_nif, &target);
            if (err == ERR_OK)
                source->swept_hosts++;
            else if (err == ERR_MEM)
                source->truncated = true; // Skip transient pbuf exhaustion; keep sweeping.
            else
                source->error = "arp_request_failed";
        }
        UNLOCK_TCPIP_CORE();
        vTaskDelay(pdMS_TO_TICKS(ARP_BATCH_DELAY_MS));
        arp_harvest(lwip_nif, budget, seen, source, hc);
    }
    if (source->found < hc && !budget->exhausted && !source->error) {
        vTaskDelay(pdMS_TO_TICKS(ARP_SETTLE_MS));
        arp_harvest(lwip_nif, budget, seen, source, hc);
    }
    if (source->found >= hc && source->swept_hosts < (uint32_t)hc)
        source->truncated = true;
    free(seen);
    ESP_LOGI(TAG, "ARP: %d hosts", source->found);
}

// ---- mDNS ------------------------------------------------------------------

static const char *s_mdns_services[][2] = {
    {"_hap",         "_tcp"},
    {"_googlecast",  "_tcp"},
    {"_airplay",     "_tcp"},
    {"_raop",        "_tcp"},
    {"_sonos",       "_tcp"},
    {"_matter",      "_tcp"},
    {"_hue",         "_tcp"},
    {"_http",        "_tcp"},
    {"_meshcop",     "_udp"},
    {"_smb",         "_tcp"},
};
#define NUM_MDNS_SERVICES (sizeof(s_mdns_services) / sizeof(s_mdns_services[0]))

static int dns_encode_name(const char *name, uint8_t *buf, int buf_len) {
    int pos = 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        int ll = dot ? (int)(dot - p) : (int)strlen(p);
        if (ll > 63 || pos + 1 + ll >= buf_len) return -1;
        buf[pos++] = (uint8_t)ll;
        memcpy(&buf[pos], p, ll);
        pos += ll;
        p += ll + (dot ? 1 : 0);
    }
    if (pos + 1 > buf_len) return -1;
    buf[pos++] = 0;
    return pos;
}

static int dns_decode_name(const uint8_t *pkt, int pkt_len, int pos,
                           char *out, int out_len) {
    int out_pos = 0, start = pos, jumped = 0, bytes = 0, max = 20;
    while (pos < pkt_len && max-- > 0) {
        uint8_t len = pkt[pos];
        if (len == 0) { if (!jumped) bytes = pos - start + 1; break; }
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= pkt_len) return -1;
            if (!jumped) bytes = pos - start + 2;
            pos = ((len & 0x3F) << 8) | pkt[pos + 1];
            jumped = 1; continue;
        }
        pos++;
        if (pos + len > pkt_len) return -1;
        if (out_pos > 0 && out_pos < out_len - 1) out[out_pos++] = '.';
        int c = len;
        if (out_pos + c >= out_len) c = out_len - out_pos - 1;
        if (c > 0) { memcpy(out + out_pos, pkt + pos, c); out_pos += c; }
        pos += len;
    }
    out[out_pos] = '\0';
    return bytes > 0 ? bytes : -1;
}

typedef struct {
    char instance[96];
    char hostname[64];
    char ip_str[16];
    uint16_t port;
    char txt[256];
} mdns_entry_t;

static int mdns_find_or_add(mdns_entry_t *arr, int *count, int max,
                            const char *instance) {
    for (int i = 0; i < *count; i++)
        if (strcmp(arr[i].instance, instance) == 0) return i;
    if (*count >= max) return -1;
    int idx = (*count)++;
    memset(&arr[idx], 0, sizeof(arr[idx]));
    snprintf(arr[idx].instance, sizeof(arr[idx].instance), "%s", instance);
    return idx;
}

static void parse_mdns_pkt(const uint8_t *pkt, int len,
                           mdns_entry_t *found, int *fc, int max) {
    if (len < 12) return;
    int qdcount = (pkt[4] << 8) | pkt[5];
    int total = ((pkt[6] << 8) | pkt[7]) + ((pkt[8] << 8) | pkt[9]) +
                ((pkt[10] << 8) | pkt[11]);

    int pos = 12;
    for (int q = 0; q < qdcount && pos < len; q++) {
        char skip[128];
        int n = dns_decode_name(pkt, len, pos, skip, sizeof(skip));
        if (n < 0) return;
        pos += n + 4;
    }

    for (int rr = 0; rr < total && pos < len; rr++) {
        char rname[128];
        int nb = dns_decode_name(pkt, len, pos, rname, sizeof(rname));
        if (nb < 0) break;
        pos += nb;
        if (pos + 10 > len) break;
        uint16_t rtype = (pkt[pos] << 8) | pkt[pos + 1];
        uint16_t rdlen = (pkt[pos + 8] << 8) | pkt[pos + 9];
        pos += 10;
        if (pos + rdlen > len) break;

        if (rtype == 12) {
            char inst[96];
            if (dns_decode_name(pkt, len, pos, inst, sizeof(inst)) > 0)
                mdns_find_or_add(found, fc, max, inst);
        } else if (rtype == 33 && rdlen >= 6) {
            uint16_t port = (pkt[pos + 4] << 8) | pkt[pos + 5];
            char target[64];
            if (dns_decode_name(pkt, len, pos + 6, target, sizeof(target)) > 0) {
                for (int i = 0; i < *fc; i++) {
                    if (strstr(rname, found[i].instance) ||
                        !strcmp(rname, found[i].instance)) {
                        found[i].port = port;
                        if (!found[i].hostname[0])
                            snprintf(found[i].hostname, sizeof(found[i].hostname),
                                     "%s", target);
                        break;
                    }
                }
            }
        } else if (rtype == 1 && rdlen == 4) {
            char aip[16];
            snprintf(aip, sizeof(aip), "%d.%d.%d.%d",
                     pkt[pos], pkt[pos+1], pkt[pos+2], pkt[pos+3]);
            for (int i = 0; i < *fc; i++) {
                if (!found[i].ip_str[0] &&
                    (strstr(rname, found[i].hostname) ||
                     strstr(found[i].hostname, rname) ||
                     !found[i].hostname[0])) {
                    snprintf(found[i].ip_str, sizeof(found[i].ip_str), "%s", aip);
                    break;
                }
            }
        } else if (rtype == 16) {
            int tp = 0; char tb[256]; int to = 0;
            while (tp < rdlen) {
                uint8_t tl = pkt[pos + tp]; tp++;
                if (tp + tl > rdlen) break;
                if (to > 0 && to < (int)sizeof(tb) - 1) tb[to++] = ';';
                int c = tl;
                if (to + c >= (int)sizeof(tb)) c = sizeof(tb) - to - 1;
                if (c > 0) { memcpy(tb + to, pkt + pos + tp, c); to += c; }
                tp += tl;
            }
            tb[to] = '\0';
            for (int i = 0; i < *fc; i++) {
                if (!found[i].txt[0] &&
                    (strstr(rname, found[i].instance) ||
                     !strcmp(rname, found[i].instance))) {
                    snprintf(found[i].txt, sizeof(found[i].txt), "%s", tb);
                    break;
                }
            }
        }
        pos += rdlen;
    }
}

static void mdns_probe(discovery_budget_t *budget, discovery_source_t *source) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { source->error = "socket_failed"; return; }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in ba = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(sock, (struct sockaddr *)&ba, sizeof(ba));

    struct ip_mreq mr = { .imr_multiaddr.s_addr = inet_addr("224.0.0.251"),
                          .imr_interface.s_addr = htonl(INADDR_ANY) };
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr));

    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT),
                                .sin_addr.s_addr = inet_addr("224.0.0.251") };
    uint8_t qbuf[256];

    ESP_LOGI(TAG, "mDNS: querying %d service types", (int)NUM_MDNS_SERVICES);
    for (int s = 0; s < (int)NUM_MDNS_SERVICES; s++) {
        memset(qbuf, 0, 12);
        qbuf[5] = 1;
        char qn[64];
        snprintf(qn, sizeof(qn), "%s.%s.local",
                 s_mdns_services[s][0], s_mdns_services[s][1]);
        int nl = dns_encode_name(qn, qbuf + 12, sizeof(qbuf) - 16);
        if (nl < 0) continue;
        int off = 12 + nl;
        qbuf[off] = 0; qbuf[off + 1] = 12;
        qbuf[off + 2] = 0x80; qbuf[off + 3] = 1;
        sendto(sock, qbuf, off + 4, 0, (struct sockaddr *)&dest, sizeof(dest));
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    mdns_entry_t *found = calloc(MAX_MDNS, sizeof(mdns_entry_t));
    if (!found) { source->error = "out_of_memory"; close(sock); return; }
    int fc = 0;

    struct timeval tv = { .tv_sec = MDNS_LISTEN_MS / 1000,
                          .tv_usec = (MDNS_LISTEN_MS % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t rbuf[RECV_BUF];
    int64_t deadline = esp_timer_get_time() + MDNS_LISTEN_MS * 1000LL;
    while (esp_timer_get_time() < deadline && fc < MAX_MDNS) {
        int n = recvfrom(sock, rbuf, sizeof(rbuf), 0, NULL, NULL);
        if (n <= 0) break;
        parse_mdns_pkt(rbuf, n, found, &fc, MAX_MDNS);
    }

    source->truncated = fc >= MAX_MDNS;
    for (int i = 0; i < fc; i++) {
        sanitize_ascii(found[i].instance);
        sanitize_ascii(found[i].hostname);
        sanitize_ascii(found[i].txt);
        cJSON *e = cJSON_CreateObject();
        observation_string(&e, "source", "mdns");
        observation_string(&e, "instance", found[i].instance);
        if (found[i].hostname[0])
            observation_string(&e, "hostname", found[i].hostname);
        if (found[i].ip_str[0])
            observation_string(&e, "ip", found[i].ip_str);
        if (e && found[i].port && !cJSON_AddNumberToObject(e, "port", found[i].port)) {
            cJSON_Delete(e);
            e = NULL;
        }
        if (found[i].txt[0])
            observation_string(&e, "txt", found[i].txt);
        if (!append_observation(budget, source, e)) break;
    }

    free(found);
    setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mr, sizeof(mr));
    close(sock);
    ESP_LOGI(TAG, "mDNS: %d unique services", fc);
}

// ---- SSDP ------------------------------------------------------------------

static void ssdp_probe(discovery_budget_t *budget, discovery_source_t *source) {
    static const char msearch[] =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: ssdp:all\r\n"
        "\r\n";

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { source->error = "socket_failed"; return; }

    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(SSDP_PORT),
                                .sin_addr.s_addr = inet_addr("239.255.255.250") };
    sendto(sock, msearch, strlen(msearch), 0,
           (struct sockaddr *)&dest, sizeof(dest));

    struct timeval tv = { .tv_sec = SSDP_LISTEN_MS / 1000,
                          .tv_usec = (SSDP_LISTEN_MS % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[RECV_BUF];
    int count = 0;
    int64_t deadline = esp_timer_get_time() + SSDP_LISTEN_MS * 1000LL;

    while (esp_timer_get_time() < deadline && count < MAX_SSDP) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &fl);
        if (n <= 0) break;
        buf[n] = '\0';
        char ib[16]; ip_str(from.sin_addr.s_addr, ib, sizeof(ib));
        cJSON *e = cJSON_CreateObject();
        observation_string(&e, "source", "ssdp");
        observation_string(&e, "ip", ib);

        static const char *hdrs[] = {"LOCATION","ST","USN","SERVER",NULL};
        for (int h = 0; hdrs[h]; h++) {
            size_t kl = strlen(hdrs[h]);
            for (char *line = buf; *line; ) {
                char *eol = strstr(line, "\r\n");
                if (!eol) eol = line + strlen(line);
                if ((size_t)(eol - line) > kl + 1
                    && line[kl] == ':'
                    && strncasecmp(line, hdrs[h], kl) == 0) {
                    char *val = line + kl + 1;
                    while (*val == ' ') val++;
                    char save = *eol; *eol = '\0';
                    char lk[16];
                    for (int i = 0; hdrs[h][i]; i++)
                        lk[i] = (hdrs[h][i] >= 'A' && hdrs[h][i] <= 'Z')
                                 ? hdrs[h][i] + 32 : hdrs[h][i];
                    lk[kl] = '\0';
                    // Sanitize a copy so subsequent header scans retain their CRLFs.
                    char value[DISCOVERY_VALUE_BUF];
                    int len = snprintf(value, sizeof(value), "%s", val);
                    if (len >= (int)sizeof(value))
                        ESP_LOGW(TAG, "SSDP %s value truncated from %d to %u bytes",
                                 hdrs[h], len, (unsigned)(sizeof(value) - 1));
                    sanitize_ascii(value);
                    observation_string(&e, lk, value);
                    *eol = save;
                    break;
                }
                if (!*eol) break;
                line = eol + 2;
            }
        }

        if (!append_observation(budget, source, e)) break;
        count++;
    }

    source->found = count;
    source->truncated |= count >= MAX_SSDP;
    close(sock);
    ESP_LOGI(TAG, "SSDP: %d responses", count);
}

// ---- WS-Discovery ----------------------------------------------------------

static void wsd_probe(discovery_budget_t *budget, discovery_source_t *source) {
    static const char probe[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
        " xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">"
        "<soap:Header>"
        "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe"
        "</wsa:Action>"
        "<wsa:MessageID>urn:uuid:00000001-0001-0001-0001-000000000001"
        "</wsa:MessageID>"
        "<wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>"
        "</soap:Header>"
        "<soap:Body><wsd:Probe/></soap:Body>"
        "</soap:Envelope>";

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { source->error = "socket_failed"; return; }

    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(WSD_PORT),
                                .sin_addr.s_addr = inet_addr("239.255.255.250") };
    sendto(sock, probe, strlen(probe), 0,
           (struct sockaddr *)&dest, sizeof(dest));

    struct timeval tv = { .tv_sec = WSD_LISTEN_MS / 1000,
                          .tv_usec = (WSD_LISTEN_MS % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[RECV_BUF];
    int count = 0;
    int64_t deadline = esp_timer_get_time() + WSD_LISTEN_MS * 1000LL;

    while (esp_timer_get_time() < deadline && count < MAX_WSD) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &fl);
        if (n <= 0) break;
        buf[n] = '\0';
        char ib[16]; ip_str(from.sin_addr.s_addr, ib, sizeof(ib));
        cJSON *e = cJSON_CreateObject();
        observation_string(&e, "source", "wsd");
        observation_string(&e, "ip", ib);
        char *types = strstr(buf, "<wsd:Types>");
        if (types) {
            types += 11;
            char *te = strstr(types, "</wsd:Types>");
            if (te) {
                *te = '\0';
                char value[DISCOVERY_VALUE_BUF];
                int len = snprintf(value, sizeof(value), "%s", types);
                if (len >= (int)sizeof(value))
                    ESP_LOGW(TAG, "WSD types value truncated from %d to %u bytes",
                             len, (unsigned)(sizeof(value) - 1));
                sanitize_ascii(value);
                observation_string(&e, "types", value);
            }
        }
        if (!append_observation(budget, source, e)) break;
        count++;
    }

    source->found = count;
    source->truncated |= count >= MAX_WSD;
    close(sock);
    ESP_LOGI(TAG, "WSD: %d responses", count);
}

// ---- SNMP ------------------------------------------------------------------

static bool snmp_extract_value(const uint8_t *data, int len,
                               char *out, int out_len) {
    static const uint8_t oid[] = {0x2b,0x06,0x01,0x02,0x01,0x01,0x01,0x00};
    for (int i = 0; i + (int)sizeof(oid) + 2 < len; i++) {
        if (data[i] == 0x06 && data[i+1] == sizeof(oid) &&
            memcmp(data + i + 2, oid, sizeof(oid)) == 0) {
            int vp = i + 2 + sizeof(oid);
            if (vp + 2 > len) return false;
            vp++;
            int vl = data[vp++];
            if (vl & 0x80) {
                int n = vl & 0x7F;
                if (n > 2 || vp + n > len) return false;
                vl = 0;
                for (int j = 0; j < n; j++) vl = (vl << 8) | data[vp++];
            }
            int c = vl < out_len - 1 ? vl : out_len - 1;
            if (vp + c > len) c = len - vp;
            if (c <= 0) return false;
            memcpy(out, data + vp, c);
            out[c] = '\0';
            return true;
        }
    }
    return false;
}

static void snmp_probe(discovery_budget_t *budget, esp_netif_t *netif,
                          discovery_source_t *source) {
    static const uint8_t pkt[] = {
        0x30,0x26,
          0x02,0x01,0x00,
          0x04,0x06,'p','u','b','l','i','c',
          0xa0,0x19,
            0x02,0x01,0x01,
            0x02,0x01,0x00,
            0x02,0x01,0x00,
            0x30,0x0e,
              0x30,0x0c,
                0x06,0x08,0x2b,0x06,0x01,0x02,0x01,0x01,0x01,0x00,
                0x05,0x00
    };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { source->error = "socket_failed"; return; }
    int bc = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bc, sizeof(bc));

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        source->error = "ip_info_failed"; close(sock); return;
    }
    uint32_t baddr = info.ip.addr | ~info.netmask.addr;

    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(161),
                                .sin_addr.s_addr = baddr };
    sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&dest, sizeof(dest));

    struct timeval tv = { .tv_sec = SNMP_LISTEN_MS / 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[RECV_BUF];
    int count = 0;
    int64_t deadline = esp_timer_get_time() + SNMP_LISTEN_MS * 1000LL;

    while (esp_timer_get_time() < deadline && count < MAX_SNMP) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &fl);
        if (n <= 0) break;
        char val[256];
        if (snmp_extract_value(buf, n, val, sizeof(val))) {
            sanitize_ascii(val);
            char ib[16]; ip_str(from.sin_addr.s_addr, ib, sizeof(ib));
            cJSON *e = cJSON_CreateObject();
            observation_string(&e, "source", "snmp");
            observation_string(&e, "ip", ib);
            observation_string(&e, "raw", val);
            if (!append_observation(budget, source, e)) break;
            count++;
        }
    }

    source->found = count;
    source->truncated |= count >= MAX_SNMP;
    close(sock);
    ESP_LOGI(TAG, "SNMP: %d responses", count);
}

// ---- NetBIOS ---------------------------------------------------------------

static void netbios_probe(discovery_budget_t *budget, esp_netif_t *netif,
                          discovery_source_t *source) {
    static const uint8_t query[] = {
        0x82,0xb2,
        0x00,0x00,
        0x00,0x01, 0x00,0x00, 0x00,0x00, 0x00,0x00,
        0x20,
        'C','K','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
        'A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
        0x00,
        0x00,0x21,
        0x00,0x01,
    };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { source->error = "socket_failed"; return; }
    int bc = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bc, sizeof(bc));

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        source->error = "ip_info_failed"; close(sock); return;
    }
    uint32_t baddr = info.ip.addr | ~info.netmask.addr;

    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(137),
                                .sin_addr.s_addr = baddr };
    sendto(sock, query, sizeof(query), 0,
           (struct sockaddr *)&dest, sizeof(dest));

    struct timeval tv = { .tv_sec = NETBIOS_LISTEN_MS / 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[RECV_BUF];
    int count = 0;
    int64_t deadline = esp_timer_get_time() + NETBIOS_LISTEN_MS * 1000LL;

    while (esp_timer_get_time() < deadline && count < MAX_NETBIOS) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &fl);
        if (n <= 0) break;

        int pos = 12;
        while (pos < n) {
            uint8_t lbl = buf[pos];
            if (lbl == 0) { pos++; break; }
            if ((lbl & 0xC0) == 0xC0) { pos += 2; break; }
            pos += 1 + lbl;
        }

        if (pos + 10 > n) continue;
        uint16_t rdlen = (buf[pos + 8] << 8) | buf[pos + 9];
        pos += 10;
        if (pos + rdlen > n || rdlen < 1) continue;

        int nn = buf[pos++];
        char nb[256] = "";
        int bo = 0;
        for (int i = 0; i < nn && pos + 18 <= n; i++) {
            char nm[16];
            memcpy(nm, buf + pos, 15);
            nm[15] = '\0';
            for (int j = 14; j >= 0 && nm[j] == ' '; j--) nm[j] = '\0';
            uint8_t sfx = buf[pos + 15];
            pos += 18;
            if (nm[0] && bo < (int)sizeof(nb) - 24) {
                if (bo > 0) nb[bo++] = ';';
                bo += snprintf(nb + bo, sizeof(nb) - bo, "%s<%02x>", nm, sfx);
            }
        }
        if (!nb[0]) continue;
        sanitize_ascii(nb);
        if (!nb[0]) continue;

        char ib[16]; ip_str(from.sin_addr.s_addr, ib, sizeof(ib));
        cJSON *e = cJSON_CreateObject();
        observation_string(&e, "source", "netbios");
        observation_string(&e, "ip", ib);
        observation_string(&e, "raw", nb);
        if (!append_observation(budget, source, e)) break;
        count++;
    }

    source->found = count;
    source->truncated |= count >= MAX_NETBIOS;
    close(sock);
    ESP_LOGI(TAG, "NetBIOS: %d responses", count);
}

// ---- Public API ------------------------------------------------------------

void net_discovery_init(void) {
    ESP_LOGI(TAG, "network discovery initialized");
}

cJSON *net_discovery_run(cJSON *params) {
    esp_netif_t *netif = wifi_mgr_get_netif();
    if (!netif || !wifi_mgr_is_connected()) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "code", "no_network");
        cJSON_AddStringToObject(err, "message", "WiFi not connected");
        cJSON_AddItemToObject(r, "error", err);
        return r;
    }

    uint32_t sources = parse_sources(params);

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_get_ip_info(netif, &ip_info);

    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "ok", true)) {
        cJSON_Delete(result); return NULL;
    }

    cJSON *network = cJSON_AddObjectToObject(result, "network");
    char ib[16], nb[16], gb[16];
    snprintf(ib, sizeof(ib), IPSTR, IP2STR(&ip_info.ip));
    snprintf(nb, sizeof(nb), IPSTR, IP2STR(&ip_info.netmask));
    snprintf(gb, sizeof(gb), IPSTR, IP2STR(&ip_info.gw));
    if (!network || !cJSON_AddStringToObject(network, "ip", ib)
        || !cJSON_AddStringToObject(network, "netmask", nb)
        || !cJSON_AddStringToObject(network, "gateway", gb)) {
        cJSON_Delete(result); return NULL;
    }

    cJSON *obs = cJSON_AddArrayToObject(result, "observations");
    cJSON *reports = cJSON_AddObjectToObject(result, "sources");
    if (!obs || !reports) { cJSON_Delete(result); return NULL; }
    static const struct {
        const char *name;
        uint32_t mask;
        int cap;
        int listen_ms;
    } specs[] = {
        {"arp", SRC_ARP, MAX_ARP, 0},
        {"mdns", SRC_MDNS, MAX_MDNS, MDNS_LISTEN_MS},
        {"ssdp", SRC_SSDP, MAX_SSDP, SSDP_LISTEN_MS},
        {"wsd", SRC_WSD, MAX_WSD, WSD_LISTEN_MS},
        {"snmp", SRC_SNMP, MAX_SNMP, SNMP_LISTEN_MS},
        {"netbios", SRC_NETBIOS, MAX_NETBIOS, NETBIOS_LISTEN_MS},
    };
    discovery_budget_t budget = { .obs = obs, .estimated_bytes = DISCOVERY_METADATA_BYTES };
    bool complete = true, failed = false;
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        if (!(sources & specs[i].mask)) continue;
        discovery_source_t source = { .cap = specs[i].cap };
        if (budget.exhausted) {
            source.truncated = true;
        } else switch (specs[i].mask) {
            case SRC_ARP: arp_probe(&budget, netif, &source); break;
            case SRC_MDNS: mdns_probe(&budget, &source); break;
            case SRC_SSDP: ssdp_probe(&budget, &source); break;
            case SRC_WSD: wsd_probe(&budget, &source); break;
            case SRC_SNMP: snmp_probe(&budget, netif, &source); break;
            case SRC_NETBIOS: netbios_probe(&budget, netif, &source); break;
        }
        cJSON *report = cJSON_AddObjectToObject(reports, specs[i].name);
        if (!report
            || !cJSON_AddNumberToObject(report, "found", source.found)
            || !cJSON_AddNumberToObject(report, "cap", source.cap)
            || !cJSON_AddBoolToObject(report, "truncated", source.truncated)
            || (specs[i].mask == SRC_ARP
                ? (!cJSON_AddNumberToObject(report, "swept_hosts", source.swept_hosts)
                   || !cJSON_AddNumberToObject(report, "subnet_hosts", source.subnet_hosts))
                : !cJSON_AddNumberToObject(report, "listen_ms", specs[i].listen_ms))
            || (source.error && !cJSON_AddStringToObject(report, "error", source.error))) {
            cJSON_Delete(result);
            return NULL;
        }
        if (source.truncated || source.error) complete = false;
        if (source.error) failed = true;
    }
    if (!cJSON_AddBoolToObject(result, "complete", complete)) {
        cJSON_Delete(result);
        return NULL;
    }
    if (failed) {
        cJSON_SetBoolValue(cJSON_GetObjectItem(result, "ok"), false);
        cJSON *error = cJSON_AddObjectToObject(result, "error");
        if (!error || !cJSON_AddStringToObject(error, "code", "discovery_failed")
            || !cJSON_AddStringToObject(error, "message", "one or more discovery sources failed")) {
            cJSON_Delete(result);
            return NULL;
        }
    }

    ESP_LOGI(TAG, "discovery complete: %d observations",
             cJSON_GetArraySize(obs));
    return result;
}
