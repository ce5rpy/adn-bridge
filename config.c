/*
 * INI configuration loader for adn-bridge.
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

#include "config.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>

void adn_bridge_config_init(adn_bridge_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = ADN_BRIDGE_MODE_YSF_DMR;
    cfg->log_level = LOG_LEVEL_INFO;
    cfg->dmr_log_level = -1;
    cfg->ysf_log_level = -1;
    cfg->default_ysf_dmrid = 0;
    cfg->dmr_clear_dynamic_tg = 0;
    adn_bridge_aliases_cfg_init(&cfg->aliases);
    strncpy(cfg->vocoder.host, "127.0.0.1", sizeof(cfg->vocoder.host) - 1);
    cfg->vocoder.port = 2460;
    cfg->vocoder.log_level = -1;
    /* tlb defaults: LoginInterval=360, StationListInterval=600 */
    cfg->echolink.login_interval = 360;
    cfg->echolink.station_list_interval = 600;
    cfg->echolink.gain = 1.0f;
    cfg->echolink.proxy_port = 0; /* set to 8100 when proxy_server is used */
    cfg->echolink.log_level = -1;
}

const char *adn_bridge_mode_name(int mode)
{
    switch (mode) {
    case ADN_BRIDGE_MODE_ECHOLINK_DMR: return "echolink-dmr";
    case ADN_BRIDGE_MODE_ECHOLINK_YSF: return "echolink-ysf";
    default:                        return "ysf-dmr";
    }
}

