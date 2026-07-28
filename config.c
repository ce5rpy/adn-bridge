/*
 * INI configuration loader for adn-bridge ([peer.*] bus model).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"
#include "log.h"
#include "media/codec_plan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void adn_bridge_config_init(adn_bridge_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->log_level = LOG_LEVEL_INFO;
    adn_bridge_aliases_cfg_init(&cfg->aliases);
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

static int adn_bridge_config_count_peers(const adn_bridge_config_t *cfg,
                                         adn_bridge_peer_type_t type, int enabled_only)
{
    int i, n = 0;

    if (!cfg)
        return 0;
    for (i = 0; i < cfg->peer_count; i++) {
        if (!cfg->peers[i].type_set || cfg->peers[i].type != type)
            continue;
        if (enabled_only && !cfg->peers[i].enabled)
            continue;
        n++;
    }
    return n;
}

const adn_bridge_peer_t *adn_bridge_config_find_peer(const adn_bridge_config_t *cfg,
                                                       adn_bridge_peer_type_t type)
{
    int i;

    if (!cfg)
        return NULL;
    for (i = 0; i < cfg->peer_count; i++) {
        if (cfg->peers[i].enabled && cfg->peers[i].type_set
            && cfg->peers[i].type == type)
            return &cfg->peers[i];
    }
    return NULL;
}

const char *adn_bridge_layout_name(const adn_bridge_config_t *cfg)
{
    static char buf[64];
    int n_dmr, n_ysf, n_el;
    size_t off = 0;

    if (!cfg)
        return "unknown";
    n_dmr = adn_bridge_config_count_peers(cfg, ADN_BRIDGE_PEER_TYPE_DMR, 1);
    n_ysf = adn_bridge_config_count_peers(cfg, ADN_BRIDGE_PEER_TYPE_YSF, 1);
    n_el = adn_bridge_config_count_peers(cfg, ADN_BRIDGE_PEER_TYPE_ECHOLINK, 1);
    buf[0] = '\0';
    if (n_dmr)
        off += snprintf(buf + off, sizeof(buf) - off, "%dx dmr", n_dmr);
    if (n_ysf)
        off += snprintf(buf + off, sizeof(buf) - off, "%s%dx ysf",
                         off ? " + " : "", n_ysf);
    if (n_el)
        off += snprintf(buf + off, sizeof(buf) - off, "%s%dx echolink",
                         off ? " + " : "", n_el);
    if (!off)
        return "no peers";
    return buf;
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
    size_t n;

    if (!val || !*val || !dst || dstlen == 0)
        return;
    n = strlen(val);
    if (n >= dstlen)
        n = dstlen - 1;
    memcpy(dst, val, n);
    dst[n] = '\0';
}

static void set_int(int *dst, const char *val)
{
    if (!val || !*val)
        return;
    *dst = atoi(val);
}

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

/* CSV tokens: console, console-timed, file, file-timed, null. Combinable
 * (e.g. "console,file-timed"); unknown tokens warn once and are ignored. */
static void parse_log_handlers(adn_bridge_log_output_t *lo, const char *val)
{
    char buf[128];
    char *tok, *save = NULL;

    if (!val || !*val)
        return;
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    lo->handlers_set = 1;
    lo->console = 0;
    lo->console_timed = 0;
    lo->file = 0;
    lo->file_timed = 0;

    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *t = trim(tok);
        size_t i;

        for (i = 0; t[i]; i++)
            t[i] = (char)tolower((unsigned char)t[i]);

        if (strcmp(t, "console") == 0) {
            lo->console = 1;
        } else if (strcmp(t, "console-timed") == 0) {
            lo->console = 1;
            lo->console_timed = 1;
        } else if (strcmp(t, "file") == 0) {
            lo->file = 1;
        } else if (strcmp(t, "file-timed") == 0) {
            lo->file = 1;
            lo->file_timed = 1;
        } else if (strcmp(t, "null") == 0) {
            /* explicit all-off — already zeroed above */
        } else if (t[0]) {
            LOG_WARNING("[log] unknown handlers token '%s', ignoring\n", t);
        }
    }
}

static void parse_directory_servers(adn_bridge_peer_el_t *el, const char *val)
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

