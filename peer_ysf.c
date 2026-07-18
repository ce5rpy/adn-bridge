/*
 * YSF reflector peer client (YSFP, DGID activation).
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

#include "peer_ysf.h"
#include "ysf_fich.h"
#include "log.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOST_TIMEOUT 10
#define HEARTBEAT_INTERVAL 5

int peer_ysf_open(peer_ysf_t *p, const char *host, int port, const char *cs, uint8_t dgid)
{
    struct hostent *hp;

    memset(p, 0, sizeof(*p));
    p->sock = -1;
    p->dgid = dgid;
    memset(p->callsign, ' ', 10);
    {
        size_t cs_len = strlen(cs);

        if (cs_len > 10)
            cs_len = 10;
        memcpy(p->callsign, cs, cs_len);
    }

    if ((p->sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("peer_ysf socket");
        return -1;
    }

    memset(&p->peer, 0, sizeof(p->peer));
    p->peer.sin_family = AF_INET;
    p->peer.sin_port = htons((uint16_t)port);
    hp = gethostbyname(host);
    if (!hp) {
        LOG_YSF_ERROR("cannot resolve %s\n", host);
        return -1;
    }
    memcpy(&p->peer.sin_addr, hp->h_addr_list[0], hp->h_length);

    p->last_rx = time(NULL);
    p->reconnect_pending = 1;
    LOG_YSF_INFO("peer %s:%d DGID %u\n", host, port, (unsigned)dgid);
    return 0;
}

void peer_ysf_close(peer_ysf_t *p)
{
    if (p->sock >= 0) {
        close(p->sock);
        p->sock = -1;
    }
}

int peer_ysf_linked(const peer_ysf_t *p)
{
    return p->linked;
}

static void peer_ysf_send_poll(peer_ysf_t *p)
{
    uint8_t pkt[14];
    pkt[0] = 'Y'; pkt[1] = 'S'; pkt[2] = 'F'; pkt[3] = 'P';
    memcpy(pkt + 4, p->callsign, 10);
    sendto(p->sock, pkt, 14, 0, (struct sockaddr *)&p->peer, sizeof(p->peer));
}

static void peer_ysf_reconnect(peer_ysf_t *p)
{
    LOG_YSF_INFO("reconnecting (YSFP + DGID activation)...\n");
    p->linked = 0;
    peer_ysf_send_poll(p);
    sleep(1);
    if (p->dgid >= 1U)
        ysf_send_activation_burst(p->sock, &p->peer, p->callsign, p->dgid);
    p->last_rx = time(NULL);
    LOG_YSF_INFO("link setup sent (waiting for reflector)\n");
}

void peer_ysf_tick(peer_ysf_t *p)
{
    time_t now = time(NULL);

    if (p->reconnect_pending) {
        p->reconnect_pending = 0;
        peer_ysf_reconnect(p);
        return;
    }

    if ((now - p->last_rx) > LOST_TIMEOUT) {
        if (p->linked) {
            LOG_YSF_WARNING("%ld s without response, reflector down — reconnecting\n",
                            (long)(now - p->last_rx));
            p->linked = 0;
        }
        p->reconnect_pending = 1;
    }
}

int peer_ysf_poll(peer_ysf_t *p, int timeout_ms, int *from_peer)
{
    fd_set set;
    struct timeval tv;
    struct sockaddr_in rx;
    socklen_t l = sizeof(rx);
    int r, rxlen;

    *from_peer = 0;
    FD_ZERO(&set);
    FD_SET(p->sock, &set);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    r = select(p->sock + 1, &set, NULL, NULL, &tv);
    if (r <= 0)
        return 0;

    rxlen = (int)recvfrom(p->sock, p->buf, sizeof(p->buf), 0, (struct sockaddr *)&rx, &l);
    if (rxlen <= 0)
        return rxlen;

    if (rx.sin_addr.s_addr == p->peer.sin_addr.s_addr) {
        *from_peer = 1;
        p->last_rx = time(NULL);
        if (!p->linked) {
            p->linked = 1;
            LOG_YSF_INFO("linked (RX %d bytes)\n", rxlen);
        }
    }
    return rxlen;
}

static void peer_ysf_send(peer_ysf_t *p, const uint8_t *data, int len)
{
    sendto(p->sock, data, len, 0, (const struct sockaddr *)&p->peer, sizeof(p->peer));
}

void peer_ysf_send_ysfd(peer_ysf_t *p, uint8_t *frame155, int len)
{
    /* Like dgidcon: stamp configured DGID into FICH on every YSFD (voice +
     * HC/TC). Activation burst alone is not enough for dashboard/stream room. */
    if (len == 155)
        ysf_fich_rewrite_dgid(frame155, p->dgid);
    peer_ysf_send(p, frame155, len);
}

void peer_ysf_on_sigint(peer_ysf_t *p)
{
    if (p->sock >= 0) {
        uint8_t pkt[14];
        pkt[0] = 'Y'; pkt[1] = 'S'; pkt[2] = 'F'; pkt[3] = 'U';
        memcpy(pkt + 4, p->callsign, 10);
        sendto(p->sock, pkt, 14, 0, (struct sockaddr *)&p->peer, sizeof(p->peer));
        peer_ysf_close(p);
    }
}

void peer_ysf_on_alarm(peer_ysf_t *p)
{
    if (p->linked)
        peer_ysf_send_poll(p);
}
