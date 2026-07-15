/*
 * INI configuration types for ysf2dmrcon.
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

#ifndef YSF2DMR_CONFIG_H
#define YSF2DMR_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "log.h"
#include "aliases.h"

typedef struct {
    char callsign[16];
    int dmrid;
    char description[20];
    char location[21];
    char ysf_host[128];
    int ysf_port;
    int dgid;
    char dmr_host[128];
    int dmr_port;
    char dmr_options[128]; /* RPTO OPTIONS; voice TG parsed from TS1=/TS2= */
    char dmr_password[64];
    int dmr_tg;            /* derived from options after load */
    char radio_id[6];      /* 5-char YSF RadioID for DMR->YSF CSD/DCH (DMR2YSF) */
    int default_ysf_dmrid; /* YSF talker fallback when alias lookup fails (0 = none) */
    log_level_t log_level; /* [log] level=DEBUG|INFO|WARNING|ERROR */
    ysf2dmr_aliases_cfg_t aliases;
} ysf2dmr_config_t;

void ysf2dmr_config_init(ysf2dmr_config_t *cfg);
/* Resolve default ysf2dmrcon.ini (cwd, then directory of argv[0]). */
int ysf2dmr_config_default_path(const char *argv0, char *path, size_t pathlen);
/* Returns 0 on success, -1 on error (message in err, errlen). */
int ysf2dmr_config_load(const char *path, ysf2dmr_config_t *cfg, char *err, size_t errlen);
int ysf2dmr_config_valid(const ysf2dmr_config_t *cfg, char *err, size_t errlen);

#endif
