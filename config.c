/*
 * INI configuration loader for ysf2dmrcon.
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

#define YSF_RADIO_ID_DEFAULT "FT-5D"

typedef struct {
    const char *name;
    const char rid[6];
} ysf_radio_model_t;

static const ysf_radio_model_t ysf_radio_models[] = {
    { "FT-70D",  "FT-70" },
    { "FT-3D",   "FT-3D" },
    { "FT-991",  "FT991" },
    { "FTM500",  "FTM50" },
    { "FTM-500", "FTM50" },
    { "FTM400",  "FTM40" },
    { "FTM-400", "FTM40" },
    { "FTM300",  "FTM30" },
    { "FTM-300", "FTM30" },
    { "FTM310",  "FTM31" },
    { "FTM-310", "FTM31" },
    { "FTM3200", "FTM32" },
    { "FTM-3200", "FTM32" },
    { "FT-1XD",  "FT-1X" },
    { "FT7250",  "FT725" },
    { "FT-2D",   "FT-2D" },
    { "FTM100",  "FTM10" },
    { "FTM-100", "FTM10" },
    { "FT-5D",   "FT-5D" },
    { "FT3207",  "FT320" },
    { "FTM200",  "FTM20" },
    { "FTM-200", "FTM20" },
    { NULL,      "" }
};

static int str_ieq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

void ysf2dmr_config_set_radio_id(ysf2dmr_config_t *cfg, const char *val)
{
    const ysf_radio_model_t *m;
    char compact[16];
    int i, j;

    if (!cfg)
        return;

    if (!val || !val[0] || strcmp(val, "*****") == 0) {
        memcpy(cfg->radio_id, YSF_RADIO_ID_DEFAULT, 6);
        return;
    }

    for (m = ysf_radio_models; m->name; m++) {
        if (str_ieq(val, m->name)) {
            memcpy(cfg->radio_id, m->rid, 6);
            return;
        }
    }

    if (strlen(val) <= 5) {
        memset(cfg->radio_id, 0, sizeof(cfg->radio_id));
        memcpy(cfg->radio_id, val, strlen(val));
        return;
    }

    /* Longer names: drop dashes and take the first 5 alnum chars. */
    memset(compact, 0, sizeof(compact));
    for (i = 0, j = 0; val[i] && j < 5; i++) {
        unsigned char c = (unsigned char)val[i];
        if (c == '-' || c == ' ')
            continue;
        compact[j++] = (char)toupper(c);
    }
    if (j == 0) {
        memcpy(cfg->radio_id, YSF_RADIO_ID_DEFAULT, 6);
        return;
    }
    memcpy(cfg->radio_id, compact, 6);
}

void ysf2dmr_config_init(ysf2dmr_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->log_level = LOG_LEVEL_INFO;
    cfg->default_ysf_dmrid = 0;
    ysf2dmr_config_set_radio_id(cfg, NULL);
    ysf2dmr_aliases_cfg_init(&cfg->aliases);
}

int ysf2dmr_config_default_path(const char *argv0, char *path, size_t pathlen)
{
    char exebuf[PATH_MAX];

    if (access("ysf2dmrcon.ini", R_OK) == 0) {
        strncpy(path, "ysf2dmrcon.ini", pathlen);
        path[pathlen - 1] = '\0';
        return 0;
    }

    if (argv0 && argv0[0]) {
        strncpy(exebuf, argv0, sizeof(exebuf) - 1);
        exebuf[sizeof(exebuf) - 1] = '\0';
        snprintf(path, pathlen, "%s/ysf2dmrcon.ini", dirname(exebuf));
        if (access(path, R_OK) == 0)
            return 0;
    }

    strncpy(path, "ysf2dmrcon.ini", pathlen);
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

static int tg_from_options(const char *options)
{
    static const char *keys[] = {"TS2=", "TS1=", NULL};
    int i;

    if (!options || !options[0])
        return 0;
    for (i = 0; keys[i]; i++) {
        const char *p = strstr(options, keys[i]);
        if (p) {
            int tg = atoi(p + strlen(keys[i]));
            if (tg > 0)
                return tg;
        }
    }
    return 0;
}

static void apply_identity_key(ysf2dmr_config_t *cfg, const char *key, const char *val)
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

static void apply_key(ysf2dmr_config_t *cfg, const char *section, const char *key, const char *val)
{
    if (!section || !key)
        return;

    if (strcmp(section, "ysf") == 0) {
        if (strcmp(key, "host") == 0)
            set_str(cfg->ysf_host, sizeof(cfg->ysf_host), val);
        else if (strcmp(key, "port") == 0)
            set_int(&cfg->ysf_port, val);
        else if (strcmp(key, "dgid") == 0)
            set_int(&cfg->dgid, val);
        else if (strcmp(key, "radio_id") == 0 || strcmp(key, "radio_model") == 0)
            ysf2dmr_config_set_radio_id(cfg, val);
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
        else if (strcmp(key, "password") == 0 || strcmp(key, "passphrase") == 0)
            set_str(cfg->dmr_password, sizeof(cfg->dmr_password), val);
        else if (strcmp(key, "default_ysf_dmrid") == 0)
            set_int(&cfg->default_ysf_dmrid, val);
        return;
    }
    if (strcmp(section, "aliases") == 0) {
        if (strcmp(key, "try_download") == 0)
            cfg->aliases.try_download = (atoi(val) != 0);
        else if (strcmp(key, "stale_minutes") == 0)
            set_int(&cfg->aliases.stale_minutes, val);
        else if (strcmp(key, "stale_days") == 0) {
            /* legacy key: days -> minutes */
            set_int(&cfg->aliases.stale_minutes, val);
            cfg->aliases.stale_minutes *= 24 * 60;
        }
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
        if (strcmp(key, "level") == 0)
            cfg->log_level = log_level_from_string(val);
        return;
    }
}

int ysf2dmr_config_load(const char *path, ysf2dmr_config_t *cfg, char *err, size_t errlen)
{
    FILE *fp;
    char line[512];
    char section[32] = "";
    int lineno = 0;

    ysf2dmr_config_init(cfg);

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

    cfg->dmr_tg = tg_from_options(cfg->dmr_options);

    return 0;
}

int ysf2dmr_config_valid(const ysf2dmr_config_t *cfg, char *err, size_t errlen)
{
    if (!cfg->callsign[0]) {
        snprintf(err, errlen, "missing [dmr] callsign");
        return -1;
    }
    if (cfg->dmrid <= 0) {
        snprintf(err, errlen, "missing [dmr] dmrid");
        return -1;
    }
    if (!cfg->ysf_host[0] || cfg->ysf_port <= 0) {
        snprintf(err, errlen, "missing [ysf] host/port");
        return -1;
    }
    if (!cfg->dmr_host[0] || cfg->dmr_port <= 0) {
        snprintf(err, errlen, "missing [dmr] host/port");
        return -1;
    }
    if (!cfg->dmr_options[0]) {
        snprintf(err, errlen, "missing [dmr] options");
        return -1;
    }
    if (cfg->dmr_tg <= 0) {
        snprintf(err, errlen, "[dmr] options must include TS1= or TS2= talkgroup");
        return -1;
    }
    if (!cfg->dmr_password[0]) {
        snprintf(err, errlen, "missing [dmr] password (or passphrase)");
        return -1;
    }
    if (cfg->dgid < 0 || cfg->dgid > 99) {
        snprintf(err, errlen, "invalid [ysf] dgid (0-99)");
        return -1;
    }
    return 0;
}