static void parse_callsign_list(const char *val, char list[][16], int *count)
{
    char buf[256];
    char *tok, *save = NULL;
    int n = 0;

    if (!val || !*val)
        return;
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (tok = strtok_r(buf, ", \t", &save); tok && n < ADN_BRIDGE_EL_ALLOW_MAX;
         tok = strtok_r(NULL, ", \t", &save)) {
        set_str(list[n], 16, tok);
        n++;
    }
    *count = n;
}

/* Literal "\n" (backslash + 'n', two INI characters) becomes a real EchoLink
 * line separator ('\r') so welcome_text can be multi-line in a single INI
 * value. Any other backslash escape is left as-is (copied verbatim). */
static void parse_welcome_text(char *dst, size_t dstlen, const char *val)
{
    size_t o = 0;

    if (!val || !dst || dstlen == 0)
        return;
    for (; *val && o + 1 < dstlen; val++) {
        if (val[0] == '\\' && val[1] == 'n') {
            dst[o++] = '\r';
            val++;
        } else {
            dst[o++] = *val;
        }
    }
    dst[o] = '\0';
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
    cfg->peers[i].u.dmr.log_level = -1;
    cfg->peers[i].u.ysf.log_level = -1;
    cfg->peers[i].u.el.log_level = -1;
    cfg->peers[i].u.el.vocoder_log_level = -1;
    cfg->peers[i].u.el.vocoder_port = 2460;
    cfg->peers[i].u.el.login_interval = 360;
    cfg->peers[i].u.el.station_list_interval = 600;
    cfg->peers[i].u.el.gain = 1.0f;
    cfg->peers[i].u.el.max_inbound = 1;
    return &cfg->peers[i];
}

static int is_vocoder_peer_key(const char *key)
{
    return strcmp(key, "vocoder_host") == 0 || strcmp(key, "vocoder_port") == 0
           || strcmp(key, "vocoder_log_level") == 0 || strcmp(key, "vocoder_log") == 0
           || strcmp(key, "vocoder") == 0;
}

static void apply_peer_dmr_key(adn_bridge_peer_dmr_t *d, const char *key, const char *val)
{
    if (strcmp(key, "host") == 0)
        set_str(d->host, sizeof(d->host), val);
    else if (strcmp(key, "port") == 0)
        set_int(&d->port, val);
    else if (strcmp(key, "callsign") == 0)
        set_str(d->callsign, sizeof(d->callsign), val);
    else if (strcmp(key, "dmrid") == 0)
        set_int(&d->dmrid, val);
    else if (strcmp(key, "description") == 0)
        set_str(d->description, sizeof(d->description), val);
    else if (strcmp(key, "location") == 0)
        set_str(d->location, sizeof(d->location), val);
    else if (strcmp(key, "options") == 0)
        set_str(d->options, sizeof(d->options), val);
    else if (strcmp(key, "password") == 0 || strcmp(key, "passphrase") == 0)
        set_str(d->password, sizeof(d->password), val);
    else if (strcmp(key, "tg") == 0)
        set_int(&d->tg, val);
    else if (strcmp(key, "clear_dynamic_tg") == 0)
        set_bool01(&d->clear_dynamic_tg, val);
    else if (strcmp(key, "log_level") == 0 || strcmp(key, "log") == 0)
        d->log_level = (int)log_level_from_string(val);
}

static void apply_peer_ysf_key(adn_bridge_peer_ysf_t *y, const char *key, const char *val)
{
    if (strcmp(key, "host") == 0)
        set_str(y->host, sizeof(y->host), val);
    else if (strcmp(key, "port") == 0)
        set_int(&y->port, val);
    else if (strcmp(key, "callsign") == 0)
        set_str(y->callsign, sizeof(y->callsign), val);
    else if (strcmp(key, "dgid") == 0)
        set_int(&y->dgid, val);
    else if (strcmp(key, "log_level") == 0 || strcmp(key, "log") == 0)
        y->log_level = (int)log_level_from_string(val);
}