int adn_bridge_config_default_path(const char *argv0, char *path, size_t pathlen)
{
    char exebuf[PATH_MAX];

    if (access("adn-bridge.ini", R_OK) == 0) {
        strncpy(path, "adn-bridge.ini", pathlen);
        path[pathlen - 1] = '\0';
        return 0;
    }

    if (argv0 && argv0[0]) {
        strncpy(exebuf, argv0, sizeof(exebuf) - 1);
        exebuf[sizeof(exebuf) - 1] = '\0';
        snprintf(path, pathlen, "%s/adn-bridge.ini", dirname(exebuf));
        if (access(path, R_OK) == 0)
            return 0;
    }

    strncpy(path, "adn-bridge.ini", pathlen);
    path[pathlen - 1] = '\0';
    return -1;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    if (*s == 0)
        return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

static void set_str(char *dst, size_t dstlen, const char *val)
{
    if (!val || !*val)
        return;
    strncpy(dst, val, dstlen - 1);
    dst[dstlen - 1] = '\0';
}

static void set_int(int *dst, const char *val)
{
    if (!val || !*val)
        return;
    *dst = atoi(val);
}

/* 0/1, true/false, yes/no (case-insensitive). */
static void set_bool01(int *dst, const char *val)
{
    char buf[16];
    size_t i, n;

    if (!val || !*val || !dst)
        return;
    n = 0;
    for (i = 0; val[i] && n + 1 < sizeof(buf); i++) {
        if (!isspace((unsigned char)val[i]))
            buf[n++] = (char)tolower((unsigned char)val[i]);
    }
    buf[n] = '\0';
    if (strcmp(buf, "1") == 0 || strcmp(buf, "true") == 0 || strcmp(buf, "yes") == 0
        || strcmp(buf, "on") == 0)
        *dst = 1;
    else if (strcmp(buf, "0") == 0 || strcmp(buf, "false") == 0 || strcmp(buf, "no") == 0
             || strcmp(buf, "off") == 0)
        *dst = 0;
}

static void set_float(float *dst, const char *val)
{
    char *end = NULL;
    float v;

    if (!val || !*val || !dst)
        return;
    v = strtof(val, &end);
    if (end == val)
        return;
    *dst = v;
}

static void apply_identity_key(adn_bridge_config_t *cfg, const char *key, const char *val)
{
    if (strcmp(key, "callsign") == 0)
        set_str(cfg->callsign, sizeof(cfg->callsign), val);
    else if (strcmp(key, "dmrid") == 0)
        set_int(&cfg->dmrid, val);
    else if (strcmp(key, "description") == 0)
        set_str(cfg->description, sizeof(cfg->description), val);
    else if (strcmp(key, "location") == 0)
        set_str(cfg->location, sizeof(cfg->location), val);
}

static void parse_directory_servers(adn_bridge_echolink_cfg_t *el, const char *val)
{
    char buf[512];
    char *tok, *save = NULL;
    int n = 0;

    if (!val || !*val)
        return;
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (tok = strtok_r(buf, ", \t", &save); tok && n < ADN_BRIDGE_EL_DIR_MAX;
         tok = strtok_r(NULL, ", \t", &save)) {
        set_str(el->directory_servers[n], sizeof(el->directory_servers[n]), tok);
        n++;
    }
    el->directory_server_count = n;
}

static int parse_mode(const char *val)
{
    if (!val || !*val)
        return ADN_BRIDGE_MODE_YSF_DMR;
    if (strcmp(val, "echolink-dmr") == 0 || strcmp(val, "el-dmr") == 0)
        return ADN_BRIDGE_MODE_ECHOLINK_DMR;
    if (strcmp(val, "echolink-ysf") == 0 || strcmp(val, "el-ysf") == 0)
        return ADN_BRIDGE_MODE_ECHOLINK_YSF;
    return ADN_BRIDGE_MODE_YSF_DMR;
}

static int parse_peer_type(const char *val)
{
    if (!val || !*val)
        return -1;
    if (strcmp(val, "dmr") == 0)
        return ADN_BRIDGE_PEER_TYPE_DMR;
    if (strcmp(val, "ysf") == 0)
        return ADN_BRIDGE_PEER_TYPE_YSF;
    if (strcmp(val, "echolink") == 0 || strcmp(val, "el") == 0)
        return ADN_BRIDGE_PEER_TYPE_ECHOLINK;
    return -1;
}

static adn_bridge_peer_t *find_or_add_peer(adn_bridge_config_t *cfg, const char *peer_name)
{
    int i;

    for (i = 0; i < cfg->peer_count; i++) {
        if (strcmp(cfg->peers[i].name, peer_name) == 0)
            return &cfg->peers[i];
    }
    if (cfg->peer_count >= ADN_BRIDGE_PEER_MAX)
        return NULL;
    i = cfg->peer_count++;
    memset(&cfg->peers[i], 0, sizeof(cfg->peers[i]));
    set_str(cfg->peers[i].name, sizeof(cfg->peers[i].name), peer_name);
    cfg->peers[i].enabled = 1;
    return &cfg->peers[i];
}

static void apply_peer_key(adn_bridge_config_t *cfg, const char *peer_name,
                           const char *key, const char *val)
{
    adn_bridge_peer_t *p = find_or_add_peer(cfg, peer_name);
    int t;

    if (!p)
        return;
    if (strcmp(key, "type") == 0) {
        t = parse_peer_type(val);
        if (t >= 0)
            p->type = (adn_bridge_peer_type_t)t;
    } else if (strcmp(key, "enabled") == 0) {
        set_bool01(&p->enabled, val);
    }
}

static void apply_key(adn_bridge_config_t *cfg, const char *section, const char *key, const char *val)
{
    if (!section || !key)
        return;

    if (strncmp(section, "peer.", 5) == 0) {
        apply_peer_key(cfg, section + 5, key, val);
        return;
    }

    if (strcmp(section, "bridge") == 0) {
        if (strcmp(key, "mode") == 0)
            cfg->mode = parse_mode(val);
        return;
    }
    if (strcmp(section, "ysf") == 0) {
        if (strcmp(key, "host") == 0)
            set_str(cfg->ysf_host, sizeof(cfg->ysf_host), val);
        else if (strcmp(key, "port") == 0)
            set_int(&cfg->ysf_port, val);
        else if (strcmp(key, "dgid") == 0)
            set_int(&cfg->dgid, val);
        else if (strcmp(key, "log_level") == 0 || strcmp(key, "log") == 0)
            cfg->ysf_log_level = (int)log_level_from_string(val);
        return;
    }
    if (strcmp(section, "dmr") == 0) {
        if (strcmp(key, "callsign") == 0 || strcmp(key, "dmrid") == 0
            || strcmp(key, "description") == 0 || strcmp(key, "location") == 0) {
            apply_identity_key(cfg, key, val);
            return;
        }
        if (strcmp(key, "host") == 0)
            set_str(cfg->dmr_host, sizeof(cfg->dmr_host), val);
        else if (strcmp(key, "port") == 0)
            set_int(&cfg->dmr_port, val);
        else if (strcmp(key, "options") == 0)
            set_str(cfg->dmr_options, sizeof(cfg->dmr_options), val);
        else if (strcmp(key, "tg") == 0)
            set_int(&cfg->dmr_tg, val);
        else if (strcmp(key, "clear_dynamic_tg") == 0)
            set_bool01(&cfg->dmr_clear_dynamic_tg, val);
        else if (strcmp(key, "password") == 0 || strcmp(key, "passphrase") == 0)
            set_str(cfg->dmr_password, sizeof(cfg->dmr_password), val);
        else if (strcmp(key, "default_ysf_dmrid") == 0)
            set_int(&cfg->default_ysf_dmrid, val);
        else if (strcmp(key, "log_level") == 0 || strcmp(key, "log") == 0)
            cfg->dmr_log_level = (int)log_level_from_string(val);
        return;
    }
    if (strcmp(section, "echolink") == 0) {
        if (strcmp(key, "callsign") == 0)
            set_str(cfg->echolink.callsign, sizeof(cfg->echolink.callsign), val);
        else if (strcmp(key, "password") == 0)
            set_str(cfg->echolink.password, sizeof(cfg->echolink.password), val);
        else if (strcmp(key, "bind_addr") == 0)
            set_str(cfg->echolink.bind_addr, sizeof(cfg->echolink.bind_addr), val);
        else if (strcmp(key, "host") == 0)
            set_str(cfg->echolink.host, sizeof(cfg->echolink.host), val);
        else if (strcmp(key, "qth") == 0)
            set_str(cfg->echolink.qth, sizeof(cfg->echolink.qth), val);
        else if (strcmp(key, "email") == 0)
            set_str(cfg->echolink.email, sizeof(cfg->echolink.email), val);
        else if (strcmp(key, "directory_servers") == 0)
            parse_directory_servers(&cfg->echolink, val);
        else if (strcmp(key, "login_interval") == 0
                 || strcmp(key, "LoginInterval") == 0)
            set_int(&cfg->echolink.login_interval, val);
        else if (strcmp(key, "station_list_interval") == 0
                 || strcmp(key, "StationListInterval") == 0)
            set_int(&cfg->echolink.station_list_interval, val);
        else if (strcmp(key, "gain") == 0)
            set_float(&cfg->echolink.gain, val);
        else if (strcmp(key, "proxy_server") == 0
                 || strcmp(key, "PROXY_SERVER") == 0)
            set_str(cfg->echolink.proxy_server, sizeof(cfg->echolink.proxy_server),
                    val);
        else if (strcmp(key, "proxy_port") == 0
                 || strcmp(key, "PROXY_PORT") == 0)
            set_int(&cfg->echolink.proxy_port, val);
        else if (strcmp(key, "proxy_password") == 0
                 || strcmp(key, "PROXY_PASSWORD") == 0)
            set_str(cfg->echolink.proxy_password,
                    sizeof(cfg->echolink.proxy_password), val);
        else if (strcmp(key, "log_level") == 0 || strcmp(key, "log") == 0)
            cfg->echolink.log_level = (int)log_level_from_string(val);
        return;
    }
    if (strcmp(section, "vocoder") == 0) {
        if (strcmp(key, "host") == 0)
            set_str(cfg->vocoder.host, sizeof(cfg->vocoder.host), val);
        else if (strcmp(key, "port") == 0)
            set_int(&cfg->vocoder.port, val);
        else if (strcmp(key, "log_level") == 0 || strcmp(key, "log") == 0)
            cfg->vocoder.log_level = (int)log_level_from_string(val);
        return;
    }
    if (strcmp(section, "aliases") == 0) {
        if (strcmp(key, "try_download") == 0)
            ; /* legacy key ignored — downloads are always enabled */
        else if (strcmp(key, "stale_minutes") == 0)
            set_int(&cfg->aliases.stale_minutes, val);
        else if (strcmp(key, "stale_days") == 0) {
            set_int(&cfg->aliases.stale_minutes, val);
            cfg->aliases.stale_minutes *= 24 * 60;
        }
        else if (strcmp(key, "reload_minutes") == 0)
            set_int(&cfg->aliases.reload_minutes, val);
        else if (strcmp(key, "data_dir") == 0)
            set_str(cfg->aliases.data_dir, sizeof(cfg->aliases.data_dir), val);
        else if (strcmp(key, "subscriber_file") == 0)
            set_str(cfg->aliases.subscriber_file, sizeof(cfg->aliases.subscriber_file), val);
        else if (strcmp(key, "subscriber_url") == 0)
            set_str(cfg->aliases.subscriber_url, sizeof(cfg->aliases.subscriber_url), val);
        else if (strcmp(key, "local_subscriber_file") == 0)
            set_str(cfg->aliases.local_subscriber_file, sizeof(cfg->aliases.local_subscriber_file), val);
        else if (strcmp(key, "checksum_file") == 0)
            set_str(cfg->aliases.checksum_file, sizeof(cfg->aliases.checksum_file), val);
        else if (strcmp(key, "checksum_url") == 0)
            set_str(cfg->aliases.checksum_url, sizeof(cfg->aliases.checksum_url), val);
        return;
    }
    if (strcmp(section, "log") == 0) {
        /* Default for all channels; per-stanza log_level= overrides after load. */
        if (strcmp(key, "level") == 0)
            cfg->log_level = log_level_from_string(val);
        else if (strcmp(key, "echolink") == 0 || strcmp(key, "el") == 0)
            cfg->echolink.log_level = (int)log_level_from_string(val);
        else if (strcmp(key, "dmr") == 0)
            cfg->dmr_log_level = (int)log_level_from_string(val);
        else if (strcmp(key, "ysf") == 0)
            cfg->ysf_log_level = (int)log_level_from_string(val);
        else if (strcmp(key, "vocoder") == 0 || strcmp(key, "voc") == 0)
            cfg->vocoder.log_level = (int)log_level_from_string(val);
        return;
    }
}

static void add_synth_peer(adn_bridge_config_t *cfg, const char *name,
                           adn_bridge_peer_type_t type)
{
    adn_bridge_peer_t *p = find_or_add_peer(cfg, name);

    if (!p)
        return;
    p->type = type;
    p->enabled = 1;
}

void adn_bridge_config_synthesize_peers(adn_bridge_config_t *cfg)
{
    if (!cfg || cfg->peer_count > 0)
        return;

    switch (cfg->mode) {
    case ADN_BRIDGE_MODE_ECHOLINK_DMR:
        add_synth_peer(cfg, "echolink", ADN_BRIDGE_PEER_TYPE_ECHOLINK);
        add_synth_peer(cfg, "dmr", ADN_BRIDGE_PEER_TYPE_DMR);
        break;
    case ADN_BRIDGE_MODE_ECHOLINK_YSF:
        add_synth_peer(cfg, "echolink", ADN_BRIDGE_PEER_TYPE_ECHOLINK);
        add_synth_peer(cfg, "ysf", ADN_BRIDGE_PEER_TYPE_YSF);
        break;
    default:
        add_synth_peer(cfg, "ysf", ADN_BRIDGE_PEER_TYPE_YSF);
        add_synth_peer(cfg, "dmr", ADN_BRIDGE_PEER_TYPE_DMR);
        break;
    }
}

int adn_bridge_config_enabled_peer_count(const adn_bridge_config_t *cfg)
{
    int i, n = 0;

    if (!cfg)
        return 0;
    for (i = 0; i < cfg->peer_count; i++) {
        if (cfg->peers[i].enabled)
            n++;
    }
    return n;
}

/* Apply [log] level + per-stanza overrides to runtime channels. */
void adn_bridge_config_apply_log_levels(const adn_bridge_config_t *cfg)
{
    log_level_t def = cfg->log_level;

    log_set_channel_level(LOG_CH_APP, def);
    log_set_channel_level(LOG_CH_ECHOLINK,
                          cfg->echolink.log_level >= 0
                              ? (log_level_t)cfg->echolink.log_level : def);
    log_set_channel_level(LOG_CH_DMR,
                          cfg->dmr_log_level >= 0
                              ? (log_level_t)cfg->dmr_log_level : def);
    log_set_channel_level(LOG_CH_YSF,
                          cfg->ysf_log_level >= 0
                              ? (log_level_t)cfg->ysf_log_level : def);
    log_set_channel_level(LOG_CH_VOCODER,
                          cfg->vocoder.log_level >= 0
                              ? (log_level_t)cfg->vocoder.log_level : def);
}

int adn_bridge_config_load(const char *path, adn_bridge_config_t *cfg, char *err, size_t errlen)
{
    FILE *fp;
    char line[512];
    char section[32] = "";
    int lineno = 0;

    adn_bridge_config_init(cfg);

    fp = fopen(path, "r");
    if (!fp) {
        snprintf(err, errlen, "cannot open config: %s", path);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *key, *val, *eq;

        lineno++;
        p = trim(p);
        if (*p == 0 || *p == '#' || *p == ';')
            continue;
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (!end) {
                snprintf(err, errlen, "%s:%d: malformed section", path, lineno);
                fclose(fp);
                return -1;
            }
            *end = '\0';
            strncpy(section, p + 1, sizeof(section) - 1);
            section[sizeof(section) - 1] = '\0';
            continue;
        }
        eq = strchr(p, '=');
        if (!eq) {
            snprintf(err, errlen, "%s:%d: expected key=value", path, lineno);
            fclose(fp);
            return -1;
        }
        *eq = '\0';
        key = trim(p);
        val = trim(eq + 1);
        apply_key(cfg, section, key, val);
    }

    fclose(fp);

    if (strcmp(cfg->dmr_options, "\"\"") == 0)
        cfg->dmr_options[0] = '\0';

    /* Default directory servers if EL mode and none configured */
    if ((cfg->mode == ADN_BRIDGE_MODE_ECHOLINK_DMR || cfg->mode == ADN_BRIDGE_MODE_ECHOLINK_YSF)
        && cfg->echolink.directory_server_count == 0) {
        static const char *defs[] = {
            "server1.echolink.org", "server2.echolink.org",
            "server3.echolink.org", "server4.echolink.org"
        };
        int i;
        for (i = 0; i < 4; i++)
            set_str(cfg->echolink.directory_servers[i],
                    sizeof(cfg->echolink.directory_servers[i]), defs[i]);
        cfg->echolink.directory_server_count = 4;
    }

    /* EchoLink Proxy defaults */
    if (cfg->echolink.proxy_server[0]) {
        if (cfg->echolink.proxy_port <= 0)
            cfg->echolink.proxy_port = 8100;
        if (!cfg->echolink.proxy_password[0])
            set_str(cfg->echolink.proxy_password,
                    sizeof(cfg->echolink.proxy_password), "PUBLIC");
    }

    adn_bridge_config_synthesize_peers(cfg);

    return 0;
}

