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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "gsm.h"

/* Must stay under tlb ConfMemberTimeout (default 40s) so SDES keepalive survives. */
#define EL_DIR_LOOKUP_TIMEOUT_SEC 20

static void el_send_sdes(peer_echolink_t *p);
static int el_try_directory_login(peer_echolink_t *p);
static void el_flush_rtp_tx(peer_echolink_t *p);
static void el_login_and_list(peer_echolink_t *p);
static void el_station_list_only(peer_echolink_t *p);
static void el_dir_op_begin(peer_echolink_t *p);
static void el_dir_op_end(peer_echolink_t *p);

#define EL_SDES_INTERVAL      5
/* Peer silent longer than tlb ConfMemberTimeout → treat as unlinked. */
#define EL_PEER_STALE_SEC     45
/* tlb: first re-login is delayed 60s after startup LOGIN_AND_LIST */
#define EL_FIRST_RELOGIN_DELAY 60
#define EL_RTP_VERSION        3
#define EL_RTP_PT_GSM         3
#define EL_RTCP_RR            201
#define EL_RTCP_SDES          202
#define EL_SDES_CNAME         1
#define EL_SDES_NAME          2
#define EL_SDES_EMAIL         3
#define EL_SDES_PHONE         4
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
    if (inet_aton(p->bind_addr, &local.sin_addr) == 0
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

/*
 * Look up EchoLink node/conference callsign in the directory station list.
 * host may be CA5RPY-L or *REDCHILE*. Returns 0 and fills *out on success.
 */
static int el_directory_lookup_on_server(peer_echolink_t *p, const char *server,
                                         const char *want, int scrambled,
                                         struct in_addr *out)
{
    char line[256];
    char call[32], ipstr[96], iptry[96];
    int fd = -1;
    int n, state, found = 0, k;
    size_t linelen = 0;
    uint8_t buf[4096];
    const char *req = scrambled ? "S" : "s";

    if (el_tcp_bind_connect(p, server, &fd) != 0)
        return -1;
    if (write(fd, req, 1) != 1) {
        close(fd);
        return -1;
    }

    state = 0; /* 0=@@@, 1=count, 2=call, 3=qth, 4=nodeid, 5=ip */
    while (!found) {
        n = (int)read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (k = 0; k < n; k++) {
            if (buf[k] == '\n') {
                line[linelen] = '\0';
                trim_cr(line);
                if (state == 0) {
                    if (strncmp(line, "@@@", 3) == 0)
                        state = 1;
                } else if (state == 1) {
                    state = 2;
                } else if (strcmp(line, "+++") == 0) {
                    close(fd);
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
                        strncpy(iptry, ipstr, sizeof(iptry) - 1);
                        iptry[sizeof(iptry) - 1] = '\0';
                        if (scrambled)
                            el_descramble_ip(iptry);
                        if (inet_aton(iptry, out)) {
                            LOG_EL_INFO("echolink: %s -> %s via %s\n",
                                     want, iptry, server);
                            found = 1;
                        } else {
                            LOG_EL_WARNING("echolink: bad IP for %s (raw=%s)\n",
                                        want, ipstr);
                        }
                    }
                    state = 2;
                }
                linelen = 0;
                if (found)
                    break;
            } else if (linelen + 1 < sizeof(line)) {
                line[linelen++] = (char)buf[k];
            }
        }
    }
    close(fd);
    return found ? 0 : -1;
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
    el_send_sdes(p);
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
    if (!p->dir_thread_on)
        return;
    pthread_mutex_lock(&p->dir_mu);
    p->dir_stop = 1;
    pthread_cond_signal(&p->dir_cv);
    pthread_mutex_unlock(&p->dir_mu);
    pthread_join(p->dir_tid, NULL);
    p->dir_thread_on = 0;
    p->dir_busy = 0;
    p->dir_job = EL_DIR_JOB_NONE;
}

