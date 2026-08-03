/*
 * DMR Homebrew peer client.
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

#ifndef PEER_DMR_H
#define PEER_DMR_H

#include <stdint.h>
#include <time.h>
#include <netinet/in.h>

#define PEER_DMR_DISCONNECTED 0
#define PEER_DMR_CONNECTING   1
#define PEER_DMR_CONNECTED    2

typedef struct {
    int sock;
    struct sockaddr_in peer;
    char callsign[10];
    int dmrid;
    int tg;                /* voice bridge destination (DMRD) */
    char options[128];     /* RPTO OPTIONS line */
    char password[64];
    char description[20];
    char location[21];
    char freq[10]; /* RPTC announcement only, cosmetic; 9-digit Hz, used for both RX and TX */
    char software_id[41];
    char package_id[41];
    char slots; /* RPTC SLOTS: '0' = IP bridge (monitor RX/TX N/A) */
    int status;
    int login_phase; /* 0=RPTL..4=CONNECTED */
    int block_private; /* set by media_peer_bus_open_all() from config, not peer_dmr_open() --
                         * mirrors how media_peer_slot_t.clear_dynamic_tg is copied in. */
    time_t pong_time;
    time_t last_activity;
    time_t login_fail_until;
    uint8_t buf[2048];
} peer_dmr_t;

int peer_dmr_open(peer_dmr_t *p, const char *host, int port, const char *callsign,
                  int dmrid, int tg, const char *options,
                  const char *password,
                  const char *description, const char *location,
                  const char *freq);
void peer_dmr_close(peer_dmr_t *p);
void peer_dmr_tick(peer_dmr_t *p);
int peer_dmr_connected(const peer_dmr_t *p);
/* Returns bytes read (>0), 0 none, <0 error. Sets *from_peer if packet is from DMR host. */
int peer_dmr_poll(peer_dmr_t *p, int timeout_ms, int *from_peer);
void peer_dmr_send(peer_dmr_t *p, const uint8_t *data, int len);
void peer_dmr_on_sigint(peer_dmr_t *p);
void peer_dmr_on_alarm(peer_dmr_t *p);

#endif
