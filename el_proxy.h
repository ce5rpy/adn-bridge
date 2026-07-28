/*
 * EchoLink Proxy client.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef ADN_BRIDGE_EL_PROXY_H
#define ADN_BRIDGE_EL_PROXY_H

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#define EL_PROXY_DEFAULT_PORT 8100

typedef struct el_proxy el_proxy_t;

/* UDP payload from proxy (RTP = data, RTCP = ctrl). */
typedef void (*el_proxy_udp_cb_t)(void *user, struct in_addr from, int is_ctrl,
                                  const uint8_t *data, int len);

struct el_proxy {
    int fd;
    int ready; /* authenticated */
    char host[128];
    int port;
    char callsign[16];
    char password[64]; /* uppercased; PUBLIC for public proxies */
    pthread_mutex_t mu;
    /* Incomplete TCP frame assembly */
    uint8_t rx_acc[65536];
    int rx_acc_len;
    /* Directory TCP channel through proxy (heap; sized for station list) */
    int tcp_state; /* 0 disc, 1 connecting, 2 connected, 3 disconnecting */
    uint8_t *dir_rx;
    int dir_rx_cap;
    int dir_rx_len;
    int dir_rx_r;
    pthread_cond_t dir_cv;
    uint32_t tcp_status; /* last TCP_STATUS word */
    int tcp_status_pending;
    time_t next_reconnect;
    int reconnect_pending;
};

void el_proxy_init(el_proxy_t *px);
void el_proxy_clear(el_proxy_t *px);

/* Configure and connect+auth. password empty → PUBLIC. Returns 0 if ready. */
int el_proxy_open(el_proxy_t *px, const char *host, int port,
                  const char *callsign, const char *password);

void el_proxy_close(el_proxy_t *px);

int el_proxy_ready(const el_proxy_t *px);

/*
 * Ensure connected (reconnect if needed). Returns 0 if ready, -1 if not.
 */
int el_proxy_ensure(el_proxy_t *px);

/* Non-blocking drain of proxy TCP; demux UDP to cb. Returns UDP packets handled. */
int el_proxy_poll(el_proxy_t *px, el_proxy_udp_cb_t cb, void *user);

int el_proxy_udp_data(el_proxy_t *px, struct in_addr to,
                      const void *data, int len);
int el_proxy_udp_ctrl(el_proxy_t *px, struct in_addr to,
                      const void *data, int len);

/* Directory TCP via proxy (blocking with internal drain). */
int el_proxy_tcp_open(el_proxy_t *px, struct in_addr remote, int timeout_ms);
int el_proxy_tcp_write(el_proxy_t *px, const void *data, int len);
int el_proxy_tcp_read(el_proxy_t *px, void *buf, int buflen, int timeout_ms);
int el_proxy_tcp_close(el_proxy_t *px);

#endif
