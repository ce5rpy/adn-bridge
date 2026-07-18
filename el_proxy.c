/*
 * EchoLink Proxy client.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wire: TCP :8100, 8-byte nonce + MD5(password||nonce), then mux of
 * directory TCP and UDP 5198/5199 inside typed message blocks.
 */

#include "el_proxy.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "log.h"

enum {
    MSG_TCP_OPEN = 1,
    MSG_TCP_DATA = 2,
    MSG_TCP_CLOSE = 3,
    MSG_TCP_STATUS = 4,
    MSG_UDP_DATA = 5,
    MSG_UDP_CONTROL = 6,
    MSG_SYSTEM = 7
};

enum {
    SYS_BAD_PASSWORD = 1,
    SYS_ACCESS_DENIED = 2
};

enum {
    TCP_DISC = 0,
    TCP_CONNECTING = 1,
    TCP_CONNECTED = 2,
    TCP_DISCONNECTING = 3
};

#define NONCE_SIZE 8
#define MSG_HDR 9
#define RECONNECT_SEC 10
/* Must hold a full compressed station-list reply (same order as peer_echolink). */
#define EL_PROXY_DIR_RX_CAP (1024 * 1024 + 16)

static int resolve_host(const char *host, struct in_addr *out)
{
    struct addrinfo hints, *res = NULL;
    int rc;

    if (!host || !out)
        return -1;
    if (inet_aton(host, out))
        return 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res)
        return -1;
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

static void upper_password(char *dst, size_t dstlen, const char *src)
{
    size_t i, n = 0;

    if (!dst || dstlen == 0)
        return;
    if (!src || !src[0])
        src = "PUBLIC";
    for (i = 0; src[i] && n + 1 < dstlen; i++)
        dst[n++] = (char)toupper((unsigned char)src[i]);
    dst[n] = '\0';
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void el_proxy_init(el_proxy_t *px)
{
    if (!px)
        return;
    memset(px, 0, sizeof(*px));
    px->fd = -1;
    pthread_mutex_init(&px->mu, NULL);
    pthread_cond_init(&px->dir_cv, NULL);
}

void el_proxy_clear(el_proxy_t *px)
{
    if (!px)
        return;
    el_proxy_close(px);
    free(px->dir_rx);
    px->dir_rx = NULL;
    px->dir_rx_cap = 0;
    pthread_cond_destroy(&px->dir_cv);
    pthread_mutex_destroy(&px->mu);
    memset(px, 0, sizeof(*px));
    px->fd = -1;
}

static int ensure_dir_rx_locked(el_proxy_t *px)
{
    if (px->dir_rx && px->dir_rx_cap >= EL_PROXY_DIR_RX_CAP)
        return 0;
    free(px->dir_rx);
    px->dir_rx = (uint8_t *)malloc(EL_PROXY_DIR_RX_CAP);
    if (!px->dir_rx) {
        px->dir_rx_cap = 0;
        return -1;
    }
    px->dir_rx_cap = EL_PROXY_DIR_RX_CAP;
    px->dir_rx_len = 0;
    px->dir_rx_r = 0;
    return 0;
}

static int send_raw_locked(el_proxy_t *px, const void *data, int len)
{
    const uint8_t *p = (const uint8_t *)data;
    int off = 0;

    while (off < len) {
        int n = (int)write(px->fd, p + off, (size_t)(len - off));

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pf;

                pf.fd = px->fd;
                pf.events = POLLOUT;
                if (poll(&pf, 1, 5000) <= 0)
                    return -1;
                continue;
            }
            return -1;
        }
        if (n == 0)
            return -1;
        off += n;
    }
    return 0;
}