int adn_bridge_config_valid(const adn_bridge_config_t *cfg, char *err, size_t errlen)
{
    int el = (cfg->mode == ADN_BRIDGE_MODE_ECHOLINK_DMR
              || cfg->mode == ADN_BRIDGE_MODE_ECHOLINK_YSF);
    int need_dmr = (cfg->mode == ADN_BRIDGE_MODE_YSF_DMR
                    || cfg->mode == ADN_BRIDGE_MODE_ECHOLINK_DMR);

    /* echolink-ysf: no DMR peer — [dmr] stanza is optional (omit entirely). */
    if (need_dmr) {
        if (!cfg->callsign[0]) {
            snprintf(err, errlen, "missing [dmr] callsign");
            return -1;
        }
        if (cfg->dmrid <= 0) {
            snprintf(err, errlen, "missing [dmr] dmrid");
            return -1;
        }
        if (!cfg->dmr_password[0]) {
            snprintf(err, errlen, "missing [dmr] password (or passphrase)");
            return -1;
        }
        if (!cfg->dmr_host[0] || cfg->dmr_port <= 0) {
            snprintf(err, errlen, "missing [dmr] host/port");
            return -1;
        }
        if (cfg->dmr_tg <= 0) {
            snprintf(err, errlen, "missing [dmr] tg");
            return -1;
        }
    }

    if (cfg->mode == ADN_BRIDGE_MODE_YSF_DMR || cfg->mode == ADN_BRIDGE_MODE_ECHOLINK_YSF) {
        if (!cfg->ysf_host[0] || cfg->ysf_port <= 0) {
            snprintf(err, errlen, "missing [ysf] host/port");
            return -1;
        }
        if (cfg->dgid < 0 || cfg->dgid > 99) {
            snprintf(err, errlen, "invalid [ysf] dgid (0-99)");
            return -1;
        }
    }

    if (el) {
        if (!cfg->echolink.callsign[0]) {
            snprintf(err, errlen, "missing [echolink] callsign");
            return -1;
        }
        if (!cfg->echolink.password[0]) {
            snprintf(err, errlen, "missing [echolink] password");
            return -1;
        }
        if (cfg->echolink.proxy_server[0]) {
            /* Proxy mode: bind_addr not required (defaults applied in load). */
            if (cfg->echolink.proxy_port <= 0) {
                snprintf(err, errlen, "invalid [echolink] proxy_port");
                return -1;
            }
        } else if (!cfg->echolink.bind_addr[0]) {
            snprintf(err, errlen, "missing [echolink] bind_addr");
            return -1;
        }
        /* 1.0 = unity … up to 4.0 max; must be > 0 (mute not supported). */
        if (cfg->echolink.gain <= 0.0f || cfg->echolink.gain > 4.0f) {
            snprintf(err, errlen, "invalid [echolink] gain (use 0 < gain <= 4)");
            return -1;
        }
        if (!cfg->vocoder.host[0] || cfg->vocoder.port <= 0) {
            snprintf(err, errlen, "missing [vocoder] host/port");
            return -1;
        }
    }

    if (adn_bridge_config_enabled_peer_count(cfg) < 2) {
        snprintf(err, errlen, "need at least two enabled [peer.*] entries");
        return -1;
    }
    return 0;
}
