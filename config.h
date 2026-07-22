/*
 * INI configuration types for adn-bridge.
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

#ifndef ADN_BRIDGE_CONFIG_H
#define ADN_BRIDGE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "log.h"
#include "aliases.h"

#define ADN_BRIDGE_MODE_YSF_DMR      0
#define ADN_BRIDGE_MODE_ECHOLINK_DMR 1
#define ADN_BRIDGE_MODE_ECHOLINK_YSF 2

#define ADN_BRIDGE_PEER_MAX 16
#define ADN_BRIDGE_PEER_NAME_LEN 32

typedef enum {
    ADN_BRIDGE_PEER_TYPE_DMR = 0,
    ADN_BRIDGE_PEER_TYPE_YSF,
    ADN_BRIDGE_PEER_TYPE_ECHOLINK,
} adn_bridge_peer_type_t;

typedef struct {
    char name[ADN_BRIDGE_PEER_NAME_LEN];
    adn_bridge_peer_type_t type;
    int enabled;
} adn_bridge_peer_t;

#define ADN_BRIDGE_EL_DIR_MAX 8

typedef struct {
    char callsign[16];
    char password[64];
    char bind_addr[64];
    char host[128]; /* node/conference callsign, e.g. CA5RPY-L or *REDCHILE* */
    char qth[32];
    char email[64];
    char directory_servers[ADN_BRIDGE_EL_DIR_MAX][128];
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
} adn_bridge_echolink_cfg_t;

typedef struct {
    char host[128];
    int port;
    int log_level; /* -1 = inherit [log] level= */
} adn_bridge_vocoder_cfg_t;

typedef struct {
    int mode; /* ADN_BRIDGE_MODE_* */
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
    adn_bridge_aliases_cfg_t aliases;
    adn_bridge_echolink_cfg_t echolink;
    adn_bridge_vocoder_cfg_t vocoder;
    int peer_count;
    adn_bridge_peer_t peers[ADN_BRIDGE_PEER_MAX];
} adn_bridge_config_t;

void adn_bridge_config_init(adn_bridge_config_t *cfg);
/* Build implicit [peer.*] entries from legacy mode= + flat stanzas when none set. */
void adn_bridge_config_synthesize_peers(adn_bridge_config_t *cfg);
int adn_bridge_config_enabled_peer_count(const adn_bridge_config_t *cfg);
/* Resolve default adn-bridge.ini (cwd, then directory of argv[0]). */
int adn_bridge_config_default_path(const char *argv0, char *path, size_t pathlen);
/* Returns 0 on success, -1 on error (message in err, errlen). */
int adn_bridge_config_load(const char *path, adn_bridge_config_t *cfg, char *err, size_t errlen);
int adn_bridge_config_valid(const adn_bridge_config_t *cfg, char *err, size_t errlen);
void adn_bridge_config_apply_log_levels(const adn_bridge_config_t *cfg);
const char *adn_bridge_mode_name(int mode);

#endif