static int send_msg_locked(el_proxy_t *px, int type, struct in_addr remote,
                           const void *data, int len)
{
    uint8_t hdr[MSG_HDR];
    uint32_t ip = remote.s_addr;
    uint32_t ulen = (uint32_t)(len < 0 ? 0 : len);

    if (px->fd < 0 || !px->ready)
        return -1;
    hdr[0] = (uint8_t)type;
    hdr[1] = (uint8_t)(ip & 0xff);
    hdr[2] = (uint8_t)((ip >> 8) & 0xff);
    hdr[3] = (uint8_t)((ip >> 16) & 0xff);
    hdr[4] = (uint8_t)((ip >> 24) & 0xff);
    hdr[5] = (uint8_t)(ulen & 0xff);
    hdr[6] = (uint8_t)((ulen >> 8) & 0xff);
    hdr[7] = (uint8_t)((ulen >> 16) & 0xff);
    hdr[8] = (uint8_t)((ulen >> 24) & 0xff);
    if (send_raw_locked(px, hdr, MSG_HDR) != 0)
        return -1;
    if (ulen > 0 && data && send_raw_locked(px, data, (int)ulen) != 0)
        return -1;
    return 0;
}

static void mark_disconnected_locked(el_proxy_t *px)
{
    if (px->fd >= 0) {
        close(px->fd);
        px->fd = -1;
    }
    px->ready = 0;
    px->rx_acc_len = 0;
    px->tcp_state = TCP_DISC;
    px->dir_rx_len = 0;
    px->dir_rx_r = 0;
    px->tcp_status_pending = 0;
    px->reconnect_pending = 1;
    px->next_reconnect = time(NULL) + RECONNECT_SEC;
    pthread_cond_broadcast(&px->dir_cv);
}

static void dir_rx_append_locked(el_proxy_t *px, const uint8_t *data, int len)
{
    int used = px->dir_rx_len - px->dir_rx_r;
    int space;

    if (len <= 0)
        return;
    if (!px->dir_rx || px->dir_rx_cap <= 0) {
        LOG_EL_ERROR("echolink: proxy directory RX buffer missing\n");
        mark_disconnected_locked(px);
        return;
    }
    if (px->dir_rx_r > 0 && used > 0) {
        memmove(px->dir_rx, px->dir_rx + px->dir_rx_r, (size_t)used);
        px->dir_rx_len = used;
        px->dir_rx_r = 0;
    } else if (px->dir_rx_r > 0) {
        px->dir_rx_len = 0;
        px->dir_rx_r = 0;
    }
    space = px->dir_rx_cap - px->dir_rx_len;
    if (len > space) {
        LOG_EL_ERROR("echolink: proxy directory RX overflow (%d + %d > %d)\n",
                     px->dir_rx_len, len, px->dir_rx_cap);
        mark_disconnected_locked(px);
        return;
    }
    memcpy(px->dir_rx + px->dir_rx_len, data, (size_t)len);
    px->dir_rx_len += len;
    pthread_cond_broadcast(&px->dir_cv);
}

static void handle_msg_locked(el_proxy_t *px, int type, struct in_addr remote,
                              const uint8_t *data, int len,
                              el_proxy_udp_cb_t cb, void *user, int *udp_n)
{
    switch (type) {
    case MSG_TCP_DATA:
        dir_rx_append_locked(px, data, len);
        break;
    case MSG_TCP_CLOSE:
        px->tcp_state = TCP_DISC;
        px->tcp_status_pending = 0;
        pthread_cond_broadcast(&px->dir_cv);
        break;
    case MSG_TCP_STATUS:
        if (len == 4) {
            px->tcp_status = (uint32_t)data[0]
                | ((uint32_t)data[1] << 8)
                | ((uint32_t)data[2] << 16)
                | ((uint32_t)data[3] << 24);
            px->tcp_status_pending = 1;
            if (px->tcp_state == TCP_CONNECTING) {
                if (px->tcp_status == 0) {
                    px->tcp_state = TCP_CONNECTED;
                    px->dir_rx_len = 0;
                    px->dir_rx_r = 0;
                } else {
                    LOG_EL_WARNING("echolink: proxy TCP_STATUS fail (%u)\n",
                                   (unsigned)px->tcp_status);
                    px->tcp_state = TCP_DISC;
                }
            }
            pthread_cond_broadcast(&px->dir_cv);
        }
        break;
    case MSG_UDP_DATA:
        if (cb && len > 0) {
            cb(user, remote, 0, data, len);
            if (udp_n)
                (*udp_n)++;
        }
        break;
    case MSG_UDP_CONTROL:
        if (cb && len > 0) {
            cb(user, remote, 1, data, len);
            if (udp_n)
                (*udp_n)++;
        }
        break;
    case MSG_SYSTEM:
        if (len == 1) {
            if (data[0] == SYS_BAD_PASSWORD)
                LOG_EL_ERROR("echolink: proxy bad password\n");
            else if (data[0] == SYS_ACCESS_DENIED)
                LOG_EL_ERROR("echolink: proxy access denied\n");
            else
                LOG_EL_WARNING("echolink: proxy SYSTEM %u\n",
                               (unsigned)data[0]);
            mark_disconnected_locked(px);
        }
        break;
    case MSG_TCP_OPEN:
        LOG_EL_WARNING("echolink: unexpected TCP_OPEN from proxy\n");
        break;
    default:
        LOG_EL_WARNING("echolink: unknown proxy msg type %d\n", type);
        mark_disconnected_locked(px);
        break;
    }
}