static void apply_peer_el_key(adn_bridge_peer_el_t *el, const char *key, const char *val)
{
    if (strcmp(key, "callsign") == 0)
        set_str(el->callsign, sizeof(el->callsign), val);
    else if (strcmp(key, "password") == 0)
        set_str(el->password, sizeof(el->password), val);
    else if (strcmp(key, "bind_addr") == 0)
        set_str(el->bind_addr, sizeof(el->bind_addr), val);
    else if (strcmp(key, "host") == 0)
        set_str(el->host, sizeof(el->host), val);
    else if (strcmp(key, "qth") == 0)
        set_str(el->qth, sizeof(el->qth), val);
    else if (strcmp(key, "email") == 0)
        set_str(el->email, sizeof(el->email), val);
    else if (strcmp(key, "directory_servers") == 0)
        parse_directory_servers(el, val);
    else if (strcmp(key, "max_inbound") == 0)
        set_int(&el->max_inbound, val);
    else if (strcmp(key, "allowed_callsigns") == 0)
        parse_callsign_list(val, el->allowed_callsigns, &el->allowed_callsign_count);
    else if (strcmp(key, "blocked_callsigns") == 0)
        parse_callsign_list(val, el->blocked_callsigns, &el->blocked_callsign_count);
    else if (strcmp(key, "welcome_text") == 0)
        parse_welcome_text(el->welcome_text, sizeof(el->welcome_text), val);
    else if (strcmp(key, "login_interval") == 0 || strcmp(key, "LoginInterval") == 0)
        set_int(&el->login_interval, val);
    else if (strcmp(key, "station_list_interval") == 0
             || strcmp(key, "StationListInterval") == 0)
        set_int(&el->station_list_interval, val);
    else if (strcmp(key, "gain") == 0)
        set_float(&el->gain, val);
    else if (strcmp(key, "proxy_server") == 0 || strcmp(key, "PROXY_SERVER") == 0)
        set_str(el->proxy_server, sizeof(el->proxy_server), val);
    else if (strcmp(key, "proxy_port") == 0 || strcmp(key, "PROXY_PORT") == 0)
        set_int(&el->proxy_port, val);
    else if (strcmp(key, "proxy_password") == 0 || strcmp(key, "PROXY_PASSWORD") == 0)
        set_str(el->proxy_password, sizeof(el->proxy_password), val);
    else if (strcmp(key, "vocoder_host") == 0)
        set_str(el->vocoder_host, sizeof(el->vocoder_host), val);
    else if (strcmp(key, "vocoder_port") == 0)
        set_int(&el->vocoder_port, val);
    else if (strcmp(key, "vocoder_log_level") == 0 || strcmp(key, "vocoder_log") == 0)
        el->vocoder_log_level = (int)log_level_from_string(val);
    else if (strcmp(key, "log_level") == 0 || strcmp(key, "log") == 0)
        el->log_level = (int)log_level_from_string(val);
}

static int apply_peer_key(adn_bridge_config_t *cfg, const char *peer_name,
                          const char *key, const char *val, const char *path,
                          int lineno, char *err, size_t errlen)
{
    adn_bridge_peer_t *p = find_or_add_peer(cfg, peer_name);
    int t;

    if (!p)
        return 0;
    if (strcmp(key, "type") == 0) {
        t = parse_peer_type(val);
        if (t >= 0) {
            p->type = (adn_bridge_peer_type_t)t;
            p->type_set = 1;
        }
        return 0;
    }
    if (strcmp(key, "enabled") == 0) {
        set_bool01(&p->enabled, val);
        return 0;
    }
    if (is_vocoder_peer_key(key)) {
        if (!p->type_set) {
            snprintf(err, errlen, "%s:%d: [peer.%s] set type before %s",
                     path, lineno, peer_name, key);
            return -1;
        }
        if (p->type != ADN_BRIDGE_PEER_TYPE_ECHOLINK) {
            snprintf(err, errlen,
                     "%s:%d: [peer.%s] %s only allowed on echolink peers",
                     path, lineno, peer_name, key);
            return -1;
        }
        if (strcmp(key, "vocoder") == 0) {
            snprintf(err, errlen,
                     "%s:%d: [peer.%s] use vocoder_host/vocoder_port (not vocoder=)",
                     path, lineno, peer_name);
            return -1;
        }
        apply_peer_el_key(&p->u.el, key, val);
        return 0;
    }
    if (!p->type_set)
        return 0;
    if (p->type == ADN_BRIDGE_PEER_TYPE_DMR)
        apply_peer_dmr_key(&p->u.dmr, key, val);
    else if (p->type == ADN_BRIDGE_PEER_TYPE_YSF)
        apply_peer_ysf_key(&p->u.ysf, key, val);
    else if (p->type == ADN_BRIDGE_PEER_TYPE_ECHOLINK)
        apply_peer_el_key(&p->u.el, key, val);
    return 0;
}

