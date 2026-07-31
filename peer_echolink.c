/*
 * EchoLink / iLink peer (directory login + RTP/RTCP + GSM PCM).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "peer_echolink.h"
#include "log.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <zlib.h>

#include "gsm.h"

/* Must stay under tlb ConfMemberTimeout (default 40s) so SDES keepalive survives. */
#define EL_DIR_LOOKUP_TIMEOUT_SEC 20
/* tlb MAX_STATION_LIST_SIZE — compressed or plain directory snapshot. */
#define EL_STATION_LIST_MAX (1024 * 1024)

static void el_send_sdes(peer_echolink_t *p);
static void el_send_firewall_open(peer_echolink_t *p);
static void el_send_connect_handshake(peer_echolink_t *p);
static int el_try_directory_login(peer_echolink_t *p);
static void el_flush_rtp_tx(peer_echolink_t *p);
static void el_login_and_list(peer_echolink_t *p);
static void el_station_list_only(peer_echolink_t *p);
static void el_dir_op_begin(peer_echolink_t *p);
static void el_dir_op_end(peer_echolink_t *p);
static void el_handle_rtp(peer_echolink_t *p, const uint8_t *data, int len,
                          const struct sockaddr_in *from);
static void el_handle_rtcp(peer_echolink_t *p, const uint8_t *data, int len,
                           const struct sockaddr_in *from);
static int el_tcp_bind_connect(peer_echolink_t *p, const char *server, int *out_fd);
static int resolve_dns(const char *host, struct in_addr *out);

/* --- transport: direct UDP/TCP or EchoLink Proxy --- */

static int el_rtcp_ok(const peer_echolink_t *p)
{
    return p && p->peer_resolved && (p->use_proxy || p->rtcp_sock >= 0);
}

static int el_rtp_ok(const peer_echolink_t *p)
{
    return p && p->peer_resolved && (p->use_proxy || p->rtp_sock >= 0);
}

static int el_send_udp_rtcp(peer_echolink_t *p, struct in_addr to,
                            const void *data, int len)
{
    struct sockaddr_in dest;

    if (!p || !data || len <= 0)
        return -1;
    if (p->use_proxy)
        return el_proxy_udp_ctrl(&p->proxy, to, data, len);
    if (p->rtcp_sock < 0)
        return -1;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(EL_RTCP_PORT);
    dest.sin_addr = to;
    return sendto(p->rtcp_sock, data, (size_t)len, 0,
                  (struct sockaddr *)&dest, sizeof(dest)) == len
               ? 0
               : -1;
}

static int el_send_udp_rtp(peer_echolink_t *p, struct in_addr to,
                           const void *data, int len)
{
    struct sockaddr_in dest;

    if (!p || !data || len <= 0)
        return -1;
    if (p->use_proxy)
        return el_proxy_udp_data(&p->proxy, to, data, len);
    if (p->rtp_sock < 0)
        return -1;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(EL_RTP_PORT);
    dest.sin_addr = to;
    return sendto(p->rtp_sock, data, (size_t)len, 0,
                  (struct sockaddr *)&dest, sizeof(dest)) == len
               ? 0
               : -1;
}

static void el_proxy_udp_cb(void *user, struct in_addr from, int is_ctrl,
                            const uint8_t *data, int len)
{
    peer_echolink_t *p = (peer_echolink_t *)user;
    struct sockaddr_in sa;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr = from;
    sa.sin_port = htons(is_ctrl ? EL_RTCP_PORT : EL_RTP_PORT);
    if (is_ctrl)
        el_handle_rtcp(p, data, len, &sa);
    else if (data[0] != 0x6f)
        el_handle_rtp(p, data, len, &sa);
}

typedef struct {
    int fd; /* >=0 direct TCP; -1 = open on proxy */
} el_dir_conn_t;

static int el_dir_connect(peer_echolink_t *p, const char *server, el_dir_conn_t *c)
{
    struct in_addr ip;
    int rc;

    if (!p || !c)
        return -1;
    c->fd = -1;
    if (p->use_proxy) {
        if (el_proxy_ensure(&p->proxy) != 0)
            return -1;
        if (resolve_dns(server, &ip) != 0)
            return -1;
        return el_proxy_tcp_open(&p->proxy, ip, EL_DIR_LOOKUP_TIMEOUT_SEC * 1000);
    }
    rc = el_tcp_bind_connect(p, server, &c->fd);
    if (rc == 0) {
        pthread_mutex_lock(&p->dir_mu);
        p->dir_fd = c->fd;
        pthread_mutex_unlock(&p->dir_mu);
    }
    return rc;
}

static int el_dir_write(peer_echolink_t *p, el_dir_conn_t *c, const void *buf, int len)
{
    if (!p || !c || !buf || len <= 0)
        return -1;
    if (p->use_proxy)
        return el_proxy_tcp_write(&p->proxy, buf, len);
    if (c->fd < 0)
        return -1;
    return write(c->fd, buf, (size_t)len) == len ? 0 : -1;
}

static int el_dir_read(peer_echolink_t *p, el_dir_conn_t *c, void *buf, int len)
{
    if (!p || !c || !buf || len <= 0)
        return -1;
    if (p->use_proxy)
        return el_proxy_tcp_read(&p->proxy, buf, len,
                                 EL_DIR_LOOKUP_TIMEOUT_SEC * 1000);
    if (c->fd < 0)
        return -1;
    return (int)read(c->fd, buf, (size_t)len);
}

static void el_dir_close(peer_echolink_t *p, el_dir_conn_t *c)
{
    if (!p || !c)
        return;
    if (p->use_proxy) {
        el_proxy_tcp_close(&p->proxy);
        return;
    }
    if (c->fd >= 0) {
        pthread_mutex_lock(&p->dir_mu);
        if (p->dir_fd == c->fd)
            p->dir_fd = -1;
        pthread_mutex_unlock(&p->dir_mu);
        close(c->fd);
        c->fd = -1;
    }
}

#define EL_SDES_INTERVAL      5
/* Peer silent longer than tlb ConfMemberTimeout → treat as unlinked. */
#define EL_PEER_STALE_SEC     45
/* tlb: first re-login is delayed 60s after startup LOGIN_AND_LIST */
#define EL_FIRST_RELOGIN_DELAY 60
#define EL_RTP_VERSION        3
#define EL_RTP_PT_GSM         3
#define EL_RTCP_RR            201
#define EL_RTCP_SDES          202
#define EL_RTCP_BYE           203
/* RTP/RTCP arrival gap after which a talk spurt is considered over and a
 * different EchoLink source (outbound peer or an inbound[] slot) may become
 * the active talker. Coarse (1-tick) on purpose -- matches this file's
 * existing time_t-second granularity (EL_PEER_STALE_SEC and friends). */
#define EL_TALK_HANG_SEC      1
/* No RTP from the current talker for this long -> considered done talking;
 * talk_src clears back to -1 and the roster's "->" arrow disappears (tick-
 * driven, since el_handle_rtp only ever runs while someone IS transmitting). */
#define EL_TALK_SILENCE_SEC   2
#define EL_SDES_CNAME         1
#define EL_SDES_NAME          2
#define EL_SDES_EMAIL         3
#define EL_SDES_PHONE         4
#define EL_SDES_LOC           5
#define EL_SDES_TOOL          6
#define EL_SDES_END           0