/* Parse complete frames from rx_acc. Returns UDP packets delivered. */
static int parse_acc_locked(el_proxy_t *px, el_proxy_udp_cb_t cb, void *user)
{
    int udp_n = 0;

    while (px->rx_acc_len >= MSG_HDR) {
        int type = px->rx_acc[0];
        struct in_addr remote;
        uint32_t msg_len;
        int total;

        remote.s_addr = (uint32_t)px->rx_acc[1]
            | ((uint32_t)px->rx_acc[2] << 8)
            | ((uint32_t)px->rx_acc[3] << 16)
            | ((uint32_t)px->rx_acc[4] << 24);
        msg_len = (uint32_t)px->rx_acc[5]
            | ((uint32_t)px->rx_acc[6] << 8)
            | ((uint32_t)px->rx_acc[7] << 16)
            | ((uint32_t)px->rx_acc[8] << 24);
        if (msg_len > sizeof(px->rx_acc) - MSG_HDR) {
            LOG_EL_ERROR("echolink: proxy frame too large (%u)\n",
                         (unsigned)msg_len);
            mark_disconnected_locked(px);
            return udp_n;
        }
        total = MSG_HDR + (int)msg_len;
        if (px->rx_acc_len < total)
            break;
        handle_msg_locked(px, type, remote, px->rx_acc + MSG_HDR, (int)msg_len,
                          cb, user, &udp_n);
        if (!px->ready)
            break;
        px->rx_acc_len -= total;
        if (px->rx_acc_len > 0)
            memmove(px->rx_acc, px->rx_acc + total, (size_t)px->rx_acc_len);
    }
    return udp_n;
}

/* Read available bytes from proxy fd and parse. Caller holds mu. */
static int drain_locked(el_proxy_t *px, el_proxy_udp_cb_t cb, void *user)
{
    int udp_n = 0;

    if (px->fd < 0 || !px->ready)
        return 0;
    for (;;) {
        int space = (int)sizeof(px->rx_acc) - px->rx_acc_len;
        int n;

        if (space <= 0) {
            mark_disconnected_locked(px);
            return udp_n;
        }
        n = (int)read(px->fd, px->rx_acc + px->rx_acc_len, (size_t)space);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            LOG_EL_WARNING("echolink: proxy read: %s\n", strerror(errno));
            mark_disconnected_locked(px);
            return udp_n;
        }
        if (n == 0) {
            LOG_EL_WARNING("echolink: proxy disconnected\n");
            mark_disconnected_locked(px);
            return udp_n;
        }
        px->rx_acc_len += n;
        udp_n += parse_acc_locked(px, cb, user);
        if (!px->ready)
            break;
    }
    return udp_n;
}

#define EL_PROXY_MD5_LEN 16

static int proxy_md5(const void *a, size_t alen, const void *b, size_t blen,
                     uint8_t out[EL_PROXY_MD5_LEN])
{
    EVP_MD_CTX *ctx;
    unsigned int mdlen = 0;

    ctx = EVP_MD_CTX_new();
    if (!ctx)
        return -1;
    if (EVP_DigestInit_ex(ctx, EVP_md5(), NULL) != 1
        || EVP_DigestUpdate(ctx, a, alen) != 1
        || EVP_DigestUpdate(ctx, b, blen) != 1
        || EVP_DigestFinal_ex(ctx, out, &mdlen) != 1
        || mdlen != EL_PROXY_MD5_LEN) {
        EVP_MD_CTX_free(ctx);
        return -1;
    }
    EVP_MD_CTX_free(ctx);
    return 0;
}