static int apply_key(adn_bridge_config_t *cfg, const char *section, const char *key,
                     const char *val, const char *path, int lineno, char *err,
                     size_t errlen)
{
    if (!section || !key)
        return 0;

    if (strncmp(section, "peer.", 5) == 0) {
        return apply_peer_key(cfg, section + 5, key, val, path, lineno, err,
                              errlen);
    }
    if (strncmp(section, "vocoder", 7) == 0) {
        snprintf(err, errlen,
                 "%s:%d: set vocoder_host/vocoder_port under [peer.*] (not [vocoder])",
                 path, lineno);
        return -1;
    }
    if (strcmp(section, "aliases") == 0) {
        if (strcmp(key, "try_download") == 0)
            ;
        else if (strcmp(key, "stale_minutes") == 0)
            set_int(&cfg->aliases.stale_minutes, val);
        else if (strcmp(key, "stale_days") == 0) {
            set_int(&cfg->aliases.stale_minutes, val);
            cfg->aliases.stale_minutes *= 24 * 60;
        } else if (strcmp(key, "reload_minutes") == 0)
            set_int(&cfg->aliases.reload_minutes, val);
        else if (strcmp(key, "data_dir") == 0)
            set_str(cfg->aliases.data_dir, sizeof(cfg->aliases.data_dir), val);
        else if (strcmp(key, "subscriber_file") == 0)
            set_str(cfg->aliases.subscriber_file, sizeof(cfg->aliases.subscriber_file), val);
        else if (strcmp(key, "subscriber_url") == 0)
            set_str(cfg->aliases.subscriber_url, sizeof(cfg->aliases.subscriber_url), val);
        else if (strcmp(key, "local_subscriber_file") == 0)
            set_str(cfg->aliases.local_subscriber_file,
                    sizeof(cfg->aliases.local_subscriber_file), val);
        else if (strcmp(key, "checksum_file") == 0)
            set_str(cfg->aliases.checksum_file, sizeof(cfg->aliases.checksum_file), val);
        else if (strcmp(key, "checksum_url") == 0)
            set_str(cfg->aliases.checksum_url, sizeof(cfg->aliases.checksum_url), val);
        return 0;
    }
    if (strcmp(section, "log") == 0) {
        if (strcmp(key, "level") == 0)
            cfg->log_level = log_level_from_string(val);
        else if (strcmp(key, "handlers") == 0)
            parse_log_handlers(&cfg->log_output, val);
        else if (strcmp(key, "file") == 0)
            set_str(cfg->log_output.file_path, sizeof(cfg->log_output.file_path), val);
        return 0;
    }
    return 0;
}

static void finalize_el_peer(adn_bridge_peer_el_t *el)
{
    int i;

    if (el->directory_server_count == 0) {
        static const char *defs[] = {
            "server1.echolink.org", "server2.echolink.org",
            "server3.echolink.org", "server4.echolink.org"
        };
        for (i = 0; i < 4; i++)
            set_str(el->directory_servers[i], sizeof(el->directory_servers[i]), defs[i]);
        el->directory_server_count = 4;
    }
    if (el->proxy_server[0]) {
        if (el->proxy_port <= 0)
            el->proxy_port = 8100;
        if (!el->proxy_password[0])
            set_str(el->proxy_password, sizeof(el->proxy_password), "PUBLIC");
    }
}

static void finalize_peers(adn_bridge_config_t *cfg)
{
    int i;

    for (i = 0; i < cfg->peer_count; i++) {
        if (cfg->peers[i].type_set && cfg->peers[i].type == ADN_BRIDGE_PEER_TYPE_ECHOLINK)
            finalize_el_peer(&cfg->peers[i].u.el);
        if (cfg->peers[i].type_set && cfg->peers[i].type == ADN_BRIDGE_PEER_TYPE_DMR
            && strcmp(cfg->peers[i].u.dmr.options, "\"\"") == 0)
            cfg->peers[i].u.dmr.options[0] = '\0';
    }
}

