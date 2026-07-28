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

#ifndef ADN_BRIDGE_ALIASES_H
#define ADN_BRIDGE_ALIASES_H

#include <stddef.h>

typedef struct {
    /* Re-download when file age exceeds this many minutes.
     * 0 = always download on start; no periodic download while running.
     * >0 = also re-check while running and download when stale. */
    int stale_minutes;
    /* How often (minutes) to check whether on-disk JSON is newer than the
     * in-memory table (mtime). If newer, rebuild RAM without downloading.
     * 0 = only rebuild after our own download / startup load. Default 15. */
    int reload_minutes;
    char data_dir[256];
    char subscriber_file[64];
    char subscriber_url[256];
    char local_subscriber_file[64];
    char checksum_file[64];
    char checksum_url[256];
} adn_bridge_aliases_cfg_t;

typedef struct adn_bridge_aliases adn_bridge_aliases_t;

void adn_bridge_aliases_cfg_init(adn_bridge_aliases_cfg_t *cfg);

/* Download (if enabled) and load subscriber alias files. Returns 0 on success. */
int adn_bridge_aliases_load(const adn_bridge_aliases_cfg_t *cfg, adn_bridge_aliases_t **out);

/* Periodic maintenance: re-download when stale/checksum mismatch; and/or
 * rebuild RAM when on-disk mtime is newer than the last load (reload_minutes).
 * Returns 1 if the table was replaced, 0 if unchanged, -1 on failure
 * (previous table kept). */
int adn_bridge_aliases_maybe_refresh(const adn_bridge_aliases_cfg_t *cfg,
                                  adn_bridge_aliases_t **aliases);

void adn_bridge_aliases_free(adn_bridge_aliases_t *aliases);

/* callsign -> primary DMR ID; 0 if unknown.
 * Backing store: open-addressing tables. Every ID is in id→callsign
 * (7300391 and 7300392 → CE5RPY on DMR→YSF). Returns first file-order ID
 * for YSF→DMR (local overlay may overwrite the primary). */
int adn_bridge_alias_lookup_id(const adn_bridge_aliases_t *aliases, const char *callsign);

/* DMR ID -> callsign padded to 10 chars; exact id match (DMR->YSF). */
int adn_bridge_alias_lookup_callsign(const adn_bridge_aliases_t *aliases, int dmrid,
                                  char out[10]);

#endif
