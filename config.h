/*
 * INI configuration types for adn-bridge.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_BRIDGE_CONFIG_H
#define ADN_BRIDGE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "log.h"
#include "aliases.h"

#define ADN_BRIDGE_PEER_MAX 16
#define ADN_BRIDGE_PEER_NAME_LEN 32
#define ADN_BRIDGE_EL_DIR_MAX 8

typedef enum {
    ADN_BRIDGE_PEER_TYPE_DMR = 0,
    ADN_BRIDGE_PEER_TYPE_YSF,
    ADN_BRIDGE_PEER_TYPE_ECHOLINK,
} adn_bridge_peer_type_t;

typedef struct {
    char host[128];
    int port;
    char callsign[16];
    int dmrid;
    char description[20];
    char location[21];
    char options[128];
    char password[64];
    int tg;
    int clear_dynamic_tg;
    int log_level; /* -1 = inherit [log] level= */
} adn_bridge_peer_dmr_t;

typedef struct {
    char host[128];
    int port;
    char callsign[16];
    int dgid;
    int log_level;
} adn_bridge_peer_ysf_t;

typedef struct {
    char callsign[16];
    char password[64];
    char bind_addr[64];
    char host[128];
    char qth[32];
    char email[64];
    char directory_servers[ADN_BRIDGE_EL_DIR_MAX][128];
    int directory_server_count;
    int login_interval;
    int station_list_interval;
    float gain;
    char proxy_server[128];
    int proxy_port;
    char proxy_password[64];
    char vocoder_host[128];
    int vocoder_port;
    int vocoder_log_level; /* -1 = inherit [log] level= */
    int log_level;
} adn_bridge_peer_el_t;

typedef struct {
    char name[ADN_BRIDGE_PEER_NAME_LEN];
    adn_bridge_peer_type_t type;
    int enabled;
    int type_set;
    union {
        adn_bridge_peer_dmr_t dmr;
        adn_bridge_peer_ysf_t ysf;
        adn_bridge_peer_el_t  el;
    } u;
} adn_bridge_peer_t;

/* [log] handlers=/file= — see docs/bridge.md's [log] section. handlers_set
 * distinguishes "omitted" (auto-detect console vs console-timed) from an
 * explicit `handlers = null` (both sinks off). */
typedef struct {
    int  handlers_set;
    int  console;
    int  console_timed;
    int  file;
    int  file_timed;
    char file_path[256];
} adn_bridge_log_output_t;

typedef struct {
    log_level_t log_level;
    adn_bridge_log_output_t log_output;
    adn_bridge_aliases_cfg_t aliases;
    int peer_count;
    adn_bridge_peer_t peers[ADN_BRIDGE_PEER_MAX];
} adn_bridge_config_t;

int adn_bridge_config_enabled_peer_count(const adn_bridge_config_t *cfg);
const adn_bridge_peer_t *adn_bridge_config_find_peer(const adn_bridge_config_t *cfg,
                                                     adn_bridge_peer_type_t type);

/* Descriptive peer-mix string for logs, e.g. "2x dmr + 1x ysf + 1x echolink". */
const char *adn_bridge_layout_name(const adn_bridge_config_t *cfg);

int adn_bridge_config_load(const char *path, adn_bridge_config_t *cfg, char *err, size_t errlen);
int adn_bridge_config_valid(const adn_bridge_config_t *cfg, char *err, size_t errlen);
void adn_bridge_config_apply_log_levels(const adn_bridge_config_t *cfg);
/* Resolves [log] handlers=/file= (or auto-detects when handlers= is omitted)
 * and calls log_init(). Call once, after adn_bridge_config_apply_log_levels. */
void adn_bridge_config_apply_log_output(const adn_bridge_config_t *cfg);

#endif
