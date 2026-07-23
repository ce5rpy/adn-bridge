/*
 * DMR Homebrew peer client (hotspot-style login).
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

#include "peer_dmr.h"
#include "hbp/dmr_hbp.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BUFSIZE DMR_HBP_BUFSIZE
#define TIMEOUT 30
#define DISCONNECTED DMR_HBP_DISCONNECTED
#define CONNECTING   DMR_HBP_CONNECTING
#define CONNECTED    DMR_HBP_CONNECTED

static void pad_copy(char *dst, size_t n, const char *src)
{
    memset(dst, ' ', n);
    if (src && src[0]) {
        size_t src_len = strlen(src);
        size_t copy_len = src_len < n ? src_len : n;
        memcpy(dst, src, copy_len);
    }
}

static size_t base_callsign_len(const char *cs)
{
    size_t i = 0;

    if (!cs)
        return 0;
    while (cs[i] && cs[i] != '-' && cs[i] != '/' && cs[i] != '_')
        i++;
    return i;
}

static void send_rptc(peer_dmr_t *p)
{
    uint8_t out[302];
    char body[294];
    int id = get_dmrid(p->dmrid);
    const char *desc = p->description[0] ? p->description : "adn-bridge";
    /* Monitor Linked Systems shows LOCATION only (not DESCRIPTION). */
    const char *loc = p->location[0] ? p->location
                    : (p->description[0] ? p->description : "");

    memset(body, 0, sizeof(body));
    pad_copy(body + 0, 8, p->callsign);
    pad_copy(body + 8, 9, "000000000");
    pad_copy(body + 17, 9, "000000000");
    pad_copy(body + 26, 2, "99");
    pad_copy(body + 28, 2, "01");
    pad_copy(body + 30, 8, "00000000");
    pad_copy(body + 38, 9, "000000000");
    pad_copy(body + 47, 3, "000");
    pad_copy(body + 50, 20, loc);
    pad_copy(body + 70, 19, desc);
    body[89] = p->slots ? (uint8_t)p->slots : (uint8_t)'0';
    pad_copy(body + 90, 124, "");
    pad_copy(body + 214, 40, p->software_id[0] ? p->software_id : "MMDVMHost");
    pad_copy(body + 254, 40, p->package_id[0] ? p->package_id : "adn-bridge");

    memcpy(out, "RPTC", 4);
    out[4] = (id >> 24) & 0xff;
    out[5] = (id >> 16) & 0xff;
    out[6] = (id >> 8) & 0xff;
    out[7] = (id >> 0) & 0xff;
    memcpy(out + 8, body, sizeof(body));

    sendto(p->sock, out, sizeof(out), 0, (const struct sockaddr *)&p->peer, sizeof(p->peer));
    fprintf(stderr, "DMR: RPTC sent (location=%s description=%s)\n",
            loc[0] ? loc : "(empty)", desc);
}

/*
 * RPTO wire: "RPTO" + radio_id(4) + OPTIONS ASCII, variable length.
 * Unlike RPTC (fixed fields space-padded via pad_copy / PR #1), do NOT pad
 * OPTIONS with spaces or NULs — send exactly strlen(options) bytes (dmrcon /
 * hblink / adn-server all treat _data[8:] as the raw options string).
 */
static void send_rpto(peer_dmr_t *p)
{
    char out[200];
    size_t opt_len;
    int len;
    int id = get_dmrid(p->dmrid);

    if (!p->options[0])
        return;
    /* Ensure terminator even if a prior strncpy filled the buffer. */
    p->options[sizeof(p->options) - 1] = '\0';
    opt_len = strlen(p->options);
    if (opt_len > sizeof(out) - 9)
        opt_len = sizeof(out) - 9;

    memcpy(out, "RPTO", 4);
    out[4] = (id >> 24) & 0xff;
    out[5] = (id >> 16) & 0xff;
    out[6] = (id >> 8) & 0xff;
    out[7] = (id >> 0) & 0xff;
    memcpy(&out[8], p->options, opt_len);
    len = 8 + (int)opt_len;
    sendto(p->sock, out, len, 0, (const struct sockaddr *)&p->peer, sizeof(p->peer));
    fprintf(stderr, "DMR: RPTO sent (OPTIONS=%s)\n", p->options);
}