static int udp_bind(const char *addr, int port, struct sockaddr_in *out)
{
    int s;
    int on = 1;

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)port);
    if (inet_aton(addr, &out->sin_addr) == 0) {
        close(s);
        return -1;
    }
    if (bind(s, (struct sockaddr *)out, sizeof(*out)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

/* Bounded copy without -Wstringop-truncation on same-sized src/dst arrays. */
static void copy_z(char *dst, size_t dstlen, const char *src)
{
    size_t n;

    if (!dst || dstlen == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strnlen(src, dstlen - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void pcm_ring_push(peer_echolink_t *p, const int16_t *pcm, int n)
{
    int i;
    int cap = (int)(sizeof(p->pcm_in) / sizeof(p->pcm_in[0]));

    for (i = 0; i < n; i++) {
        if (p->pcm_in_count >= cap)
            break;
        p->pcm_in[p->pcm_in_w] = pcm[i];
        p->pcm_in_w = (p->pcm_in_w + 1) % cap;
        p->pcm_in_count++;
    }
}

static int resolve_dns(const char *host, struct in_addr *out)
{
    struct hostent *he;

    if (inet_aton(host, out))
        return 0;
    he = gethostbyname(host);
    if (!he)
        return -1;
    memcpy(out, he->h_addr, (size_t)he->h_length);
    return 0;
}

static int looks_like_ipv4(const char *s)
{
    int a, b, c, d;
    char extra;

    if (!s || !*s)
        return 0;
    return sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &extra) == 4;
}

static void upper_copy(char *dst, size_t dstlen, const char *src)
{
    size_t i;

    for (i = 0; i + 1 < dstlen && src[i]; i++)
        dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = '\0';
}

static void trim_cr(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) {
        s[n - 1] = '\0';
        n--;
    }
}

/* EchoLink directory IP scramble (tlb DeScrambleIP / iLink). */
static void el_descramble_ip(char *io)
{
    static const char key[] = "91182092262753893";
    static const char sub1[] = "4325198076";
    static const char sub2[] = "3719458602";
    int input_len;
    int i;
    int key_offset = 0;

    if (!io)
        return;
    input_len = (int)strlen(io);
    for (i = 0; i < input_len - 2; i++) {
        io[i] = (char)(io[i] - key[key_offset++]);
        if (key_offset > 14)
            key_offset = 0;
    }
    for (i = 0; i < input_len; i++) {
        if (isdigit((unsigned char)io[i]))
            io[i] = sub1[io[i] - '0'];
        else if (io[i] == '}')
            io[i] = '.';
    }
    for (i = 0; i < input_len; i++) {
        if (isdigit((unsigned char)io[i]))
            io[i] = sub2[io[i] - '0'];
        else if (io[i] == '=')
            io[i] = '.';
    }
}

static int el_tcp_bind_connect(peer_echolink_t *p, const char *server, int *out_fd)
{
    int fd;
    struct sockaddr_in sa, local;
    struct in_addr ip;
    struct timeval tv;

    if (resolve_dns(server, &ip) != 0)
        return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    tv.tv_sec = EL_DIR_LOOKUP_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = 0;
    /* Direct mode binds to bind_addr; with proxy we never reach here. */
    if (!p->bind_addr[0] || inet_aton(p->bind_addr, &local.sin_addr) == 0
        || bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(fd);
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(EL_DIR_PORT);
    sa.sin_addr = ip;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    *out_fd = fd;
    return 0;
}

/* tlb OurNodeID = crc32(callsign) — used in RTCP RR/SDES SSRC. */
static uint32_t el_node_id(const peer_echolink_t *p)
{
    if (!p || !p->callsign[0])
        return 0;
    return (uint32_t)crc32(0L, (const Bytef *)p->callsign,
                           (uInt)strlen(p->callsign));
}

/* Finish SDES after END item: tlb-style pad count + RTCP length. */
static int el_finish_sdes(uint8_t *out, int o, int sdes_start)
{
    int pad = (4 - (o & 3)) & 3;
    int words;

    if (pad > 0) {
        int i;

        out[sdes_start] |= 0x20; /* padding bit */
        for (i = 0; i < pad - 1; i++)
            out[o++] = 0;
        out[o++] = (uint8_t)pad;
    } else {
        out[sdes_start] &= (uint8_t)~0x20;
    }
    words = ((o - sdes_start) / 4) - 1;
    out[sdes_start + 2] = (uint8_t)((words >> 8) & 0xff);
    out[sdes_start + 3] = (uint8_t)(words & 0xff);
    return o;
}

/* memmem for station-list end marker (avoid needing _GNU_SOURCE). */
static const void *el_memmem(const void *hay, size_t haylen,
                             const void *needle, size_t nlen)
{
    const uint8_t *h = (const uint8_t *)hay;
    const uint8_t *n = (const uint8_t *)needle;
    size_t i;

    if (nlen == 0 || haylen < nlen)
        return NULL;
    for (i = 0; i + nlen <= haylen; i++) {
        if (memcmp(h + i, n, nlen) == 0)
            return h + i;
    }
    return NULL;
}

/*
 * Resolve IP from a directory row. Modern compressed 'S' lists often carry
 * plaintext dotted IPs; classic scrambled lists need el_descramble_ip.
 */
static int el_dir_ip_aton(const char *raw, int scrambled, struct in_addr *out,
                          char *ip_out, size_t ip_out_len)
{
    char trybuf[96];

    if (!raw || !out)
        return -1;
    if (scrambled) {
        strncpy(trybuf, raw, sizeof(trybuf) - 1);
        trybuf[sizeof(trybuf) - 1] = '\0';
        el_descramble_ip(trybuf);
        if (inet_aton(trybuf, out)) {
            if (ip_out && ip_out_len)
                copy_z(ip_out, ip_out_len, trybuf);
            return 0;
        }
    }
    if (looks_like_ipv4(raw) && inet_aton(raw, out)) {
        if (ip_out && ip_out_len)
            copy_z(ip_out, ip_out_len, raw);
        return 0;
    }
    return -1;
}

/* Parse decompressed/plain station list text for want; 0 = found. */
static int el_parse_station_list_text(const char *text, const char *want,
                                      int scrambled, struct in_addr *out,
                                      const char *server)
{
    char line[256];
    char call[32], ipstr[96], ipshow[96];
    size_t linelen = 0;
    int state = 0; /* 0=@@@, 1=count, 2=call, 3=qth, 4=nodeid, 5=ip */
    const char *p;

    if (!text || !want)
        return -1;
    for (p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            line[linelen] = '\0';
            trim_cr(line);
            if (state == 0) {
                if (strncmp(line, "@@@", 3) == 0)
                    state = 1;
            } else if (state == 1) {
                state = 2;
            } else if (strcmp(line, "+++") == 0) {
                return -1;
            } else if (state == 2) {
                upper_copy(call, sizeof(call), line);
                state = 3;
            } else if (state == 3) {
                state = 4;
            } else if (state == 4) {
                state = 5;
            } else if (state == 5) {
                strncpy(ipstr, line, sizeof(ipstr) - 1);
                ipstr[sizeof(ipstr) - 1] = '\0';
                if (strcmp(call, want) == 0) {
                    if (el_dir_ip_aton(ipstr, scrambled, out, ipshow,
                                       sizeof(ipshow)) == 0) {
                        LOG_EL_INFO("echolink: %s -> %s via %s\n",
                                    want, ipshow, server);
                        return 0;
                    }
                    LOG_EL_WARNING("echolink: bad IP for %s (raw=%s)\n",
                                   want, ipstr);
                    return -1;
                }
                state = 2;
            }
            linelen = 0;
            if (*p == '\0')
                break;
        } else if (linelen + 1 < sizeof(line)) {
            line[linelen++] = *p;
        }
    }
    return -1;
}

/*
 * Look up EchoLink node/conference callsign in the directory station list.
 * host may be CA5RPY-L or *REDCHILE*. Returns 0 and fills *out on success.
 * Handles plain @@@ lists and zlib-compressed snapshots (tlb dirclient).
 */
static int el_directory_lookup_on_server(peer_echolink_t *p, const char *server,
                                         const char *want, int scrambled,
                                         struct in_addr *out)
{
    uint8_t *raw = NULL;
    char *text = NULL;
    size_t raw_len = 0, raw_cap = 0;
    el_dir_conn_t conn;
    int n, rc = -1;
    const char *req = scrambled ? "S" : "s";

    if (el_dir_connect(p, server, &conn) != 0)
        return -1;
    if (el_dir_write(p, &conn, req, 1) != 0) {
        el_dir_close(p, &conn);
        return -1;
    }

    for (;;) {
        if (raw_len + 8192 > raw_cap) {
            size_t ncap = raw_cap ? raw_cap * 2 : 65536;
            uint8_t *nr;

            if (ncap > EL_STATION_LIST_MAX + 16)
                ncap = EL_STATION_LIST_MAX + 16;
            if (raw_len >= ncap)
                break;
            nr = (uint8_t *)realloc(raw, ncap);
            if (!nr)
                break;
            raw = nr;
            raw_cap = ncap;
        }
        n = el_dir_read(p, &conn, raw + raw_len, (int)(raw_cap - raw_len));
        if (n <= 0)
            break;
        raw_len += (size_t)n;
        if (raw_len >= 4 && raw[0] == '@' && raw[1] == '@' && raw[2] == '@') {
            if (el_memmem(raw, raw_len, "+++\n", 4)
                || el_memmem(raw, raw_len, "+++\r\n", 5))
                break;
        }
        /* Compressed lists: 4-byte uncompressed size + zlib; read to EOF. */
        if (raw_len >= EL_STATION_LIST_MAX + 4)
            break;
    }
    el_dir_close(p, &conn);
    if (!raw || raw_len < 4)
        goto out;

    if (raw[0] == '@' && raw[1] == '@' && raw[2] == '@') {
        text = (char *)malloc(raw_len + 1);
        if (!text)
            goto out;
        memcpy(text, raw, raw_len);
        text[raw_len] = '\0';
    } else {
        uint32_t uncomp_len;
        uLongf dest_len;
        int zerr;

        /* Prefix is uncompressed size (tlb); payload is zlib of remaining bytes. */
        memcpy(&uncomp_len, raw, 4);
        if (uncomp_len == 0 || uncomp_len > EL_STATION_LIST_MAX || raw_len <= 4)
            goto out;
        text = (char *)malloc((size_t)uncomp_len + 1);
        if (!text)
            goto out;
        dest_len = uncomp_len;
        zerr = uncompress((Bytef *)text, &dest_len, raw + 4,
                          (uLong)(raw_len - 4));
        if (zerr != Z_OK) {
            LOG_EL_WARNING("echolink: station list zlib failed (%d) on %s\n",
                           zerr, server);
            free(text);
            text = NULL;
            goto out;
        }
        text[dest_len] = '\0';
    }

    rc = el_parse_station_list_text(text, want, scrambled, out, server);

out:
    free(text);
    free(raw);
    return rc;
}

static int el_directory_lookup_node(peer_echolink_t *p, const char *node, struct in_addr *out)
{
    char want[32];
    int i;

    if (!node || !*node || p->directory_server_count <= 0)
        return -1;
    upper_copy(want, sizeof(want), node);

    /* EchoLink iLink list ('S' + scrambled IP), then legacy 's'. */
    for (i = 0; i < p->directory_server_count; i++) {
        if (el_directory_lookup_on_server(p, p->directory_servers[i], want, 1, out) == 0)
            return 0;
    }
    for (i = 0; i < p->directory_server_count; i++) {
        if (el_directory_lookup_on_server(p, p->directory_servers[i], want, 0, out) == 0)
            return 0;
    }
    return -1;
}

static int el_resolve_peer(peer_echolink_t *p, struct in_addr *out)
{
    if (!p->host[0])
        return -1;
    /* Lab escape hatch: dotted IPv4 still accepted */
    if (looks_like_ipv4(p->host))
        return inet_aton(p->host, out) ? 0 : -1;
    return el_directory_lookup_node(p, p->host, out);
}

static void el_set_peer_addr(peer_echolink_t *p, struct in_addr ip)
{
    memset(&p->peer_rtp, 0, sizeof(p->peer_rtp));
    p->peer_rtp.sin_family = AF_INET;
    p->peer_rtp.sin_port = htons(EL_RTP_PORT);
    p->peer_rtp.sin_addr = ip;
    p->peer_rtcp = p->peer_rtp;
    p->peer_rtcp.sin_port = htons(EL_RTCP_PORT);
    p->peer_resolved = 1;
}

/* Refresh peer IP from directory (or literal). Updates address if it changed. */
static int el_station_list_refresh_peer(peer_echolink_t *p)
{
    struct in_addr ip;
    char old_ip[INET_ADDRSTRLEN];
    char new_ip[INET_ADDRSTRLEN];
    int had_peer;
    uint32_t old_addr = 0;

    if (!p->host[0])
        return 0;
    /* TCP/DNS lookup — must not hold dir_mu (audio path needs it briefly). */
    if (el_resolve_peer(p, &ip) != 0) {
        LOG_EL_WARNING("echolink: station list: %s not found\n", p->host);
        return -1;
    }

    pthread_mutex_lock(&p->dir_mu);
    had_peer = p->peer_resolved;
    if (had_peer)
        old_addr = p->peer_rtp.sin_addr.s_addr;
    if (had_peer && old_addr == ip.s_addr) {
        pthread_mutex_unlock(&p->dir_mu);
        return 0;
    }
    if (had_peer) {
        struct in_addr oa;

        oa.s_addr = old_addr;
        inet_ntop(AF_INET, &oa, old_ip, sizeof(old_ip));
        inet_ntop(AF_INET, &ip, new_ip, sizeof(new_ip));
        LOG_EL_INFO("echolink: %s IP changed %s -> %s\n", p->host, old_ip, new_ip);
    } else {
        LOG_EL_INFO("echolink: connecting to %s (%s)\n", p->host, inet_ntoa(ip));
    }
    el_set_peer_addr(p, ip);
    pthread_mutex_unlock(&p->dir_mu);
    /* Same as tlb CmdConnect: direct SDES + directory-relayed OPEN. */
    el_send_connect_handshake(p);
    return 0;
}

/* Keep RTCP alive around blocking directory TCP (tlb ConfMemberTimeout). */
static void el_dir_op_begin(peer_echolink_t *p)
{
    el_send_sdes(p);
}

static void el_dir_op_end(peer_echolink_t *p)
{
    el_send_sdes(p);
}

/* tlb SERV_REQ_LOGIN_AND_LIST */
static void el_login_and_list(peer_echolink_t *p)
{
    el_dir_op_begin(p);
    el_try_directory_login(p);
    if (p->station_list_interval > 0)
        el_station_list_refresh_peer(p);
    el_dir_op_end(p);
}

/* tlb SERV_REQ_STATION_LIST */
static void el_station_list_only(peer_echolink_t *p)
{
    el_dir_op_begin(p);
    if (p->station_list_interval > 0)
        el_station_list_refresh_peer(p);
    el_dir_op_end(p);
}

/* Schedule a directory job on the worker; returns 0 if accepted, -1 if busy/stopped. */
static int el_dir_schedule(peer_echolink_t *p, int job)
{
    if (!p || !p->dir_thread_on || job == EL_DIR_JOB_NONE)
        return -1;
    pthread_mutex_lock(&p->dir_mu);
    if (p->dir_stop || p->dir_busy || p->dir_job != EL_DIR_JOB_NONE) {
        pthread_mutex_unlock(&p->dir_mu);
        return -1;
    }
    p->dir_job = job;
    pthread_cond_signal(&p->dir_cv);
    pthread_mutex_unlock(&p->dir_mu);
    return 0;
}

static void *el_dir_thread_main(void *arg)
{
    peer_echolink_t *p = (peer_echolink_t *)arg;

    for (;;) {
        int job;

        pthread_mutex_lock(&p->dir_mu);
        while (!p->dir_stop && p->dir_job == EL_DIR_JOB_NONE)
            pthread_cond_wait(&p->dir_cv, &p->dir_mu);
        if (p->dir_stop) {
            pthread_mutex_unlock(&p->dir_mu);
            break;
        }
        job = p->dir_job;
        p->dir_job = EL_DIR_JOB_NONE;
        p->dir_busy = 1;
        pthread_mutex_unlock(&p->dir_mu);

        switch (job) {
        case EL_DIR_JOB_LOGIN:
            el_dir_op_begin(p);
            el_try_directory_login(p);
            el_dir_op_end(p);
            break;
        case EL_DIR_JOB_LIST:
            el_station_list_only(p);
            break;
        case EL_DIR_JOB_LOGIN_LIST:
            el_login_and_list(p);
            break;
        default:
            break;
        }

        pthread_mutex_lock(&p->dir_mu);
        p->dir_busy = 0;
        pthread_mutex_unlock(&p->dir_mu);
    }
    return NULL;
}

static int el_dir_thread_start(peer_echolink_t *p)
{
    int err;

    p->dir_stop = 0;
    p->dir_busy = 0;
    p->dir_job = EL_DIR_JOB_NONE;
    p->dir_fd = -1;
    err = pthread_create(&p->dir_tid, NULL, el_dir_thread_main, p);
    if (err != 0) {
        LOG_EL_WARNING("echolink: directory thread create failed (%s); "
                       "periodic refresh will block the audio loop\n",
                       strerror(err));
        p->dir_thread_on = 0;
        return -1;
    }
    p->dir_thread_on = 1;
    return 0;
}

static void el_dir_thread_stop(peer_echolink_t *p)
{
    int fd;

    if (!p->dir_thread_on)
        return;
    pthread_mutex_lock(&p->dir_mu);
    p->dir_stop = 1;
    fd = p->dir_fd;
    pthread_cond_signal(&p->dir_cv);
    pthread_mutex_unlock(&p->dir_mu);
    /* Force any in-flight connect()/read() to return now instead of making
     * shutdown wait out the directory timeout (up to EL_DIR_LOOKUP_TIMEOUT_SEC,
     * possibly repeated across a station-list read loop). */
    if (fd >= 0)
        shutdown(fd, SHUT_RDWR);
    pthread_join(p->dir_tid, NULL);
    p->dir_thread_on = 0;
    p->dir_busy = 0;
    p->dir_job = EL_DIR_JOB_NONE;
}

/* Directory TCP login — returns 0 on OK. */
static int el_directory_login(peer_echolink_t *p, const char *server)
{
    el_dir_conn_t conn;
    char body[512];
    char ack[8];
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm;
    int n, body_len;
    uint8_t l = 'l';

    if (el_dir_connect(p, server, &conn) != 0) {
        LOG_EL_WARNING("echolink: cannot connect directory %s\n", server);
        return -1;
    }

    tm = localtime_r(&now, &tm_buf);
    /* callsign AC AC password \r STATUS version B(HH:DD) \r QTH \r email \r */
    body_len = snprintf(body, sizeof(body),
                        "%s%c%c%s\rONLINE%sB(%02d:%02d)\r%s\r%s\r",
                        p->callsign, 0xac, 0xac, p->password,
                        "0.56",
                        tm ? tm->tm_hour : 0, tm ? tm->tm_mday : 1,
                        p->qth[0] ? p->qth : "ADN",
                        p->email[0] ? p->email : "");
    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        el_dir_close(p, &conn);
        return -1;
    }

    if (el_dir_write(p, &conn, &l, 1) != 0
        || el_dir_write(p, &conn, body, body_len) != 0) {
        el_dir_close(p, &conn);
        return -1;
    }

    n = el_dir_read(p, &conn, ack, (int)sizeof(ack) - 1);
    el_dir_close(p, &conn);
    if (n < 2) {
        LOG_EL_WARNING("echolink: directory %s short reply\n", server);
        return -1;
    }
    ack[n] = '\0';
    if (strncmp(ack, "OK", 2) == 0) {
        LOG_EL_INFO("echolink: directory login OK (%s as %s)\n", server, p->callsign);
        /* Do not demote PEER_EL_CONNECTED — login refresh must not clear link state. */
        pthread_mutex_lock(&p->dir_mu);
        if (p->status != PEER_EL_CONNECTED)
            p->status = PEER_EL_DIR_OK;
        p->last_dir_login = now;
        pthread_mutex_unlock(&p->dir_mu);
        return 0;
    }
    LOG_EL_WARNING("echolink: directory login rejected by %s: %.16s\n", server, ack);
    return -1;
}

static int el_try_directory_login(peer_echolink_t *p)
{
    int i;

    for (i = 0; i < p->directory_server_count; i++) {
        if (el_directory_login(p, p->directory_servers[i]) == 0)
            return 0;
    }
    return -1;
}

/*
 * Build RTCP RR + SDES (EchoLink / tlb GenSDES).
 * Wire identity (tlb CallSignString): CNAME and EMAIL are the literal
 * "CALLSIGN"; the real station callsign lives in NAME. Conferences
 * (thebridge / EchoLink soft) expect this; -L/-R nodes are often lenient.
 * Firewall OPEN keeps the real callsign in CNAME (SendFirewallOpenRequest).
 */
static int el_build_sdes(peer_echolink_t *p, uint8_t *out, int outlen)
{
    static const char callsign_lit[] = "CALLSIGN";
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    char phone[16];
    char tool[] = "adn-bridge";
    const char *name_txt;
    uint32_t nid;
    int o = 0;
    int name_len, cname_len, email_len, phone_len, tool_len;
    int sdes_start;
    int need;

    if (outlen < 128)
        return -1;

    snprintf(phone, sizeof(phone), "%02d:%02d",
             tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);

    cname_len = (int)strlen(callsign_lit);
    email_len = cname_len;
    /* NAME = our callsign, or bridged talker for conference display. */
    name_txt = p->talker_name[0] ? p->talker_name : p->callsign;
    name_len = (int)strlen(name_txt);
    phone_len = (int)strlen(phone);
    tool_len = (int)strlen(tool);
    /* RR(8) + SDES hdr(8) + items + END + pad(<=3) */
    need = 8 + 8 + (2 + cname_len) + (2 + name_len) + (2 + email_len)
           + (2 + phone_len) + (2 + tool_len) + 1 + 3;
    if (need > outlen)
        return -1;

    nid = el_node_id(p);

    /* RR */
    out[o++] = (uint8_t)((EL_RTP_VERSION << 6)); /* v=3, p=0, count=0 */
    out[o++] = EL_RTCP_RR;
    out[o++] = 0;
    out[o++] = 1; /* length = 1 word after common header */
    memcpy(out + o, &nid, 4);
    o += 4;

    sdes_start = o;
    out[o++] = (uint8_t)((EL_RTP_VERSION << 6) | 1); /* v=3, p filled later */
    out[o++] = EL_RTCP_SDES;
    out[o++] = 0;
    out[o++] = 0; /* length filled later */
    memcpy(out + o, &nid, 4);
    o += 4;

    out[o++] = EL_SDES_CNAME;
    out[o++] = (uint8_t)cname_len;
    memcpy(out + o, callsign_lit, (size_t)cname_len);
    o += cname_len;

    out[o++] = EL_SDES_NAME;
    out[o++] = (uint8_t)name_len;
    memcpy(out + o, name_txt, (size_t)name_len);
    o += name_len;

    out[o++] = EL_SDES_EMAIL;
    out[o++] = (uint8_t)email_len;
    memcpy(out + o, callsign_lit, (size_t)email_len);
    o += email_len;

    out[o++] = EL_SDES_PHONE;
    out[o++] = (uint8_t)phone_len;
    memcpy(out + o, phone, (size_t)phone_len);
    o += phone_len;

    out[o++] = EL_SDES_TOOL;
    out[o++] = (uint8_t)tool_len;
    memcpy(out + o, tool, (size_t)tool_len);
    o += tool_len;

    out[o++] = EL_SDES_END;
    return el_finish_sdes(out, o, sdes_start);
}

static void el_send_sdes(peer_echolink_t *p)
{
    uint8_t buf[256];
    struct in_addr to;
    int n;

    pthread_mutex_lock(&p->dir_mu);
    if (!el_rtcp_ok(p)) {
        pthread_mutex_unlock(&p->dir_mu);
        return;
    }
    to = p->peer_rtcp.sin_addr;
    n = el_build_sdes(p, buf, (int)sizeof(buf));
    if (n > 0)
        p->last_sdes = time(NULL);
    pthread_mutex_unlock(&p->dir_mu);
    if (n <= 0)
        return;
    el_send_udp_rtcp(p, to, buf, n);
}

/*
 * Reply to an inbound EchoLink station (not the configured outbound peer) --
 * doesn't gate on el_rtcp_ok()/peer_resolved (those describe the OUTBOUND
 * target only) and doesn't touch last_sdes (the outbound keepalive cadence
 * tracker). Called only from the main/poll-loop thread (RTCP dispatch), so
 * no dir_mu needed -- `to` is a plain local value here, not shared state.
 */
static void el_send_sdes_to(peer_echolink_t *p, struct in_addr to)
{
    uint8_t buf[256];
    int n = el_build_sdes(p, buf, (int)sizeof(buf));

    if (n > 0)
        el_send_udp_rtcp(p, to, buf, n);
}

/*
 * Build an RTCP BYE (thelinkbox GenBye format, conference.c: RR + BYE chunk
 * with SSRC + reason string, padded to a 4-byte boundary) -- sent instead of
 * a silent drop when rejecting an inbound connection (unauthorized callsign,
 * or node at capacity).
 */
static int el_build_bye(peer_echolink_t *p, const char *reason, uint8_t *out, int outlen)
{
    uint32_t nid;
    int o = 0;
    int reason_len = (int)strlen(reason);
    int bye_start;
    int need = 8 + 8 + 1 + reason_len + 3; /* RR(8) + BYE hdr(8) + len byte + reason + pad(<=3) */

    if (need > outlen)
        return -1;

    nid = el_node_id(p);

    out[o++] = (uint8_t)(EL_RTP_VERSION << 6);
    out[o++] = EL_RTCP_RR;
    out[o++] = 0;
    out[o++] = 1;
    memcpy(out + o, &nid, 4);
    o += 4;

    bye_start = o;
    out[o++] = (uint8_t)((EL_RTP_VERSION << 6) | 1); /* p filled by el_finish_sdes */
    out[o++] = EL_RTCP_BYE;
    out[o++] = 0;
    out[o++] = 0; /* length filled by el_finish_sdes */
    memcpy(out + o, &nid, 4);
    o += 4;

    out[o++] = (uint8_t)reason_len;
    memcpy(out + o, reason, (size_t)reason_len);
    o += reason_len;

    return el_finish_sdes(out, o, bye_start);
}

static void el_send_bye_to(peer_echolink_t *p, struct in_addr to, const char *reason)
{
    uint8_t buf[128];
    int n = el_build_bye(p, reason, buf, (int)sizeof(buf));

    if (n > 0)
        el_send_udp_rtcp(p, to, buf, n);
}

/*
 * tlb SendStationList (conference.c): the EchoLink app's "connected users"
 * list and its conference welcome text are the SAME thing -- there is no
 * separate one-shot welcome packet. It's an unauthenticated DATA packet (not
 * RTP audio) sent on the *audio* port (5198), marked by a leading 0x6f byte
 * instead of the usual RTP version bits, followed by the literal ASCII
 * "NDATA" and then free text with lines separated by '\r', NUL-terminated
 * (the NUL is part of the sent length).
 *
 * Matches a real conference's wire format (observed against RedChile.org):
 *   CONF <callsign> [<count>/<max>]
 *   --
 *   <welcome_text, verbatim>
 *   --
 *   (blank line)
 *   ->TALKER          <- "->" PREFIXES the currently transmitting station
 *   OTHER_STATION      (no annotation at all for stations that aren't)
 * one line per currently connected station (the outbound peer, if linked,
 * plus every inbound[] slot); talk_src picks which one gets the "->" arrow
 * (same transmitting/receiving feedback as SvxLink's node status).
 */
#define EL_DATA_MARKER 0x6f

static int el_append(char *out, int o, int outlen, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(out + o, (size_t)(outlen - o), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= outlen - o)
        return -1;
    return o + n;
}

static int el_build_station_list(peer_echolink_t *p, uint8_t *out, int outlen)
{
    int o;
    int i;
    int count = 0;

    for (i = 0; i < EL_MAX_INBOUND; i++)
        if (p->inbound[i].used)
            count++;
    if (p->linked)
        count++;

    o = el_append((char *)out, 0, outlen, "%cNDATACONF %s [%d/%d]\r",
                 EL_DATA_MARKER, p->callsign, count, p->max_inbound);
    if (o < 0)
        return -1;

    if (p->welcome_text[0]) {
        o = el_append((char *)out, o, outlen, "--\r%s\r--\r", p->welcome_text);
        if (o < 0)
            return -1;
    }
    o = el_append((char *)out, o, outlen, "\r");
    if (o < 0)
        return -1;

    if (p->linked) {
        o = el_append((char *)out, o, outlen, "%s%s\r",
                     p->talk_src == 0 ? "->" : "",
                     p->remote_talker[0] ? p->remote_talker : p->host);
        if (o < 0)
            return -1;
    }
    for (i = 0; i < EL_MAX_INBOUND; i++) {
        if (!p->inbound[i].used)
            continue;
        o = el_append((char *)out, o, outlen, "%s%s\r",
                     p->talk_src == i + 1 ? "->" : "", p->inbound[i].cname);
        if (o < 0)
            return -1;
    }
    /* The bridge leg currently relaying audio INTO EchoLink (DMR/YSF/...),
     * if any -- e.g. "DMR 7141001" or "YSF N0CALL". Always shown as the
     * active talker: setting relay_label inherently means that leg is
     * transmitting right now (see peer_el_set_relay_label callers). */
    if (p->relay_label[0]) {
        o = el_append((char *)out, o, outlen, "->%s\r", p->relay_label);
        if (o < 0)
            return -1;
    }
    if (o >= outlen)
        return -1;
    out[o++] = 0; /* terminator, included in the sent length (tlb convention) */
    return o;
}

static void el_send_station_list_to(peer_echolink_t *p, struct in_addr to)
{
    uint8_t buf[512];
    int n = el_build_station_list(p, buf, (int)sizeof(buf));

    if (n > 0)
        el_send_udp_rtp(p, to, buf, n);
}

/*
 * Broadcast the roster/welcome blob to every currently connected inbound
 * station -- called whenever inbound membership changes (join/leave), same
 * trigger as tlb's bSendStationList dirty flag (conference.c:2049-2058).
 * Not sent to the configured outbound peer (that's typically a repeater/
 * conference bridge, not an app UI needing a roster).
 */
static void el_broadcast_station_list(peer_echolink_t *p)
{
    int i;

    if (p->use_proxy)
        return; /* same proxy.mu deadlock guard as the SDES/BYE sends above */
    for (i = 0; i < EL_MAX_INBOUND; i++)
        if (p->inbound[i].used)
            el_send_station_list_to(p, p->inbound[i].addr.sin_addr);
}

/*
 * K1RFD / tlb SendFirewallOpenRequest: RTCP SDES to a directory server so it
 * can relay OPEN to the peer (needed for many conferences and NAT paths).
 *   CNAME = our callsign
 *   LOC   = "OPEN"
 *   EMAIL = peer dotted IP
 */
static int el_build_firewall_open(peer_echolink_t *p, const char *dest_ip,
                                  uint8_t *out, int outlen)
{
    const char open_loc[] = "OPEN";
    uint32_t nid;
    int o = 0;
    int cname_len, loc_len, email_len;
    int sdes_start;
    int need;

    if (!p || !dest_ip || !dest_ip[0] || outlen < 128)
        return -1;
    cname_len = (int)strlen(p->callsign);
    loc_len = (int)strlen(open_loc);
    email_len = (int)strlen(dest_ip);
    if (cname_len <= 0 || email_len <= 0)
        return -1;
    need = 8 + 8 + (2 + cname_len) + (2 + loc_len) + (2 + email_len) + 1 + 3;
    if (need > outlen)
        return -1;

    nid = el_node_id(p);

    /* RR */
    out[o++] = (uint8_t)(EL_RTP_VERSION << 6);
    out[o++] = EL_RTCP_RR;
    out[o++] = 0;
    out[o++] = 1;
    memcpy(out + o, &nid, 4);
    o += 4;

    sdes_start = o;
    out[o++] = (uint8_t)((EL_RTP_VERSION << 6) | 1); /* p filled by el_finish_sdes */
    out[o++] = EL_RTCP_SDES;
    out[o++] = 0;
    out[o++] = 0;
    memcpy(out + o, &nid, 4);
    o += 4;

    out[o++] = EL_SDES_CNAME;
    out[o++] = (uint8_t)cname_len;
    memcpy(out + o, p->callsign, (size_t)cname_len);
    o += cname_len;

    out[o++] = EL_SDES_LOC;
    out[o++] = (uint8_t)loc_len;
    memcpy(out + o, open_loc, (size_t)loc_len);
    o += loc_len;

    out[o++] = EL_SDES_EMAIL;
    out[o++] = (uint8_t)email_len;
    memcpy(out + o, dest_ip, (size_t)email_len);
    o += email_len;

    out[o++] = EL_SDES_END;
    return el_finish_sdes(out, o, sdes_start);
}

static void el_send_firewall_open(peer_echolink_t *p)
{
    uint8_t buf[256];
    struct in_addr peer_ip;
    struct in_addr dir_ip;
    char peer_ip_str[INET_ADDRSTRLEN];
    int n, i, sent = 0;

    /*
     * Directory-relayed OPEN (still needed for many conferences). With a
     * proxy this goes out as UDP_CONTROL to the directory host — not a
     * local bind punch (proxy already owns public 5198/5199).
     */
    pthread_mutex_lock(&p->dir_mu);
    if (!el_rtcp_ok(p) || p->directory_server_count <= 0) {
        pthread_mutex_unlock(&p->dir_mu);
        return;
    }
    /* Literal IP lab peers: no directory relay (tlb only for EchoLink nodes). */
    if (looks_like_ipv4(p->host)) {
        pthread_mutex_unlock(&p->dir_mu);
        return;
    }
    peer_ip = p->peer_rtp.sin_addr;
    pthread_mutex_unlock(&p->dir_mu);

    if (!inet_ntop(AF_INET, &peer_ip, peer_ip_str, sizeof(peer_ip_str)))
        return;
    n = el_build_firewall_open(p, peer_ip_str, buf, (int)sizeof(buf));
    if (n <= 0)
        return;

    for (i = 0; i < p->directory_server_count; i++) {
        if (resolve_dns(p->directory_servers[i], &dir_ip) != 0)
            continue;
        if (el_send_udp_rtcp(p, dir_ip, buf, n) == 0) {
            LOG_EL_DEBUG("echolink: firewall OPEN for %s (%s) via %s%s\n",
                         p->host, peer_ip_str, p->directory_servers[i],
                         p->use_proxy ? " (proxy)" : "");
            sent = 1;
            break; /* tlb uses one addressing server; first OK is enough */
        }
    }
    if (!sent)
        LOG_EL_WARNING("echolink: firewall OPEN for %s failed (no directory)\n",
                       p->host);
}

/* Direct SDES to peer + directory OPEN (tlb connect path; OPEN skipped with proxy). */
static void el_send_connect_handshake(peer_echolink_t *p)
{
    el_send_sdes(p);
    el_send_firewall_open(p);
}

/* Copy SDES item text into dst (printable ASCII, trimmed). */
static void el_sdes_copy_item(char *dst, size_t dstlen, const uint8_t *data, int ilen)
{
    size_t n = 0;
    int i;

    if (!dst || dstlen == 0)
        return;
    dst[0] = '\0';
    if (!data || ilen <= 0)
        return;
    for (i = 0; i < ilen && n + 1 < dstlen; i++) {
        unsigned char c = data[i];

        if (c < 32 || c > 126)
            continue;
        dst[n++] = (char)c;
    }
    while (n > 0 && dst[n - 1] == ' ')
        n--;
    dst[n] = '\0';
}

/* Copy first whitespace-delimited token, uppercased. */
static void el_copy_token(char *dst, size_t dstlen, const char *src)
{
    size_t n = 0;

    dst[0] = '\0';
    if (!src || dstlen == 0)
        return;
    while (*src == ' ' || *src == '\t')
        src++;
    while (*src && *src != ' ' && *src != '\t' && n + 1 < dstlen) {
        unsigned char c = (unsigned char)*src++;

        if (c < 32 || c > 126)
            continue;
        dst[n++] = (char)toupper(c);
    }
    dst[n] = '\0';
}

/*
 * Accept real station callsigns / conferences; reject status text such as
 * "Conference [2/8]" that thelinkbox puts inside NAME parentheses.
 */
static int el_looks_like_callsign(const char *s)
{
    int i, n = 0, letters = 0, digits = 0;

    if (!s || !s[0])
        return 0;
    if (strncasecmp(s, "CONFERENCE", 10) == 0)
        return 0;
    if (strcasecmp(s, "CALLSIGN") == 0)
        return 0;
    if (strchr(s, '[') || strchr(s, ']'))
        return 0;
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == ' ' || c == '\t')
            break;
        if (isalpha(c)) {
            letters++;
            n++;
            continue;
        }
        if (isdigit(c)) {
            digits++;
            n++;
            continue;
        }
        /* -L/-R, *CONF*, /suffix */
        if (c == '-' || c == '*' || c == '/') {
            n++;
            continue;
        }
        return 0;
    }
    /* Need a letter (or leading * for conferences) and a few chars. */
    if (n < 3)
        return 0;
    if (s[0] == '*')
        return 1;
    return letters > 0;
}

