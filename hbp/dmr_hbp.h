/*
 * DMR Homebrew Protocol peer globals and login.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * Copyright (C) 2025 Esteban Mackay, HP3ICC
 * Copyright (C) 2019 Doug McLain
 * Based on code from reflector_connectors/dmrcon.c and MMDVM_CM
 *
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

#ifndef DMR_HBP_H
#define DMR_HBP_H

#include <stdint.h>
#include <time.h>
#include <netinet/in.h>

#define DMR_HBP_BUFSIZE 2048

#define DMR_HBP_DISCONNECTED 0
#define DMR_HBP_CONNECTING   1
#define DMR_HBP_CONNECTED    2

/* Shared scratch only — safe because engine.c drives one peer at a time,
 * synchronously, never interleaved (no threads). Per-peer session state
 * (login phase, connect status, dmrid, password, socket, server address)
 * lives in peer_dmr_t and is passed explicitly below; it must never move
 * back into globals here, or logging in >1 DMR peer breaks (each peer's
 * handshake would stomp the others' state — see peer_dmr.c history). */
extern uint8_t buf[DMR_HBP_BUFSIZE];

int get_dmrid(int dmrid);
int process_connect(int connect_status, char *buf, int h, int sock,
                    const struct sockaddr_in *peer, const char *password, int dmrid);

#endif