int peer_dmr_open(peer_dmr_t *p, const char *host, int port, const char *cs,
                  int id, int tg, const char *options,
                  const char *password,
                  const char *description, const char *location)
{
    struct hostent *hp;

    memset(p, 0, sizeof(*p));
    p->sock = -1;
    p->dmrid = id;
    p->tg = tg;
    if (options && options[0]) {
        strncpy(p->options, options, sizeof(p->options) - 1);
        p->options[sizeof(p->options) - 1] = '\0';
    }
    p->slots = '0';
    strncpy(p->password, password ? password : "", sizeof(p->password) - 1);
    if (description && description[0])
        strncpy(p->description, description, sizeof(p->description) - 1);
    if (location && location[0])
        strncpy(p->location, location, sizeof(p->location) - 1);
    strncpy(p->package_id, "adn-bridge", sizeof(p->package_id) - 1);
    strncpy(p->software_id, "MMDVMHost", sizeof(p->software_id) - 1);
    memset(p->callsign, ' ', 10);
    {
        size_t cs_len = base_callsign_len(cs);

        if (cs_len > 10)
            cs_len = 10;
        memcpy(p->callsign, cs, cs_len);
    }

    if ((p->sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("peer_dmr socket");
        return -1;
    }

    memset(&p->peer, 0, sizeof(p->peer));
    p->peer.sin_family = AF_INET;
    p->peer.sin_port = htons((uint16_t)port);
    hp = gethostbyname(host);
    if (!hp) {
        fprintf(stderr, "peer_dmr: cannot resolve %s\n", host);
        return -1;
    }
    memcpy(&p->peer.sin_addr, hp->h_addr_list[0], hp->h_length);

    p->login_phase = 0;
    p->pong_time = time(NULL);
    p->status = DISCONNECTED;
    fprintf(stderr, "DMR peer: %s:%d TG %d (hotspot/HBP client)\n", host, port, tg);
    return 0;
}

void peer_dmr_close(peer_dmr_t *p)
{
    if (p->sock >= 0) {
        close(p->sock);
        p->sock = -1;
    }
}

int peer_dmr_connected(const peer_dmr_t *p)
{
    return p->status == PEER_DMR_CONNECTED;
}

void peer_dmr_tick(peer_dmr_t *p)
{
    time_t now = time(NULL);
    int id;

    if (p->status == DISCONNECTED) {
        if (now < p->login_fail_until)
            return;
        p->status = CONNECTING;
        p->login_phase = 0;
        p->pong_time = now;
        id = get_dmrid(p->dmrid);
        buf[0] = 'R'; buf[1] = 'P'; buf[2] = 'T'; buf[3] = 'L';
        buf[4] = (id >> 24) & 0xff;
        buf[5] = (id >> 16) & 0xff;
        buf[6] = (id >> 8) & 0xff;
        buf[7] = (id >> 0) & 0xff;
        sendto(p->sock, buf, 8, 0, (const struct sockaddr *)&p->peer, sizeof(p->peer));
        fprintf(stderr, "DMR: RPTL sent...\n");
    }

    if (p->status == CONNECTED) {
        if (now - p->pong_time > TIMEOUT) {
            p->status = DISCONNECTED;
            fprintf(stderr, "DMR: keepalive timeout (no MSTPONG), reconnecting...\n");
        }
    } else if (p->status == CONNECTING) {
        if (now - p->pong_time > TIMEOUT * 2) {
            fprintf(stderr, "DMR: login timeout, retrying...\n");
            p->status = DISCONNECTED;
        }
    }
}

static void handle_rx(peer_dmr_t *p)
{
    /* Keepalive is MSTPONG-only (narspt/dmrcon). Refreshing on any packet
     * masked server restarts: MSTCL/stray traffic kept the session "alive"
     * while RPTPING went unanswered. */
    if (p->status == CONNECTED) {
        if (memcmp(buf, "MSTCL", 5) == 0) {
            fprintf(stderr, "DMR: MSTCL from server — disconnected, will reconnect\n");
            p->status = DISCONNECTED;
            return;
        }
        if (memcmp(buf, "MSTNAK", 6) == 0) {
            fprintf(stderr, "DMR: MSTNAK while connected — disconnected, will reconnect\n");
            p->status = DISCONNECTED;
            p->login_fail_until = time(NULL) + 3;
            return;
        }
        if (memcmp(buf, "MSTPONG", 7) == 0)
            p->pong_time = time(NULL);
    }

    if (p->status == CONNECTING) {
        if (memcmp(buf, "RPTACK", 6) == 0) {
            switch (p->login_phase) {
            case 0:
                p->status = process_connect(p->status, (char *)buf, 1, p->sock,
                                            &p->peer, p->password, p->dmrid);
                p->login_phase = 1;
                fprintf(stderr, "DMR: RPTK sent, waiting RPTACK...\n");
                break;
            case 1:
                send_rptc(p);
                p->login_phase = 2;
                break;
            case 2:
                if (p->options[0]) {
                    send_rpto(p);
                    p->login_phase = 3;
                    fprintf(stderr, "DMR: waiting RPTACK for RPTO...\n");
                } else {
                    p->status = CONNECTED;
                    p->login_phase = 4;
                    p->pong_time = time(NULL);
                    fprintf(stderr, "DMR: login complete — peer registered (no RPTO)\n");
                }
                break;
            case 3:
                p->status = CONNECTED;
                p->login_phase = 4;
                p->pong_time = time(NULL);
                fprintf(stderr, "DMR: login complete — peer registered on server\n");
                break;
            default:
                break;
            }
        } else if (memcmp(buf, "MSTNAK", 6) == 0) {
            fprintf(stderr, "DMR: login rejected (MSTNAK) at phase %d — check ID/callsign/password\n",
                    p->login_phase);
            p->status = DISCONNECTED;
            p->login_fail_until = time(NULL) + 3;
        }
    }
}

int peer_dmr_poll(peer_dmr_t *p, int timeout_ms, int *from_peer)
{
    fd_set set;
    struct timeval tv;
    struct sockaddr_in rx;
    socklen_t l = sizeof(rx);
    int r, rxlen = 0;

    *from_peer = 0;
    FD_ZERO(&set);
    FD_SET(p->sock, &set);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    r = select(p->sock + 1, &set, NULL, NULL, &tv);
    if (r <= 0)
        return 0;

    rxlen = (int)recvfrom(p->sock, buf, BUFSIZE, 0, (struct sockaddr *)&rx, &l);
    if (rxlen <= 0)
        return rxlen;

    memcpy(p->buf, buf, rxlen < (int)sizeof(p->buf) ? rxlen : (int)sizeof(p->buf));

    if (rx.sin_addr.s_addr == p->peer.sin_addr.s_addr) {
        *from_peer = 1;
        handle_rx(p);
    }
    return rxlen;
}

void peer_dmr_send(peer_dmr_t *p, const uint8_t *data, int len)
{
    /* DMRD / app traffic only while registered. Login uses sendto() directly. */
    if (p->status != CONNECTED)
        return;
    sendto(p->sock, data, len, 0, (const struct sockaddr *)&p->peer, sizeof(p->peer));
}

void peer_dmr_on_sigint(peer_dmr_t *p)
{
    if (p->sock >= 0) {
        if (p->status == CONNECTED) {
            uint8_t b[20];
            int id = get_dmrid(p->dmrid);

            b[0] = 'R'; b[1] = 'P'; b[2] = 'T'; b[3] = 'C'; b[4] = 'L';
            b[5] = (id >> 24) & 0xff;
            b[6] = (id >> 16) & 0xff;
            b[7] = (id >> 8) & 0xff;
            b[8] = (id >> 0) & 0xff;
            sendto(p->sock, b, 9, 0, (const struct sockaddr *)&p->peer, sizeof(p->peer));
        }
        peer_dmr_close(p);
    }
}

void peer_dmr_on_alarm(peer_dmr_t *p)
{
    if (p->status != CONNECTED)
        return;
    uint8_t b[20];
    const char tag[] = { 'R','P','T','P','I','N','G' };
    int id = get_dmrid(p->dmrid);

    memcpy(b, tag, 7);
    b[7] = (id >> 24) & 0xff;
    b[8] = (id >> 16) & 0xff;
    b[9] = (id >> 8) & 0xff;
    b[10] = (id >> 0) & 0xff;
    sendto(p->sock, b, 11, 0, (const struct sockaddr *)&p->peer, sizeof(p->peer));
}