/* Case-insensitive station token compare (HP3ICC vs hp3icc). */
static int el_same_station(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/*
 * Derive remote talker from inbound SDES:
 *   NAME "NODE (CE5ABC) CONF" → CE5ABC (only if paren text looks like callsign)
 *   NAME "CA5RPY-L (Conference [2/8]) CONF" → keep last user if recent RTP
 *   NAME "CALLSIGN Name" → first token
 *   else CNAME / connected host
 *
 * thelinkbox often sends (User Name) while keyed, then Conference [n/m]
 * while RTP is still flowing — keep the user only during that active RX.
 * bridge clears the sticky talker when the EL→DMR/YSF call ends.
 */
/*
 * Pure derivation, shared by el_apply_remote_sdes (outbound peer) and
 * el_handle_inbound_sdes (inbound connections): most EchoLink clients (tlb
 * CallSignString convention) send CNAME as the literal placeholder
 * "CALLSIGN" with the real station identity in NAME instead -- either as
 * "NODE (CE5ABC) CONF" (paren) or "CE5ABC Name" (first token). CNAME is only
 * used as a last resort, for the minority of peers that put a real callsign
 * there directly. Writes an empty string to `out` if nothing plausible is
 * found (out_cap must be >= 1).
 */
static void el_derive_callsign(const char *cname, const char *name, char *out, size_t out_cap,
                               int *from_paren_out)
{
    char cand[64];
    const char *lp, *rp;
    size_t i, n;
    int from_paren = 0;

    out[0] = '\0';
    if (name && name[0]) {
        lp = strrchr(name, '(');
        rp = strrchr(name, ')');
        if (lp && rp && rp > lp + 1) {
            char paren[64];

            n = 0;
            for (i = 1; lp[i] && &lp[i] < rp && n + 1 < sizeof(paren); i++) {
                unsigned char c = (unsigned char)lp[i];

                if (c < 32 || c > 126)
                    continue;
                paren[n++] = (char)c;
            }
            paren[n] = '\0';
            /* "HP3ICC Esteban" / "Conference [2/8]" → first token only. */
            el_copy_token(cand, sizeof(cand), paren);
            if (el_looks_like_callsign(cand)) {
                copy_z(out, out_cap, cand);
                from_paren = 1;
            }
        }
        if (!out[0]) {
            el_copy_token(cand, sizeof(cand), name);
            if (el_looks_like_callsign(cand))
                copy_z(out, out_cap, cand);
        }
    }
    if (!out[0] && cname && cname[0] && el_looks_like_callsign(cname))
        el_copy_token(out, out_cap, cname);
    if (from_paren_out)
        *from_paren_out = from_paren;
}

static void el_apply_remote_sdes(peer_echolink_t *p, const char *cname, const char *name)
{
    char talker[sizeof(p->remote_talker)];
    int from_paren = 0;
    time_t now;

    el_derive_callsign(cname, name, talker, sizeof(talker), &from_paren);
    if (!talker[0] && p->host[0])
        copy_z(talker, sizeof(talker), p->host);

    if (cname && cname[0] && el_looks_like_callsign(cname))
        el_copy_token(p->remote_cname, sizeof(p->remote_cname), cname);
    if (!talker[0])
        return;

    /*
     * Weak update (node / Conference status): keep explicit user only while
     * RTP is still active (~2s). Call-end clears sticky for the next QSO.
     */
    if (!from_paren && p->remote_talker_explicit && p->remote_talker[0]
        && !el_same_station(p->remote_talker, talker)) {
        now = time(NULL);
        if (p->last_rtp_rx != 0 && now >= p->last_rtp_rx
            && (now - p->last_rtp_rx) < 2) {
            return;
        }
    }

    if (strcmp(p->remote_talker, talker) == 0) {
        if (from_paren)
            p->remote_talker_explicit = 1;
        return;
    }
    copy_z(p->remote_talker, sizeof(p->remote_talker), talker);
    p->remote_talker_explicit = from_paren;
    LOG_EL_INFO("echolink: remote talker=%s (cname=%s name=%s)\n",
                p->remote_talker,
                p->remote_cname[0] ? p->remote_cname : "-",
                (name && name[0]) ? name : "-");
}

/*
 * Parse an inbound RTCP SDES chunk, returning CNAME/NAME to the caller
 * (out_cname/out_name, always NUL-terminated, empty if absent). Returns 1 if
 * this is a K1RFD firewall OPEN (directory-relayed, not a peer link/talker
 * update), 0 for a normal SDES, -1 on a malformed packet. The caller decides
 * *where* to apply the result -- the configured outbound peer (unchanged
 * el_apply_remote_sdes path) or an inbound[] slot.
 */
static int el_parse_sdes_packet(const uint8_t *pkt, int plen,
                                char *out_cname, size_t cname_cap,
                                char *out_name, size_t name_cap)
{
    char loc[32];
    int o;
    int count;

    out_cname[0] = '\0';
    out_name[0] = '\0';
    if (plen < 8)
        return -1;
    loc[0] = '\0';
    count = pkt[0] & 0x1f;
    o = 4; /* after RTCP common header */
    while (count-- > 0 && o + 4 <= plen) {
        o += 4; /* SSRC */
        while (o + 1 <= plen) {
            uint8_t type = pkt[o];
            uint8_t ilen = (o + 1 < plen) ? pkt[o + 1] : 0;

            if (type == EL_SDES_END) {
                o++;
                while ((o & 3) != 0 && o < plen)
                    o++;
                break;
            }
            if (o + 2 + ilen > plen)
                return -1;
            if (type == EL_SDES_CNAME)
                el_sdes_copy_item(out_cname, cname_cap, pkt + o + 2, ilen);
            else if (type == EL_SDES_NAME)
                el_sdes_copy_item(out_name, name_cap, pkt + o + 2, ilen);
            else if (type == EL_SDES_LOC)
                el_sdes_copy_item(loc, sizeof(loc), pkt + o + 2, ilen);
            o += 2 + ilen;
        }
    }
    if (loc[0] && strcasecmp(loc, "OPEN") == 0) {
        /* Directory-relayed punch request — not a peer link / talker update. */
        LOG_EL_DEBUG("echolink: got firewall OPEN from %s\n",
                     out_cname[0] ? out_cname : "?");
        return 1;
    }
    return 0;
}

/* blocked_callsigns is checked separately (and first) by the caller -- an
 * explicit block always wins even if the same callsign is also listed in
 * allowed_callsigns. */
static int el_is_blocked_callsign(const peer_echolink_t *p, const char *cname)
{
    int i;

    if (!cname || !cname[0])
        return 0;
    for (i = 0; i < p->blocked_callsign_count; i++)
        if (el_same_station(p->blocked_callsigns[i], cname))
            return 1;
    return 0;
}

/* Case-insensitive lookup of `cname` in the allowed_callsigns allow-list.
 * Empty list = nothing is authorized (no "open node" mode). */
static int el_authorized_callsign(const peer_echolink_t *p, const char *cname)
{
    int i;

    if (!cname || !cname[0])
        return 0;
    for (i = 0; i < p->allowed_callsign_count; i++)
        if (el_same_station(p->allowed_callsigns[i], cname))
            return 1;
    return 0;
}

/*
 * Dispatch an SDES from a source that is neither the configured outbound
 * peer nor an already-accepted inbound[] slot: either a brand new inbound
 * connection attempt, or a keepalive/update from one already accepted.
 * Mirrors thelinkbox RTCP_Rx (conference.c): look up by source address,
 * AuthorizedClient()-style allow-list check, then the capacity ceiling —
 * simplified to a flat allowed_callsigns list and used_count>=max_inbound
 * (no ACL trees / busy flag, not needed at this scale). Unauthorized or
 * over-capacity attempts get a real RTCP BYE (GenBye format), not a silent
 * drop.
 */
static void el_handle_inbound_sdes(peer_echolink_t *p, const struct sockaddr_in *from,
                                   const char *cname, const char *name)
{
    int i;
    int idx = -1, free_idx = -1, used_count = 0;
    char norm[32];

    if (p->max_inbound <= 0)
        return; /* feature disabled -- identical to today's behavior */

    for (i = 0; i < EL_MAX_INBOUND; i++) {
        if (!p->inbound[i].used) {
            if (free_idx < 0)
                free_idx = i;
            continue;
        }
        used_count++;
        if (p->inbound[i].addr.sin_addr.s_addr == from->sin_addr.s_addr)
            idx = i;
    }

    /* Most EchoLink clients send CNAME as the literal placeholder "CALLSIGN"
     * with the real identity in NAME instead -- same derivation as the
     * outbound-peer path (el_apply_remote_sdes), so a plain app connection
     * doesn't get checked against the literal string "CALLSIGN". */
    el_derive_callsign(cname, name, norm, sizeof(norm), NULL);

    if (idx >= 0) {
        /* Already-accepted inbound station: refresh identity/keepalive. */
        if (norm[0])
            copy_z(p->inbound[idx].cname, sizeof(p->inbound[idx].cname), norm);
        p->inbound[idx].last_rtcp = time(NULL);
        /* Under proxy, demux holds proxy.mu here -- sending would deadlock
         * (same reason as the outbound-peer path below). Skip the reply;
         * the station will retry the keepalive on its own timer. */
        if (!p->use_proxy)
            el_send_sdes_to(p, from->sin_addr);
        return;
    }

    if (el_is_blocked_callsign(p, norm)) {
        LOG_EL_WARNING("echolink: rejecting blocked inbound %s from %s\n",
                       norm[0] ? norm : "?", inet_ntoa(from->sin_addr));
        if (!p->use_proxy)
            el_send_bye_to(p, from->sin_addr, "Blocked");
        return;
    }
    if (!el_authorized_callsign(p, norm)) {
        LOG_EL_WARNING("echolink: rejecting unauthorized inbound %s from %s\n",
                       norm[0] ? norm : "?", inet_ntoa(from->sin_addr));
        if (!p->use_proxy)
            el_send_bye_to(p, from->sin_addr, "Not authorized");
        return;
    }
    if (free_idx < 0 || used_count >= p->max_inbound) {
        LOG_EL_WARNING("echolink: rejecting inbound %s from %s (node busy, %d/%d)\n",
                       norm, inet_ntoa(from->sin_addr), used_count, p->max_inbound);
        if (!p->use_proxy)
            el_send_bye_to(p, from->sin_addr, "Node busy");
        return;
    }

    memset(&p->inbound[free_idx], 0, sizeof(p->inbound[free_idx]));
    p->inbound[free_idx].used = 1;
    p->inbound[free_idx].addr = *from;
    copy_z(p->inbound[free_idx].cname, sizeof(p->inbound[free_idx].cname), norm);
    p->inbound[free_idx].last_rtcp = time(NULL);
    LOG_EL_INFO("echolink: inbound connection accepted: %s from %s (%d/%d)\n",
               norm, inet_ntoa(from->sin_addr), used_count + 1, p->max_inbound);
    /* Same proxy.mu deadlock guard as above -- accepted state is tracked
     * either way; under proxy the SDES ack just waits for the station's
     * own retry (matches sdes_reply_pending's existing defer pattern for
     * the outbound peer, not yet extended to per-inbound targets). */
    if (!p->use_proxy) {
        el_send_sdes_to(p, from->sin_addr);
        /* Roster/welcome text -- same blob doubles as both (tlb convention),
         * sent to the new station and re-broadcast to everyone else already
         * connected so their "connected users" view stays current. */
        el_broadcast_station_list(p);
    }
}

static void el_handle_rtcp(peer_echolink_t *p, const uint8_t *data, int len,
                           const struct sockaddr_in *from)
{
    int o = 0;

    while (o + 4 <= len) {
        uint8_t pt = data[o + 1];
        int words = (data[o + 2] << 8) | data[o + 3];
        int plen = (words + 1) * 4;

        if (plen < 4 || o + plen > len)
            break;
        if (pt == EL_RTCP_SDES) {
            char cname[64];
            char name[64];
            int is_open = el_parse_sdes_packet(data + o, plen, cname, sizeof(cname),
                                               name, sizeof(name));

            if (is_open == 1) {
                /*
                 * Reply with normal SDES toward our configured peer so any
                 * NAT mapping stays warm; do not mark linked on OPEN itself.
                 */
                if (p->use_proxy)
                    p->sdes_reply_pending = 1;
                else
                    el_send_sdes(p);
            } else if (is_open == 0 && from && p->peer_resolved
                       && from->sin_addr.s_addr == p->peer_rtp.sin_addr.s_addr) {
                /* Configured outbound peer -- unchanged from before. */
                int just_linked = 0;

                el_apply_remote_sdes(p, cname, name);
                pthread_mutex_lock(&p->dir_mu);
                p->last_peer_rtcp = time(NULL);
                if (!p->linked) {
                    p->linked = 1;
                    p->status = PEER_EL_CONNECTED;
                    just_linked = 1;
                }
                pthread_mutex_unlock(&p->dir_mu);
                if (just_linked)
                    LOG_EL_INFO("echolink: linked to %s (RTCP SDES)\n", p->host);
                /*
                 * Under proxy, demux holds proxy.mu — must not send here
                 * (el_proxy_udp_ctrl would deadlock and freeze RTP).
                 */
                if (p->use_proxy)
                    p->sdes_reply_pending = 1;
                else
                    el_send_sdes(p);
            } else if (is_open == 0 && from) {
                el_handle_inbound_sdes(p, from, cname, name);
            }
        }
        o += plen;
    }
}

/* Identify which known source `from` is: -1 = unknown (no prior accepted
 * SDES -- RTP from it is dropped, mirroring thelinkbox's requirement of a
 * registered ConfClient before RTP_Data is processed), 0 = the configured
 * outbound peer, i+1 = inbound[i]. */
static int el_rtp_source_id(const peer_echolink_t *p, const struct sockaddr_in *from)
{
    int i;

    if (p->peer_resolved && from->sin_addr.s_addr == p->peer_rtp.sin_addr.s_addr)
        return 0;
    for (i = 0; i < EL_MAX_INBOUND; i++)
        if (p->inbound[i].used && p->inbound[i].addr.sin_addr.s_addr == from->sin_addr.s_addr)
            return i + 1;
    return -1;
}

static void el_handle_rtp(peer_echolink_t *p, const uint8_t *data, int len,
                          const struct sockaddr_in *from)
{
    gsm g = (gsm)p->gsm_dec;
    int16_t pcm[EL_GSM_SAMPLES];
    int i;
    int frames;
    int src;
    time_t now;
    static unsigned rtp_dbg;
    static time_t rtp_dbg_last;

    /* EchoLink: 4 GSM frames / packet (144 bytes). Accept exact or with pad. */
    if (len < EL_RTP_FRAME_LEN || !g) {
        static int short_dbg;
        if (++short_dbg <= 5 || (short_dbg % 50) == 0)
            LOG_EL_DEBUG("echolink: RTP drop len=%d (need >=%d) from %s\n",
                         len, EL_RTP_FRAME_LEN, p->host);
        return;
    }

    src = el_rtp_source_id(p, from);
    if (src < 0)
        return; /* not an accepted source -- no SDES handshake yet */

    now = time(NULL);
    /* Single-talker arbitration across the outbound peer + inbound[]: the
     * first source to key up owns the spurt; a different source is dropped
     * while it's still recent, so two simultaneous EchoLink sources don't
     * garble pcm_in (same principle as a per-timeslot source lock). */
    if (p->talk_src >= 0 && p->talk_src != src
        && (now - p->talk_last) < EL_TALK_HANG_SEC) {
        return;
    }
    if (p->talk_src != src) {
        /* New talker taking over (or first RTP after silence) -- update
         * everyone's "connected users" view live so the "->" arrow moves,
         * same feedback as a real conference. Not on every packet: only
         * once per talk-spurt start. */
        p->talk_src = src;
        el_broadcast_station_list(p);
    }
    p->talk_last = now;

    frames = 4;
    for (i = 0; i < frames; i++) {
        const uint8_t *frame = data + 12 + i * EL_GSM_FRAME;
        if (gsm_decode(g, (gsm_byte *)frame, (gsm_signal *)pcm) < 0)
            continue;
        pcm_ring_push(p, pcm, EL_GSM_SAMPLES);
    }
    /* New talk spurt after >=2s idle → reset DEBUG counter. */
    if (rtp_dbg_last && (now - rtp_dbg_last) >= 2)
        rtp_dbg = 0;
    p->last_rtp_rx = now;
    rtp_dbg_last = now;
    p->rtp_rx_packets++;
    if (src == 0) {
        p->last_peer_rtcp = now; /* RTP also proves the peer path is alive */
        if (!p->linked) {
            p->linked = 1;
            p->status = PEER_EL_CONNECTED;
            LOG_EL_INFO("echolink: linked to %s (first RTP)\n", p->host);
        }
    } else {
        p->inbound[src - 1].last_rtp = now;
    }
    /* First packets of a spurt + every 25th while RX is active. */
    rtp_dbg++;
    if (rtp_dbg == 1 || rtp_dbg == 2 || (rtp_dbg % 25) == 0)
        LOG_EL_DEBUG("echolink: RTP RX #%u len=%d pcm_in=%d pt=%u seq=%u from %s\n",
                     rtp_dbg, len, p->pcm_in_count,
                     (unsigned)(data[1] & 0x7f),
                     (unsigned)((data[2] << 8) | data[3]),
                     src == 0 ? p->host : p->inbound[src - 1].cname);
}

static void el_flush_rtp_tx(peer_echolink_t *p)
{
    uint8_t pkt[EL_RTP_FRAME_LEN];
    struct in_addr targets[1 + EL_MAX_INBOUND];
    int ntargets = 0;
    gsm g = (gsm)p->gsm_enc;
    int i;

    if (!g || p->pcm_out_count <= 0)
        return;

    pthread_mutex_lock(&p->dir_mu);
    if (el_rtp_ok(p))
        targets[ntargets++] = p->peer_rtp.sin_addr;
    pthread_mutex_unlock(&p->dir_mu);
    for (i = 0; i < EL_MAX_INBOUND; i++)
        if (p->inbound[i].used)
            targets[ntargets++] = p->inbound[i].addr.sin_addr;
    if (ntargets == 0)
        return; /* nobody to send to yet -- leave pcm_out_count queued */

    /* Pad short final packet with silence so VTERM audio is not dropped. */
    if (p->pcm_out_count < EL_RTP_SAMPLES)
        memset(p->pcm_out + p->pcm_out_count, 0,
               (size_t)(EL_RTP_SAMPLES - p->pcm_out_count) * sizeof(int16_t));

    memset(pkt, 0, sizeof(pkt));
    /* little-endian bitfield layout matching tlb rtp.h / ILINK_RTP_VERSION */
    pkt[0] = (uint8_t)(EL_RTP_VERSION << 6);
    pkt[1] = EL_RTP_PT_GSM;
    pkt[2] = (uint8_t)((p->rtp_seq >> 8) & 0xff);
    pkt[3] = (uint8_t)(p->rtp_seq & 0xff);
    p->rtp_seq++;
    /* tlb iLink TX uses ts=0; keep 0 for maximum client compatibility */
    pkt[4] = pkt[5] = pkt[6] = pkt[7] = 0;
    /* ssrc left 0 (EchoLink clients historically require zero) */

    for (i = 0; i < 4; i++) {
        gsm_encode(g, (gsm_signal *)(p->pcm_out + i * EL_GSM_SAMPLES),
                   (gsm_byte *)(pkt + 12 + i * EL_GSM_FRAME));
    }
    /* Encoded once, fanned out to every currently connected station (the
     * configured outbound peer, if resolved, plus every accepted inbound[]
     * slot) -- same principle as thelinkbox/SvxLink relaying one talker's
     * audio to all other conference members. */
    for (i = 0; i < ntargets; i++)
        el_send_udp_rtp(p, targets[i], pkt, EL_RTP_FRAME_LEN);
    p->pcm_out_count = 0;
    p->rtp_tx_packets++;
}

int peer_el_open(peer_echolink_t *p, const adn_bridge_peer_el_t *cfg)
{
    int i;

    memset(p, 0, sizeof(*p));
    p->rtp_sock = -1;
    p->rtcp_sock = -1;
    p->dir_fd = -1;
    el_proxy_init(&p->proxy);
    pthread_mutex_init(&p->dir_mu, NULL);
    pthread_cond_init(&p->dir_cv, NULL);
    if (!cfg || !cfg->callsign[0]) {
        pthread_cond_destroy(&p->dir_cv);
        pthread_mutex_destroy(&p->dir_mu);
        el_proxy_clear(&p->proxy);
        return -1;
    }
    p->use_proxy = cfg->proxy_server[0] ? 1 : 0;
    if (!p->use_proxy && !cfg->bind_addr[0]) {
        pthread_cond_destroy(&p->dir_cv);
        pthread_mutex_destroy(&p->dir_mu);
        el_proxy_clear(&p->proxy);
        return -1;
    }

    copy_z(p->callsign, sizeof(p->callsign), cfg->callsign);
    copy_z(p->password, sizeof(p->password), cfg->password);
    copy_z(p->bind_addr, sizeof(p->bind_addr), cfg->bind_addr);
    copy_z(p->host, sizeof(p->host), cfg->host);
    copy_z(p->qth, sizeof(p->qth), cfg->qth);
    copy_z(p->email, sizeof(p->email), cfg->email);
    /* Until inbound SDES arrives, treat connected node as remote identity. */
    if (p->host[0])
        copy_z(p->remote_talker, sizeof(p->remote_talker), p->host);
    p->talk_src = -1;
    p->max_inbound = cfg->max_inbound;
    if (p->max_inbound > EL_MAX_INBOUND)
        p->max_inbound = EL_MAX_INBOUND;
    p->allowed_callsign_count = cfg->allowed_callsign_count;
    for (i = 0; i < cfg->allowed_callsign_count && i < ADN_BRIDGE_EL_ALLOW_MAX; i++)
        copy_z(p->allowed_callsigns[i], sizeof(p->allowed_callsigns[i]),
               cfg->allowed_callsigns[i]);
    p->blocked_callsign_count = cfg->blocked_callsign_count;
    for (i = 0; i < cfg->blocked_callsign_count && i < ADN_BRIDGE_EL_ALLOW_MAX; i++)
        copy_z(p->blocked_callsigns[i], sizeof(p->blocked_callsigns[i]),
               cfg->blocked_callsigns[i]);
    copy_z(p->welcome_text, sizeof(p->welcome_text), cfg->welcome_text);
    p->directory_server_count = cfg->directory_server_count;
    p->login_interval = cfg->login_interval;
    p->station_list_interval = cfg->station_list_interval;
    for (i = 0; i < cfg->directory_server_count && i < ADN_BRIDGE_EL_DIR_MAX; i++)
        copy_z(p->directory_servers[i], sizeof(p->directory_servers[i]),
               cfg->directory_servers[i]);

    p->gsm_enc = gsm_create();
    p->gsm_dec = gsm_create();
    if (!p->gsm_enc || !p->gsm_dec) {
        LOG_EL_ERROR("echolink: gsm_create failed\n");
        peer_el_close(p);
        return -1;
    }

    if (p->use_proxy) {
        if (el_proxy_open(&p->proxy, cfg->proxy_server, cfg->proxy_port,
                          p->callsign, cfg->proxy_password) != 0) {
            LOG_EL_ERROR("echolink: proxy open failed (%s:%d)\n",
                         cfg->proxy_server, cfg->proxy_port > 0
                             ? cfg->proxy_port : EL_PROXY_DEFAULT_PORT);
            peer_el_close(p);
            return -1;
        }
        LOG_EL_INFO("echolink: using proxy %s:%d as %s (no local UDP bind)\n",
                    cfg->proxy_server,
                    cfg->proxy_port > 0 ? cfg->proxy_port : EL_PROXY_DEFAULT_PORT,
                    p->callsign);
    } else {
        p->rtp_sock = udp_bind(p->bind_addr, EL_RTP_PORT, &p->bind_rtp);
        if (p->rtp_sock < 0) {
            LOG_EL_ERROR("echolink: cannot bind %s:%d: %s\n",
                      p->bind_addr, EL_RTP_PORT, strerror(errno));
            peer_el_close(p);
            return -1;
        }
        p->rtcp_sock = udp_bind(p->bind_addr, EL_RTCP_PORT, &p->bind_rtcp);
        if (p->rtcp_sock < 0) {
            LOG_EL_ERROR("echolink: cannot bind %s:%d: %s\n",
                      p->bind_addr, EL_RTCP_PORT, strerror(errno));
            peer_el_close(p);
            return -1;
        }
        LOG_EL_INFO("echolink: bound %s:%d/%d as %s\n",
                 p->bind_addr, EL_RTP_PORT, EL_RTCP_PORT, p->callsign);
    }

    /*
     * tlb startup (conference.c): LOGIN_AND_LIST when StationListInterval > 0,
     * NextLoginTime = now+60, NextStationListTime = now+StationListInterval.
     */
    {
        time_t now = time(NULL);

        if (p->login_interval > 0) {
            p->next_login_time = now + EL_FIRST_RELOGIN_DELAY;
            if (p->station_list_interval > 0) {
                p->next_station_list_time = now + p->station_list_interval;
                LOG_EL_INFO("echolink: directory timers login=%ds list=%ds (tlb)\n",
                         p->login_interval, p->station_list_interval);
                el_login_and_list(p);
            } else {
                p->next_station_list_time = 0;
                el_try_directory_login(p);
            }
        } else {
            p->next_login_time = 0;
            p->next_station_list_time = 0;
            if (p->host[0])
                el_station_list_refresh_peer(p);
        }
    }

    /* Periodic directory TCP must not block RTP/PCM after startup. */
    el_dir_thread_start(p);
    return 0;
}

void peer_el_close(peer_echolink_t *p)
{
    el_dir_thread_stop(p);
    if (p->use_proxy)
        el_proxy_close(&p->proxy);
    if (p->rtp_sock >= 0)
        close(p->rtp_sock);
    if (p->rtcp_sock >= 0)
        close(p->rtcp_sock);
    if (p->gsm_enc)
        gsm_destroy((gsm)p->gsm_enc);
    if (p->gsm_dec)
        gsm_destroy((gsm)p->gsm_dec);
    p->rtp_sock = p->rtcp_sock = -1;
    p->gsm_enc = p->gsm_dec = NULL;
    p->linked = 0;
    p->peer_resolved = 0;
    p->status = PEER_EL_DISCONNECTED;
    pthread_cond_destroy(&p->dir_cv);
    pthread_mutex_destroy(&p->dir_mu);
    el_proxy_clear(&p->proxy);
    p->use_proxy = 0;
}

void peer_el_tick(peer_echolink_t *p)
{
    if (p->use_proxy)
        el_proxy_ensure(&p->proxy);
    time_t now = time(NULL);

    /* tlb RTCP_Handler directory refresh — schedule worker (non-blocking). */
    if (p->login_interval > 0 && p->next_login_time > 0
        && now >= p->next_login_time) {
        int job = EL_DIR_JOB_LOGIN;

        p->next_login_time = now + p->login_interval;
        if (p->next_station_list_time != 0
            && now >= p->next_station_list_time) {
            p->next_station_list_time = now + p->station_list_interval;
            job = EL_DIR_JOB_LOGIN_LIST;
            LOG_EL_DEBUG("echolink: refreshing login and station list\n");
        } else {
            LOG_EL_DEBUG("echolink: refreshing login\n");
        }
        if (p->dir_thread_on) {
            if (el_dir_schedule(p, job) != 0)
                LOG_EL_DEBUG("echolink: directory job busy — skip this cycle\n");
        } else if (job == EL_DIR_JOB_LOGIN_LIST) {
            el_login_and_list(p);
        } else {
            el_dir_op_begin(p);
            el_try_directory_login(p);
            el_dir_op_end(p);
        }
    } else if (p->login_interval > 0 && p->next_station_list_time > 0
               && now >= p->next_station_list_time) {
        p->next_station_list_time = now + p->station_list_interval;
        LOG_EL_DEBUG("echolink: refreshing station list\n");
        if (p->dir_thread_on) {
            if (el_dir_schedule(p, EL_DIR_JOB_LIST) != 0)
                LOG_EL_DEBUG("echolink: directory job busy — skip this cycle\n");
        } else {
            el_station_list_only(p);
        }
    }

    if (p->peer_resolved
        && (p->last_sdes == 0 || now - p->last_sdes >= EL_SDES_INTERVAL)) {
        /* While unlinked, keep OPEN+SDES (conferences often need the OPEN). */
        if (!p->linked)
            el_send_connect_handshake(p);
        else
            el_send_sdes(p);
    }

    /* Conference dropped us (ConfMemberTimeout ~40s) or path died — recover. */
    if (p->linked && p->last_peer_rtcp > 0
        && now - p->last_peer_rtcp >= EL_PEER_STALE_SEC) {
        LOG_EL_WARNING("echolink: peer %s silent %lds — unlinked, re-SDES\n",
                       p->host, (long)(now - p->last_peer_rtcp));
        pthread_mutex_lock(&p->dir_mu);
        p->linked = 0;
        p->status = PEER_EL_DIR_OK;
        p->remote_talker_explicit = 0;
        if (p->host[0])
            copy_z(p->remote_talker, sizeof(p->remote_talker), p->host);
        pthread_mutex_unlock(&p->dir_mu);
        el_send_connect_handshake(p);
    }

    /* Inbound EchoLink connections: same stale criterion as the configured
     * outbound peer above, applied per slot. */
    {
        int i;
        int any_removed = 0;

        for (i = 0; i < EL_MAX_INBOUND; i++) {
            if (!p->inbound[i].used)
                continue;
            if (now - p->inbound[i].last_rtcp >= EL_PEER_STALE_SEC
                && now - p->inbound[i].last_rtp >= EL_PEER_STALE_SEC) {
                LOG_EL_INFO("echolink: inbound %s silent %lds — disconnected\n",
                           p->inbound[i].cname,
                           (long)(now - p->inbound[i].last_rtcp));
                if (p->talk_src == i + 1)
                    p->talk_src = -1;
                memset(&p->inbound[i], 0, sizeof(p->inbound[i]));
                any_removed = 1;
            }
        }
        /* Update everyone still connected's "connected users" view. */
        if (any_removed && !p->use_proxy)
            el_broadcast_station_list(p);
    }

    /* Current talker gone silent -> clear the "->" arrow and update everyone
     * still connected (el_handle_rtp only ever runs while someone IS
     * transmitting, so this tick-driven check is what detects release). */
    if (p->talk_src >= 0 && now - p->talk_last >= EL_TALK_SILENCE_SEC) {
        p->talk_src = -1;
        if (!p->use_proxy)
            el_broadcast_station_list(p);
    }
}

int peer_el_poll(peer_echolink_t *p, int timeout_ms)
{
    struct pollfd pf[2];
    int n;
    int drained = 0;

    if (p->use_proxy) {
        struct pollfd ppf;

        el_proxy_ensure(&p->proxy);
        if (!el_proxy_ready(&p->proxy)) {
            if (timeout_ms > 0)
                usleep((useconds_t)timeout_ms * 1000U);
            return p->pcm_in_count;
        }
        ppf.fd = p->proxy.fd;
        ppf.events = POLLIN;
        if (timeout_ms >= 0)
            poll(&ppf, 1, timeout_ms);
        el_proxy_poll(&p->proxy, el_proxy_udp_cb, p);
        /* SDES replies deferred from RTCP handler (avoids proxy.mu deadlock). */
        if (p->sdes_reply_pending) {
            p->sdes_reply_pending = 0;
            el_send_sdes(p);
        }
        return p->pcm_in_count;
    }

    if (p->rtp_sock < 0)
        return 0;
    pf[0].fd = p->rtp_sock;
    pf[0].events = POLLIN;
    pf[1].fd = p->rtcp_sock;
    pf[1].events = POLLIN;
    if (poll(pf, 2, timeout_ms) <= 0)
        return 0;

    /* Drain socket queues — EchoLink sends ~12.5 RTP/s; one recv/loop drops audio. */
    while (1) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);

        n = (int)recvfrom(p->rtp_sock, p->rx_buf, sizeof(p->rx_buf), MSG_DONTWAIT,
                          (struct sockaddr *)&from, &flen);
        if (n <= 0)
            break;
        if (p->rx_buf[0] != 0x6f) {
            el_handle_rtp(p, p->rx_buf, n, &from);
            drained = 1;
        }
    }
    while (1) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);

        n = (int)recvfrom(p->rtcp_sock, p->rx_buf, sizeof(p->rx_buf), MSG_DONTWAIT,
                          (struct sockaddr *)&from, &flen);
        if (n <= 0)
            break;
        el_handle_rtcp(p, p->rx_buf, n, &from);
    }
    (void)drained;
    return p->pcm_in_count;
}

