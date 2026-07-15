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

extern struct sockaddr_in host1;
extern int udp1;
extern uint8_t buf[DMR_HBP_BUFSIZE];
extern char callsign[10];
extern int dmrid;
extern int host1_tg;
extern char *host1_pw;
extern int host1_connect_status;
extern time_t pong_time1;

int get_dmrid(int host_num, int for_traffic);
int process_connect(int connect_status, char *buf, int h);

#endif