static int do_connect_auth(el_proxy_t *px)
{
    struct sockaddr_in sa;
    struct in_addr ip;
    struct timeval tv;
    uint8_t nonce[NONCE_SIZE];
    uint8_t auth[32 + 1 + EL_PROXY_MD5_LEN];
    uint8_t digest[EL_PROXY_MD5_LEN];
    int n, auth_len;
    size_t cs_len;

    if (px->fd >= 0) {
        close(px->fd);
        px->fd = -1;
    }
    px->ready = 0;
    if (ensure_dir_rx_locked(px) != 0) {
        LOG_EL_ERROR("echolink: proxy directory RX alloc failed\n");
        return -1;
    }
    if (resolve_host(px->host, &ip) != 0) {
        LOG_EL_WARNING("echolink: proxy cannot resolve %s\n", px->host);
        return -1;
    }
    px->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (px->fd < 0)
        return -1;
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    setsockopt(px->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(px->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)px->port);
    sa.sin_addr = ip;
    if (connect(px->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG_EL_WARNING("echolink: proxy connect %s:%d: %s\n",
                       px->host, px->port, strerror(errno));
        close(px->fd);
        px->fd = -1;
        return -1;
    }
    n = (int)read(px->fd, nonce, NONCE_SIZE);
    if (n != NONCE_SIZE) {
        if (n < 0)
            LOG_EL_WARNING("echolink: proxy auth nonce failed (%d): %s\n",
                           n, strerror(errno));
        else
            LOG_EL_WARNING("echolink: proxy auth nonce failed (got %d of %d)\n",
                           n, NONCE_SIZE);
        close(px->fd);
        px->fd = -1;
        return -1;
    }
    cs_len = strlen(px->callsign);
    if (cs_len + 1 + EL_PROXY_MD5_LEN > sizeof(auth)) {
        close(px->fd);
        px->fd = -1;
        return -1;
    }
    memcpy(auth, px->callsign, cs_len);
    auth[cs_len] = '\n';
    if (proxy_md5(px->password, strlen(px->password), nonce, NONCE_SIZE,
                  digest) != 0) {
        LOG_EL_WARNING("echolink: proxy MD5 failed\n");
        close(px->fd);
        px->fd = -1;
        return -1;
    }
    memcpy(auth + cs_len + 1, digest, EL_PROXY_MD5_LEN);
    auth_len = (int)cs_len + 1 + EL_PROXY_MD5_LEN;
    if (send_raw_locked(px, auth, auth_len) != 0) {
        close(px->fd);
        px->fd = -1;
        return -1;
    }
    if (set_nonblock(px->fd) != 0) {
        close(px->fd);
        px->fd = -1;
        return -1;
    }
    px->ready = 1;
    px->reconnect_pending = 0;
    px->rx_acc_len = 0;
    px->tcp_state = TCP_DISC;
    px->dir_rx_len = 0;
    px->dir_rx_r = 0;
    LOG_EL_INFO("echolink: connected to proxy %s:%d as %s\n",
                px->host, px->port, px->callsign);
    return 0;
}

int el_proxy_open(el_proxy_t *px, const char *host, int port,
                  const char *callsign, const char *password)
{
    if (!px || !host || !host[0] || !callsign || !callsign[0])
        return -1;
    pthread_mutex_lock(&px->mu);
    if (px->fd >= 0) {
        close(px->fd);
        px->fd = -1;
    }
    px->ready = 0;
    if (ensure_dir_rx_locked(px) != 0) {
        LOG_EL_ERROR("echolink: proxy directory RX alloc failed\n");
        pthread_mutex_unlock(&px->mu);
        return -1;
    }
    strncpy(px->host, host, sizeof(px->host) - 1);
    px->host[sizeof(px->host) - 1] = '\0';
    px->port = (port > 0) ? port : EL_PROXY_DEFAULT_PORT;
    strncpy(px->callsign, callsign, sizeof(px->callsign) - 1);
    px->callsign[sizeof(px->callsign) - 1] = '\0';
    upper_password(px->password, sizeof(px->password), password);
    if (do_connect_auth(px) != 0) {
        px->reconnect_pending = 1;
        px->next_reconnect = time(NULL) + RECONNECT_SEC;
        pthread_mutex_unlock(&px->mu);
        return -1;
    }
    pthread_mutex_unlock(&px->mu);
    return 0;
}

void el_proxy_close(el_proxy_t *px)
{
    if (!px)
        return;
    pthread_mutex_lock(&px->mu);
    if (px->fd >= 0) {
        close(px->fd);
        px->fd = -1;
    }
    px->ready = 0;
    px->reconnect_pending = 0;
    px->rx_acc_len = 0;
    px->tcp_state = TCP_DISC;
    px->dir_rx_len = 0;
    px->dir_rx_r = 0;
    pthread_cond_broadcast(&px->dir_cv);
    pthread_mutex_unlock(&px->mu);
}

int el_proxy_ready(const el_proxy_t *px)
{
    return px && px->ready && px->fd >= 0;
}

int el_proxy_ensure(el_proxy_t *px)
{
    time_t now;

    if (!px)
        return -1;
    pthread_mutex_lock(&px->mu);
    if (px->ready && px->fd >= 0) {
        pthread_mutex_unlock(&px->mu);
        return 0;
    }
    now = time(NULL);
    if (px->reconnect_pending && now < px->next_reconnect) {
        pthread_mutex_unlock(&px->mu);
        return -1;
    }
    if (do_connect_auth(px) != 0) {
        px->reconnect_pending = 1;
        px->next_reconnect = now + RECONNECT_SEC;
        pthread_mutex_unlock(&px->mu);
        return -1;
    }
    pthread_mutex_unlock(&px->mu);
    return 0;
}

int el_proxy_poll(el_proxy_t *px, el_proxy_udp_cb_t cb, void *user)
{
    int n;

    if (!px)
        return 0;
    pthread_mutex_lock(&px->mu);
    n = drain_locked(px, cb, user);
    pthread_mutex_unlock(&px->mu);
    return n;
}

int el_proxy_udp_data(el_proxy_t *px, struct in_addr to,
                      const void *data, int len)
{
    int rc;

    if (!px || !data || len <= 0)
        return -1;
    pthread_mutex_lock(&px->mu);
    rc = send_msg_locked(px, MSG_UDP_DATA, to, data, len);
    if (rc != 0)
        mark_disconnected_locked(px);
    pthread_mutex_unlock(&px->mu);
    return rc;
}

int el_proxy_udp_ctrl(el_proxy_t *px, struct in_addr to,
                      const void *data, int len)
{
    int rc;

    if (!px || !data || len <= 0)
        return -1;
    pthread_mutex_lock(&px->mu);
    rc = send_msg_locked(px, MSG_UDP_CONTROL, to, data, len);
    if (rc != 0)
        mark_disconnected_locked(px);
    pthread_mutex_unlock(&px->mu);
    return rc;
}

static int wait_ms_locked(el_proxy_t *px, int timeout_ms)
{
    struct timespec ts;
    struct timeval tv;

    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec + timeout_ms / 1000;
    ts.tv_nsec = (long)(tv.tv_usec * 1000L) + (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(&px->dir_cv, &px->mu, &ts);
}

int el_proxy_tcp_open(el_proxy_t *px, struct in_addr remote, int timeout_ms)
{
    struct in_addr zero;
    time_t deadline;
    int rc = -1;

    if (!px)
        return -1;
    memset(&zero, 0, sizeof(zero));
    deadline = time(NULL) + (timeout_ms > 0 ? (timeout_ms + 999) / 1000 : 20);

    pthread_mutex_lock(&px->mu);
    if (!px->ready) {
        pthread_mutex_unlock(&px->mu);
        return -1;
    }
    if (px->tcp_state == TCP_CONNECTED || px->tcp_state == TCP_CONNECTING) {
        /* Close stale channel first */
        send_msg_locked(px, MSG_TCP_CLOSE, zero, NULL, 0);
        px->tcp_state = TCP_DISC;
    }
    px->dir_rx_len = 0;
    px->dir_rx_r = 0;
    px->tcp_status_pending = 0;
    px->tcp_state = TCP_CONNECTING;
    if (send_msg_locked(px, MSG_TCP_OPEN, remote, NULL, 0) != 0) {
        mark_disconnected_locked(px);
        pthread_mutex_unlock(&px->mu);
        return -1;
    }
    while (px->ready && px->tcp_state == TCP_CONNECTING) {
        drain_locked(px, NULL, NULL);
        if (px->tcp_state == TCP_CONNECTED) {
            rc = 0;
            break;
        }
        if (px->tcp_state == TCP_DISC) {
            rc = -1;
            break;
        }
        if (time(NULL) >= deadline) {
            LOG_EL_WARNING("echolink: proxy TCP_OPEN timeout\n");
            px->tcp_state = TCP_DISC;
            rc = -1;
            break;
        }
        wait_ms_locked(px, 200);
    }
    if (px->tcp_state == TCP_CONNECTED)
        rc = 0;
    pthread_mutex_unlock(&px->mu);
    return rc;
}

int el_proxy_tcp_write(el_proxy_t *px, const void *data, int len)
{
    struct in_addr zero;
    int rc;

    if (!px || !data || len <= 0)
        return -1;
    memset(&zero, 0, sizeof(zero));
    pthread_mutex_lock(&px->mu);
    if (!px->ready || px->tcp_state != TCP_CONNECTED) {
        pthread_mutex_unlock(&px->mu);
        return -1;
    }
    rc = send_msg_locked(px, MSG_TCP_DATA, zero, data, len);
    if (rc != 0)
        mark_disconnected_locked(px);
    pthread_mutex_unlock(&px->mu);
    return rc;
}

int el_proxy_tcp_read(el_proxy_t *px, void *buf, int buflen, int timeout_ms)
{
    time_t deadline;
    int got = 0;

    if (!px || !buf || buflen <= 0)
        return -1;
    deadline = time(NULL) + (timeout_ms > 0 ? (timeout_ms + 999) / 1000 : 20);

    pthread_mutex_lock(&px->mu);
    /*
     * Deliver buffered TCP_DATA even after TCP_CLOSE (directory EOF).
     * Previously we required tcp_state==CONNECTED and dropped the last
     * chunks — that truncated zlib station lists (Z_DATA_ERROR).
     */
    for (;;) {
        int avail;

        if (px->fd >= 0 && px->ready)
            drain_locked(px, NULL, NULL);

        avail = px->dir_rx_len - px->dir_rx_r;
        if (avail > 0) {
            int take = avail < buflen ? avail : buflen;

            memcpy(buf, px->dir_rx + px->dir_rx_r, (size_t)take);
            px->dir_rx_r += take;
            if (px->dir_rx_r >= px->dir_rx_len) {
                px->dir_rx_r = 0;
                px->dir_rx_len = 0;
            }
            got = take;
            break;
        }
        /* Empty buffer + closed directory TCP (or proxy down) → EOF */
        if (!px->ready || px->tcp_state != TCP_CONNECTED)
            break;
        if (time(NULL) >= deadline)
            break;
        wait_ms_locked(px, 200);
    }
    pthread_mutex_unlock(&px->mu);
    /* 0 = EOF/timeout (same as read()); >0 = bytes */
    return got;
}

int el_proxy_tcp_close(el_proxy_t *px)
{
    struct in_addr zero;
    int rc = 0;

    if (!px)
        return -1;
    memset(&zero, 0, sizeof(zero));
    pthread_mutex_lock(&px->mu);
    if (px->ready && px->tcp_state != TCP_DISC) {
        px->tcp_state = TCP_DISCONNECTING;
        if (send_msg_locked(px, MSG_TCP_CLOSE, zero, NULL, 0) != 0) {
            mark_disconnected_locked(px);
            rc = -1;
        }
    }
    px->tcp_state = TCP_DISC;
    px->dir_rx_len = 0;
    px->dir_rx_r = 0;
    pthread_cond_broadcast(&px->dir_cv);
    pthread_mutex_unlock(&px->mu);
    return rc;
}