void adn_bridge_config_apply_log_levels(const adn_bridge_config_t *cfg)
{
    log_level_t def = cfg->log_level;
    int i;

    log_set_channel_level(LOG_CH_APP, def);
    log_set_channel_level(LOG_CH_VOCODER, def);
    log_set_channel_level(LOG_CH_DMR, def);
    log_set_channel_level(LOG_CH_YSF, def);
    log_set_channel_level(LOG_CH_ECHOLINK, def);

    for (i = 0; i < cfg->peer_count; i++) {
        const adn_bridge_peer_t *p = &cfg->peers[i];
        log_level_t lv = def;

        if (!p->enabled || !p->type_set)
            continue;
        if (p->type == ADN_BRIDGE_PEER_TYPE_ECHOLINK && p->u.el.vocoder_log_level >= 0)
            log_set_channel_level(LOG_CH_VOCODER,
                                  (log_level_t)p->u.el.vocoder_log_level);
        if (p->type == ADN_BRIDGE_PEER_TYPE_DMR && p->u.dmr.log_level >= 0)
            lv = (log_level_t)p->u.dmr.log_level;
        else if (p->type == ADN_BRIDGE_PEER_TYPE_YSF && p->u.ysf.log_level >= 0)
            lv = (log_level_t)p->u.ysf.log_level;
        else if (p->type == ADN_BRIDGE_PEER_TYPE_ECHOLINK && p->u.el.log_level >= 0)
            lv = (log_level_t)p->u.el.log_level;

        if (p->type == ADN_BRIDGE_PEER_TYPE_DMR)
            log_set_channel_level(LOG_CH_DMR, lv);
        else if (p->type == ADN_BRIDGE_PEER_TYPE_YSF)
            log_set_channel_level(LOG_CH_YSF, lv);
        else if (p->type == ADN_BRIDGE_PEER_TYPE_ECHOLINK)
            log_set_channel_level(LOG_CH_ECHOLINK, lv);
    }
}

void adn_bridge_config_apply_log_output(const adn_bridge_config_t *cfg)
{
    const adn_bridge_log_output_t *lo = &cfg->log_output;
    log_output_cfg_t out;

    memset(&out, 0, sizeof(out));
    if (lo->handlers_set) {
        out.console = lo->console;
        out.console_timed = lo->console_timed;
        out.file = lo->file;
        out.file_timed = lo->file_timed;
    } else {
        out.console = 1;
        out.console_timed = log_auto_detect_console_timed();
    }
    if (out.file && !lo->file_path[0]) {
        LOG_ERROR("[log] file handler requested but file= not set; disabling file sink\n");
        out.file = 0;
    } else if (out.file) {
        set_str(out.file_path, sizeof(out.file_path), lo->file_path);
    }
    log_init(&out);
}

