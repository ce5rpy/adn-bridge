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

#define YSF2DMR_MODE_YSF_DMR      0
#define YSF2DMR_MODE_ECHOLINK_DMR 1
#define YSF2DMR_MODE_ECHOLINK_YSF 2

#define YSF2DMR_EL_DIR_MAX 8

typedef struct {
    char callsign[16];
    char password[64];
    char bind_addr[64];
    char host[128]; /* node/conference callsign, e.g. CA5RPY-L or *REDCHILE* */
    char qth[32];
    char email[64];
    char directory_servers[YSF2DMR_EL_DIR_MAX][128];
    int directory_server_count;
    /* tlb LoginInterval / StationListInterval (seconds); 0 disables */
    int login_interval;
    int station_list_interval;
    /*
     * Linear PCM gain for EchoLink → DMR/YSF (before AMBE encode).
     * 1.0 = unity, 4.0 = max. Range (0, 4].
     */
    float gain;
    /* Optional EchoLink Proxy. Empty proxy_server = direct UDP/TCP. */
    char proxy_server[128];
    int proxy_port; /* default 8100 when proxy_server set */
    char proxy_password[64]; /* default PUBLIC when proxy_server set and empty */
    /* -1 = inherit [log] level=; else DEBUG|INFO|WARNING|ERROR */
    int log_level;
} ysf2dmr_echolink_cfg_t;

typedef struct {
    char host[128];
    int port;
    int log_level; /* -1 = inherit [log] level= */
} ysf2dmr_vocoder_cfg_t;

typedef struct {
    int mode; /* YSF2DMR_MODE_* */
    char callsign[16];
    int dmrid;
    char description[20];
    char location[21];
    char ysf_host[128];
    int ysf_port;
    int dgid;
    char dmr_host[128];
    int dmr_port;
    char dmr_options[128]; /* optional RPTO; empty = no RPTO, else sent as-is */
    char dmr_password[64];
    int dmr_tg;            /* mandatory [dmr] tg= — voice + connect PTT */
    /* 1 = on DMR login, silence-PTT TG 4000 then configured tg (drop dynamics) */
    int dmr_clear_dynamic_tg;
    int default_ysf_dmrid; /* legacy INI key; bridge [dmr] dmrid is used instead */
    log_level_t log_level; /* [log] level= — default for all channels */
    int dmr_log_level;     /* [dmr] log_level=; -1 = inherit */
    int ysf_log_level;     /* [ysf] log_level=; -1 = inherit */
    ysf2dmr_aliases_cfg_t aliases;
    ysf2dmr_echolink_cfg_t echolink;
    ysf2dmr_vocoder_cfg_t vocoder;
} ysf2dmr_config_t;

void ysf2dmr_config_init(ysf2dmr_config_t *cfg);
/* Resolve default ysf2dmrcon.ini (cwd, then directory of argv[0]). */
int ysf2dmr_config_default_path(const char *argv0, char *path, size_t pathlen);
/* Returns 0 on success, -1 on error (message in err, errlen). */
int ysf2dmr_config_load(const char *path, ysf2dmr_config_t *cfg, char *err, size_t errlen);
int ysf2dmr_config_valid(const ysf2dmr_config_t *cfg, char *err, size_t errlen);
void ysf2dmr_config_apply_log_levels(const ysf2dmr_config_t *cfg);
const char *ysf2dmr_mode_name(int mode);

#endif
