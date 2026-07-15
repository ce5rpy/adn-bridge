/*
 * Subscriber alias load and lookup.
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

#ifndef YSF2DMR_ALIASES_H
#define YSF2DMR_ALIASES_H

#include <stddef.h>

typedef struct {
    int try_download;
    int stale_minutes; /* re-download files older than this (0 = always) */
    char path[256];
    char subscriber_file[64];
    char subscriber_url[256];
    char local_subscriber_file[64];
    char checksum_file[64];
    char checksum_url[256];
} ysf2dmr_aliases_cfg_t;

typedef struct ysf2dmr_aliases ysf2dmr_aliases_t;

void ysf2dmr_aliases_cfg_init(ysf2dmr_aliases_cfg_t *cfg);

/* Download (if enabled) and load subscriber alias files. Returns 0 on success. */
int ysf2dmr_aliases_load(const ysf2dmr_aliases_cfg_t *cfg, ysf2dmr_aliases_t **out);

void ysf2dmr_aliases_free(ysf2dmr_aliases_t *aliases);

/* callsign -> DMR ID; 0 if unknown. */
int ysf2dmr_alias_lookup_id(const ysf2dmr_aliases_t *aliases, const char *callsign);

/* DMR ID -> callsign padded to 10 chars; returns 1 if found. */
int ysf2dmr_alias_lookup_callsign(const ysf2dmr_aliases_t *aliases, int dmrid,
                                  char out[10]);

#endif