/* Directory TCP login — returns 0 on OK. */
static int el_directory_login(peer_echolink_t *p, const char *server)
{
    int fd = -1;
    struct sockaddr_in sa, local;
    struct in_addr ip;
    char body[512];
    char ack[8];
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm;
    int n, body_len;
    uint8_t l = 'l';
    int i;

    if (resolve_dns(server, &ip) != 0) {
        LOG_EL_WARNING("echolink: cannot resolve directory %s\n", server);
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = 0;
    if (inet_aton(p->bind_addr, &local.sin_addr) == 0
        || bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        LOG_EL_WARNING("echolink: directory bind %s failed: %s\n",
                    p->bind_addr, strerror(errno));
        close(fd);
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(EL_DIR_PORT);
    sa.sin_addr = ip;

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG_EL_WARNING("echolink: directory connect %s: %s\n", server, strerror(errno));
        close(fd);
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
        close(fd);
        return -1;
    }

    if (write(fd, &l, 1) != 1
        || write(fd, body, (size_t)body_len) != body_len) {
        close(fd);
        return -1;
    }

    n = (int)read(fd, ack, sizeof(ack) - 1);
    close(fd);
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
    (void)i;
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

/* Build minimal RTCP RR + SDES (EchoLink version 3). */
static int el_build_sdes(peer_echolink_t *p, uint8_t *out, int outlen)
{
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    char phone[16];
    char tool[] = "ysf2dmrcon";
    int o = 0;
    int name_len, cname_len, email_len, phone_len, tool_len;
    int sdes_start, sdes_len_words;
    int need;

    if (outlen < 128)
        return -1;

    snprintf(phone, sizeof(phone), "%02d:%02d",
             tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);

    cname_len = (int)strlen(p->callsign);
    /* NAME carries remote talker when set (EchoLink UI / conference display). */
    name_len = p->talker_name[0] ? (int)strlen(p->talker_name) : cname_len;
    email_len = p->email[0] ? (int)strlen(p->email) : cname_len;
    phone_len = (int)strlen(phone);
    tool_len = (int)strlen(tool);
    /* RR(8) + SDES hdr(8) + items + END + pad(<=3) */
    need = 8 + 8 + (2 + cname_len) + (2 + name_len) + (2 + email_len)
           + (2 + phone_len) + (2 + tool_len) + 1 + 3;
    if (need > outlen)
        return -1;

    /* RR */
    out[o++] = (uint8_t)((EL_RTP_VERSION << 6)); /* v=3, p=0, count=0 */
    out[o++] = EL_RTCP_RR;
    out[o++] = 0;
    out[o++] = 1; /* length = 1 word after common header */
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 0; /* ssrc */

    sdes_start = o;
    out[o++] = (uint8_t)((EL_RTP_VERSION << 6) | 0x20 | 1); /* v=3,p=1,count=1 */
    out[o++] = EL_RTCP_SDES;
    out[o++] = 0;
    out[o++] = 0; /* length filled later */
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 0; /* ssrc */

    out[o++] = EL_SDES_CNAME;
    out[o++] = (uint8_t)cname_len;
    memcpy(out + o, p->callsign, (size_t)cname_len);
    o += cname_len;

    out[o++] = EL_SDES_NAME;
    out[o++] = (uint8_t)name_len;
    memcpy(out + o, p->talker_name[0] ? p->talker_name : p->callsign,
           (size_t)name_len);
    o += name_len;

    out[o++] = EL_SDES_EMAIL;
    out[o++] = (uint8_t)email_len;
    memcpy(out + o, p->email[0] ? p->email : p->callsign, (size_t)email_len);
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
    while ((o & 3) != 0)
        out[o++] = 0;

    sdes_len_words = ((o - sdes_start) / 4) - 1;
    out[sdes_start + 2] = (uint8_t)((sdes_len_words >> 8) & 0xff);
    out[sdes_start + 3] = (uint8_t)(sdes_len_words & 0xff);
    return o;
}

static void el_send_sdes(peer_echolink_t *p)
{
    uint8_t buf[256];
    struct sockaddr_in dest;
    int n;
    int sock;

    pthread_mutex_lock(&p->dir_mu);
    if (!p->peer_resolved || p->rtcp_sock < 0) {
        pthread_mutex_unlock(&p->dir_mu);
        return;
    }
    sock = p->rtcp_sock;
    dest = p->peer_rtcp;
    n = el_build_sdes(p, buf, (int)sizeof(buf));
    if (n > 0)
        p->last_sdes = time(NULL);
    pthread_mutex_unlock(&p->dir_mu);
    if (n <= 0)
        return;
    sendto(sock, buf, (size_t)n, 0, (struct sockaddr *)&dest, sizeof(dest));
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

/*
 * Derive remote talker from inbound SDES:
 *   NAME "NODE (CE5ABC) CONF" → CE5ABC (only if paren text looks like callsign)
 *   NAME "CA5RPY-L (Conference [2/8]) CONF" → CA5RPY-L (paren is status, skip)
 *   NAME "CALLSIGN Name" → first token
 *   else CNAME / connected host
 */
static void el_apply_remote_sdes(peer_echolink_t *p, const char *cname, const char *name)
{
    char talker[sizeof(p->remote_talker)];
    char cand[sizeof(p->remote_talker)];
    const char *lp, *rp;
    size_t i, n;

    talker[0] = '\0';
    if (name && name[0]) {
        lp = strrchr(name, '(');
        rp = strrchr(name, ')');
        if (lp && rp && rp > lp + 1) {
            n = 0;
            for (i = 1; lp[i] && &lp[i] < rp && n + 1 < sizeof(cand); i++) {
                unsigned char c = (unsigned char)lp[i];

                if (c == ' ' && n == 0)
                    continue;
                if (c < 32 || c > 126)
                    continue;
                cand[n++] = (char)toupper(c);
            }
            while (n > 0 && cand[n - 1] == ' ')
                n--;
            cand[n] = '\0';
            if (el_looks_like_callsign(cand))
                copy_z(talker, sizeof(talker), cand);
        }
        if (!talker[0]) {
            el_copy_token(cand, sizeof(cand), name);
            if (el_looks_like_callsign(cand))
                copy_z(talker, sizeof(talker), cand);
        }
    }
    if (!talker[0] && cname && cname[0] && el_looks_like_callsign(cname))
        el_copy_token(talker, sizeof(talker), cname);
    if (!talker[0] && p->host[0])
        copy_z(talker, sizeof(talker), p->host);

    if (cname && cname[0] && el_looks_like_callsign(cname))
        el_copy_token(p->remote_cname, sizeof(p->remote_cname), cname);
    if (!talker[0])
        return;
    if (strcmp(p->remote_talker, talker) == 0)
        return;
    copy_z(p->remote_talker, sizeof(p->remote_talker), talker);
    LOG_EL_INFO("echolink: remote talker=%s (cname=%s name=%s)\n",
                p->remote_talker,
                p->remote_cname[0] ? p->remote_cname : "-",
                (name && name[0]) ? name : "-");
}

static void el_parse_sdes_packet(peer_echolink_t *p, const uint8_t *pkt, int plen)
{
    char cname[64];
    char name[64];
    int o;
    int count;

    if (plen < 8)
        return;
    cname[0] = '\0';
    name[0] = '\0';
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
                return;
            if (type == EL_SDES_CNAME)
                el_sdes_copy_item(cname, sizeof(cname), pkt + o + 2, ilen);
            else if (type == EL_SDES_NAME)
                el_sdes_copy_item(name, sizeof(name), pkt + o + 2, ilen);
            o += 2 + ilen;
        }
    }
    el_apply_remote_sdes(p, cname, name);
}

static void el_handle_rtcp(peer_echolink_t *p, const uint8_t *data, int len,
                           const struct sockaddr_in *from)
{
    int o = 0;

    (void)from;
    while (o + 4 <= len) {
        uint8_t pt = data[o + 1];
        int words = (data[o + 2] << 8) | data[o + 3];
        int plen = (words + 1) * 4;

        if (plen < 4 || o + plen > len)
            break;
        if (pt == EL_RTCP_SDES) {
            int just_linked = 0;

            el_parse_sdes_packet(p, data + o, plen);
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
            /* Reply SDES */
            el_send_sdes(p);
        }
        o += plen;
    }
}

static void el_handle_rtp(peer_echolink_t *p, const uint8_t *data, int len)
{
    gsm g = (gsm)p->gsm_dec;
    int16_t pcm[EL_GSM_SAMPLES];
    int i;
    int frames;
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
    frames = 4;
    for (i = 0; i < frames; i++) {
        const uint8_t *frame = data + 12 + i * EL_GSM_FRAME;
        if (gsm_decode(g, (gsm_byte *)frame, (gsm_signal *)pcm) < 0)
            continue;
        pcm_ring_push(p, pcm, EL_GSM_SAMPLES);
    }
    now = time(NULL);
    /* New talk spurt after >=2s idle → reset DEBUG counter. */
    if (rtp_dbg_last && (now - rtp_dbg_last) >= 2)
        rtp_dbg = 0;
    p->last_rtp_rx = now;
    p->last_peer_rtcp = now; /* RTP also proves the peer path is alive */
    rtp_dbg_last = now;
    p->rtp_rx_packets++;
    if (!p->linked) {
        p->linked = 1;
        p->status = PEER_EL_CONNECTED;
        LOG_EL_INFO("echolink: linked to %s (first RTP)\n", p->host);
    }
    /* First packets of a spurt + every 25th while RX is active. */
    rtp_dbg++;
    if (rtp_dbg == 1 || rtp_dbg == 2 || (rtp_dbg % 25) == 0)
        LOG_EL_DEBUG("echolink: RTP RX #%u len=%d pcm_in=%d pt=%u seq=%u from %s\n",
                     rtp_dbg, len, p->pcm_in_count,
                     (unsigned)(data[1] & 0x7f),
                     (unsigned)((data[2] << 8) | data[3]),
                     p->host);
}

static void el_flush_rtp_tx(peer_echolink_t *p)
{
    uint8_t pkt[EL_RTP_FRAME_LEN];
    struct sockaddr_in dest;
    gsm g = (gsm)p->gsm_enc;
    int i;
    int sock;

    pthread_mutex_lock(&p->dir_mu);
    if (!g || p->pcm_out_count <= 0 || !p->peer_resolved || p->rtp_sock < 0) {
        pthread_mutex_unlock(&p->dir_mu);
        return;
    }
    sock = p->rtp_sock;
    dest = p->peer_rtp;
    pthread_mutex_unlock(&p->dir_mu);

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
    sendto(sock, pkt, EL_RTP_FRAME_LEN, 0,
           (struct sockaddr *)&dest, sizeof(dest));
    p->pcm_out_count = 0;
    p->rtp_tx_packets++;
}

int peer_el_open(peer_echolink_t *p, const ysf2dmr_echolink_cfg_t *cfg)
{
    int i;

    memset(p, 0, sizeof(*p));
    p->rtp_sock = -1;
    p->rtcp_sock = -1;
    pthread_mutex_init(&p->dir_mu, NULL);
    pthread_cond_init(&p->dir_cv, NULL);
    if (!cfg || !cfg->callsign[0] || !cfg->bind_addr[0]) {
        pthread_cond_destroy(&p->dir_cv);
        pthread_mutex_destroy(&p->dir_mu);
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
    p->directory_server_count = cfg->directory_server_count;
    p->login_interval = cfg->login_interval;
    p->station_list_interval = cfg->station_list_interval;
    for (i = 0; i < cfg->directory_server_count && i < YSF2DMR_EL_DIR_MAX; i++)
        copy_z(p->directory_servers[i], sizeof(p->directory_servers[i]),
               cfg->directory_servers[i]);

    p->gsm_enc = gsm_create();
    p->gsm_dec = gsm_create();
    if (!p->gsm_enc || !p->gsm_dec) {
        LOG_EL_ERROR("echolink: gsm_create failed\n");
        peer_el_close(p);
        return -1;
    }

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
}

void peer_el_tick(peer_echolink_t *p)
{
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
        && (p->last_sdes == 0 || now - p->last_sdes >= EL_SDES_INTERVAL))
        el_send_sdes(p);

    /* Conference dropped us (ConfMemberTimeout ~40s) or path died — recover. */
    if (p->linked && p->last_peer_rtcp > 0
        && now - p->last_peer_rtcp >= EL_PEER_STALE_SEC) {
        LOG_EL_WARNING("echolink: peer %s silent %lds — unlinked, re-SDES\n",
                       p->host, (long)(now - p->last_peer_rtcp));
        pthread_mutex_lock(&p->dir_mu);
        p->linked = 0;
        p->status = PEER_EL_DIR_OK;
        pthread_mutex_unlock(&p->dir_mu);
        el_send_sdes(p);
    }
}

int peer_el_linked(const peer_echolink_t *p)
{
    return p && p->linked;
}

int peer_el_poll(peer_echolink_t *p, int timeout_ms)
{
    struct pollfd pf[2];
    int n;
    int drained = 0;

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
            el_handle_rtp(p, p->rx_buf, n);
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

const char *peer_el_remote_talker(const peer_echolink_t *p)
{
    if (!p)
        return "";
    if (p->remote_talker[0])
        return p->remote_talker;
    if (p->remote_cname[0])
        return p->remote_cname;
    if (p->host[0])
        return p->host;
    return "";
}

void peer_el_on_sigint(peer_echolink_t *p)
{
    peer_el_close(p);
}