int peer_el_read_pcm(peer_echolink_t *p, int16_t *pcm, int max_samples)
{
    int n = 0;
    int cap = (int)(sizeof(p->pcm_in) / sizeof(p->pcm_in[0]));

    while (n < max_samples && p->pcm_in_count > 0) {
        pcm[n++] = p->pcm_in[p->pcm_in_r];
        p->pcm_in_r = (p->pcm_in_r + 1) % cap;
        p->pcm_in_count--;
    }
    return n;
}

int peer_el_write_pcm(peer_echolink_t *p, const int16_t *pcm, int samples)
{
    int i = 0;

    while (i < samples) {
        int space = EL_RTP_SAMPLES - p->pcm_out_count;
        int take = samples - i;
        if (take > space)
            take = space;
        memcpy(p->pcm_out + p->pcm_out_count, pcm + i, (size_t)take * sizeof(int16_t));
        p->pcm_out_count += take;
        i += take;
        if (p->pcm_out_count >= EL_RTP_SAMPLES)
            el_flush_rtp_tx(p);
    }
    return samples;
}

void peer_el_flush_pcm(peer_echolink_t *p)
{
    if (p && p->pcm_out_count > 0)
        el_flush_rtp_tx(p);
}

void peer_el_drop_pcm_in(peer_echolink_t *p)
{
    if (!p)
        return;
    p->pcm_in_r = 0;
    p->pcm_in_w = 0;
    p->pcm_in_count = 0;
}

