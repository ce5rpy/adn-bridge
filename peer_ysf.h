/*
 * YSF reflector peer client.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef PEER_YSF_H
#define PEER_YSF_H

#include <stdint.h>
#include <time.h>
#include <netinet/in.h>

typedef struct {
    int sock;
    struct sockaddr_in peer;
    char callsign[10];
    uint8_t dgid;
    int linked;
    int reconnect_pending;
    time_t last_rx;
    uint8_t buf[2048];
} peer_ysf_t;

int peer_ysf_open(peer_ysf_t *p, const char *host, int port, const char *callsign, uint8_t dgid);
void peer_ysf_close(peer_ysf_t *p);
void peer_ysf_tick(peer_ysf_t *p);
int peer_ysf_linked(const peer_ysf_t *p);
int peer_ysf_poll(peer_ysf_t *p, int timeout_ms, int *from_peer);
void peer_ysf_send_ysfd(peer_ysf_t *p, uint8_t *frame155, int len);
void peer_ysf_on_sigint(peer_ysf_t *p);
void peer_ysf_on_alarm(peer_ysf_t *p);

#endif