int adn_bridge_config_load(const char *path, adn_bridge_config_t *cfg, char *err, size_t errlen)
{
    FILE *fp;
    char line[512];
    char section[64] = "";
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
        if (apply_key(cfg, section, key, val, path, lineno, err, errlen) != 0) {
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    finalize_peers(cfg);
    return 0;
}

static int validate_peer_dmr(const adn_bridge_peer_t *p, char *err, size_t errlen)
{
    const adn_bridge_peer_dmr_t *d = &p->u.dmr;

    if (!d->callsign[0]) {
        snprintf(err, errlen, "[peer.%s] missing callsign", p->name);
        return -1;
    }
    if (d->dmrid <= 0) {
        snprintf(err, errlen, "[peer.%s] missing dmrid", p->name);
        return -1;
    }
    if (!d->password[0]) {
        snprintf(err, errlen, "[peer.%s] missing password", p->name);
        return -1;
    }
    if (!d->host[0] || d->port <= 0) {
        snprintf(err, errlen, "[peer.%s] missing host/port", p->name);
        return -1;
    }
    if (d->tg <= 0) {
        snprintf(err, errlen, "[peer.%s] missing tg", p->name);
        return -1;
    }
    return 0;
}

static int validate_peer_ysf(const adn_bridge_peer_t *p, char *err, size_t errlen)
{
    const adn_bridge_peer_ysf_t *y = &p->u.ysf;

    if (!y->host[0] || y->port <= 0) {
        snprintf(err, errlen, "[peer.%s] missing host/port", p->name);
        return -1;
    }
    if (y->dgid < 0 || y->dgid > 99) {
        snprintf(err, errlen, "[peer.%s] invalid dgid (0-99)", p->name);
        return -1;
    }
    if (!y->callsign[0]) {
        snprintf(err, errlen, "[peer.%s] missing callsign", p->name);
        return -1;
    }
    return 0;
}

static int validate_peer_el(const adn_bridge_peer_t *p, char *err, size_t errlen)
{
    const adn_bridge_peer_el_t *el = &p->u.el;

    if (!el->callsign[0]) {
        snprintf(err, errlen, "[peer.%s] missing callsign", p->name);
        return -1;
    }
    if (!el->password[0]) {
        snprintf(err, errlen, "[peer.%s] missing password", p->name);
        return -1;
    }
    if (el->proxy_server[0]) {
        if (el->proxy_port <= 0) {
            snprintf(err, errlen, "[peer.%s] invalid proxy_port", p->name);
            return -1;
        }
    } else if (!el->bind_addr[0]) {
        snprintf(err, errlen, "[peer.%s] missing bind_addr", p->name);
        return -1;
    }
    if (el->gain <= 0.0f || el->gain > 4.0f) {
        snprintf(err, errlen, "[peer.%s] invalid gain (0 < gain <= 4)", p->name);
        return -1;
    }
    if (el->max_inbound < 0) {
        snprintf(err, errlen, "[peer.%s] invalid max_inbound (must be >= 0)", p->name);
        return -1;
    }
    return 0;
}

static int validate_el_vocoder_when_needed(const adn_bridge_config_t *cfg,
                                            char *err, size_t errlen)
{
    media_codec_plan_t plan;
    int i;

    if (media_codec_plan_from_config(cfg, &plan) != 0 || !plan.needs_vocoder)
        return 0;

    for (i = 0; i < cfg->peer_count; i++) {
        const adn_bridge_peer_t *p = &cfg->peers[i];
        const adn_bridge_peer_el_t *el;

        if (!p->enabled || p->type != ADN_BRIDGE_PEER_TYPE_ECHOLINK)
            continue;
        el = &p->u.el;
        if (!el->vocoder_host[0]) {
            snprintf(err, errlen,
                     "[peer.%s] missing vocoder_host (PCM bridge required for this layout)",
                     p->name);
            return -1;
        }
        if (el->vocoder_port <= 0) {
            snprintf(err, errlen, "[peer.%s] missing vocoder_port", p->name);
            return -1;
        }
    }
    return 0;
}

int adn_bridge_config_valid(const adn_bridge_config_t *cfg, char *err, size_t errlen)
{
    int i;

    if (!cfg->peer_count) {
        snprintf(err, errlen, "no [peer.*] stanzas defined");
        return -1;
    }
    if (adn_bridge_config_enabled_peer_count(cfg) < 2) {
        snprintf(err, errlen, "need at least two enabled [peer.*] entries");
        return -1;
    }

    for (i = 0; i < cfg->peer_count; i++) {
        const adn_bridge_peer_t *p = &cfg->peers[i];

        if (!p->enabled)
            continue;
        if (!p->type_set) {
            snprintf(err, errlen, "[peer.%s] missing type", p->name);
            return -1;
        }
        if (p->type == ADN_BRIDGE_PEER_TYPE_DMR && validate_peer_dmr(p, err, errlen) != 0)
            return -1;
        if (p->type == ADN_BRIDGE_PEER_TYPE_YSF && validate_peer_ysf(p, err, errlen) != 0)
            return -1;
        if (p->type == ADN_BRIDGE_PEER_TYPE_ECHOLINK && validate_peer_el(p, err, errlen) != 0)
            return -1;
    }

    if (validate_el_vocoder_when_needed(cfg, err, errlen) != 0)
        return -1;

    return 0;
}