void peer_el_set_talker_name(peer_echolink_t *p, const char *name)
{
    char buf[sizeof(p->talker_name)];
    size_t i, n;

    if (!p)
        return;
    buf[0] = '\0';
    if (name && name[0]) {
        /* Trim spaces; keep printable ASCII for SDES NAME. */
        while (*name == ' ' || *name == '\t')
            name++;
        n = 0;
        for (i = 0; name[i] && n + 1 < sizeof(buf); i++) {
            unsigned char c = (unsigned char)name[i];
            if (c < 32 || c > 126)
                continue;
            if (c == ' ' && (n == 0 || buf[n - 1] == ' '))
                continue;
            buf[n++] = (char)c;
        }
        while (n > 0 && buf[n - 1] == ' ')
            n--;
        buf[n] = '\0';
    }
    if (strcmp(p->talker_name, buf) == 0)
        return;
    copy_z(p->talker_name, sizeof(p->talker_name), buf);
    if (buf[0])
        LOG_EL_INFO("echolink: talker NAME=%s (CNAME=%s)\n",
                    p->talker_name, p->callsign);
    else
        LOG_EL_INFO("echolink: talker NAME cleared (CNAME=%s)\n", p->callsign);
    el_send_sdes(p);
}

void peer_el_set_relay_label(peer_echolink_t *p, const char *label)
{
    char buf[sizeof(p->relay_label)];

    if (!p)
        return;
    buf[0] = '\0';
    if (label && label[0])
        copy_z(buf, sizeof(buf), label);
    if (strcmp(p->relay_label, buf) == 0)
        return;
    copy_z(p->relay_label, sizeof(p->relay_label), buf);
    if (!p->use_proxy)
        el_broadcast_station_list(p);
}

const char *peer_el_remote_talker(const peer_echolink_t *p)
{
    if (!p)
        return "";
    /* talk_src tracks whichever EL source (outbound peer or an inbound
     * app/conference connection) is currently the arbitrated talker --
     * remote_talker/remote_cname below only ever reflect the outbound
     * peer's own SDES, so an inbound talker must be checked first or
     * DMR/YSF loses the caller's identity while an inbound station is
     * the one actually talking. */
    if (p->talk_src > 0) {
        int i = p->talk_src - 1;

        if (i >= 0 && i < EL_MAX_INBOUND && p->inbound[i].used && p->inbound[i].cname[0])
            return p->inbound[i].cname;
    }
    if (p->remote_talker[0])
        return p->remote_talker;
    if (p->remote_cname[0])
        return p->remote_cname;
    if (p->host[0])
        return p->host;
    return "";
}

void peer_el_clear_remote_talker(peer_echolink_t *p)
{
    if (!p)
        return;
    p->remote_talker_explicit = 0;
    if (p->host[0])
        copy_z(p->remote_talker, sizeof(p->remote_talker), p->host);
    else
        p->remote_talker[0] = '\0';
}

void peer_el_on_sigint(peer_echolink_t *p)
{
    peer_el_close(p);
}
