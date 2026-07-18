/*
 * ysf2dmrcon — YSF/EchoLink <-> DMR voice bridge.
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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#include "bridge.h"
#include "bridge_el.h"
#include "config.h"
#include "log.h"
#include "aliases.h"
#include "peer_dmr.h"
#include "peer_ysf.h"
#include "peer_echolink.h"
#include "talker_alias.h"
#include "vocoder.h"

#define YSF2DMR_VERSION "0.3.0"

static ysf2dmr_bridge_t bridge;
static bridge_el_t bridge_el;
static ysf2dmr_aliases_t *g_aliases;
static volatile sig_atomic_t keep_running = 1;
/* Only set flags in the handler — never sendto/log (unsafe with blocking EL dir TCP). */
static volatile sig_atomic_t alarm_pending = 0;

static void on_signal(int sig)
{
    if (sig == SIGINT)
        keep_running = 0;
    if (sig == SIGALRM) {
        alarm_pending = 1;
        alarm(5);
    }
}

static void service_alarm(void)
{
    if (!alarm_pending)
        return;
    alarm_pending = 0;
    if (bridge.dmr.sock >= 0)
        peer_dmr_on_alarm(&bridge.dmr);
    if (bridge.ysf.sock >= 0)
        peer_ysf_on_alarm(&bridge.ysf);
    if (bridge_el.dmr.sock >= 0)
        peer_dmr_on_alarm(&bridge_el.dmr);
    if (bridge_el.ysf.sock >= 0)
        peer_ysf_on_alarm(&bridge_el.ysf);
}

static void print_credits(FILE *out)
{
    fprintf(out, "ysf2dmrcon %s — YSF/EchoLink <-> DMR voice bridge\n", YSF2DMR_VERSION);
    fprintf(out, "Copyright (C) 2026 Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>\n");
    fprintf(out, "License: GPL-3.0-or-later (see LICENSE)\n");
}

static void usage(const char *prog)
{
    print_credits(stderr);
    fprintf(stderr,
            "\nUsage:\n"
            "  %s                 load ysf2dmrcon.ini (cwd or next to binary)\n"
            "  %s -c config.ini\n"
            "  %s config.ini\n"
            "  %s -h | --help     show this help\n"
            "  %s -v | --version  print version\n"
            "\n"
            "INI [bridge] mode=ysf-dmr|echolink-dmr|echolink-ysf — see examples\n",
            prog, prog, prog, prog, prog);
}

static int parse_args(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 1;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("ysf2dmrcon %s\n", YSF2DMR_VERSION);
            return 1;
        }
    }
    return 0;
}

static int resolve_config(int argc, char **argv, ysf2dmr_config_t *cfg, char *err, size_t errlen)
{
    char default_ini[PATH_MAX];
    const char *ini_path = NULL;

    if (argc == 1) {
        if (ysf2dmr_config_default_path(argv[0], default_ini, sizeof(default_ini)) != 0) {
            snprintf(err, errlen, "cannot find ysf2dmrcon.ini (cwd or next to binary)");
            return -1;
        }
        ini_path = default_ini;
    } else if (strcmp(argv[1], "-c") == 0) {
        if (argc < 3) {
            snprintf(err, errlen, "missing path after -c");
            return -1;
        }
        ini_path = argv[2];
    } else if (argc == 2) {
        ini_path = argv[1];
    } else {
        snprintf(err, errlen, "invalid arguments");
        return -1;
    }

    if (ysf2dmr_config_load(ini_path, cfg, err, errlen) != 0)
        return -1;
    return 0;
}

static int run_ysf_dmr(ysf2dmr_config_t *cfg)
{
    time_t last_alias_poll = time(NULL);

    bridge_init(&bridge, cfg->dmr_options, g_aliases, cfg->default_ysf_dmrid,
                cfg->dmr_clear_dynamic_tg);

    if (peer_ysf_open(&bridge.ysf, cfg->ysf_host, cfg->ysf_port, cfg->callsign, (uint8_t)cfg->dgid) < 0)
        return 1;
    if (peer_dmr_open(&bridge.dmr, cfg->dmr_host, cfg->dmr_port, cfg->callsign,
                      cfg->dmrid, cfg->dmr_tg, cfg->dmr_options,
                      cfg->dmr_password,
                      cfg->description, cfg->location) < 0)
        return 1;

    LOG_INFO("bridge running (YSF<->DMR voice via ModeConv)\n");

    while (keep_running) {
        int from_dmr = 0, from_ysf = 0, len;
        time_t now;

        service_alarm();
        peer_dmr_tick(&bridge.dmr);
        peer_ysf_tick(&bridge.ysf);

        now = time(NULL);
        if ((cfg->aliases.stale_minutes > 0 || cfg->aliases.reload_minutes > 0)
            && now - last_alias_poll >= 60) {
            last_alias_poll = now;
            if (ysf2dmr_aliases_maybe_refresh(&cfg->aliases, &g_aliases) > 0)
                bridge.aliases = g_aliases;
        }

        len = peer_dmr_poll(&bridge.dmr, 5, &from_dmr);
        if (from_dmr && len > 0) {
            if (len == 55 && memcmp(bridge.dmr.buf, "DMRD", 4) == 0)
                bridge_on_dmrd(&bridge, bridge.dmr.buf, len);
            else if (len == DMRA_PACKET_LEN && memcmp(bridge.dmr.buf, "DMRA", 4) == 0)
                bridge_on_dmra(&bridge, bridge.dmr.buf, len);
        }

        len = peer_ysf_poll(&bridge.ysf, 5, &from_ysf);
        if (from_ysf && len > 0) {
            if (len == 155)
                bridge_on_ysfd(&bridge, bridge.ysf.buf, len);
        }

        bridge_tick(&bridge);
    }

    peer_dmr_on_sigint(&bridge.dmr);
    peer_ysf_on_sigint(&bridge.ysf);
    return 0;
}

static int run_echolink(ysf2dmr_config_t *cfg)
{
    time_t last_alias_poll = time(NULL);
    int use_dmr = (cfg->mode == YSF2DMR_MODE_ECHOLINK_DMR);
    int use_ysf = (cfg->mode == YSF2DMR_MODE_ECHOLINK_YSF);

    bridge_el_init(&bridge_el, cfg->mode, cfg->dmr_options, g_aliases, cfg->dmrid,
                   cfg->echolink.gain, cfg->dmr_clear_dynamic_tg);

    if (vocoder_open(&bridge_el.voc, cfg->vocoder.host, cfg->vocoder.port) < 0)
        return 1;
    if (peer_el_open(&bridge_el.el, &cfg->echolink) < 0)
        return 1;

    if (use_dmr) {
        if (peer_dmr_open(&bridge_el.dmr, cfg->dmr_host, cfg->dmr_port, cfg->callsign,
                          cfg->dmrid, cfg->dmr_tg, cfg->dmr_options,
                          cfg->dmr_password,
                          cfg->description, cfg->location) < 0)
            return 1;
        LOG_INFO("bridge running (EchoLink<->DMR via vocoder %s:%d)\n",
                 cfg->vocoder.host, cfg->vocoder.port);
    }
    if (use_ysf) {
        char ysf_cs[10];

        /* Gateway YSFP callsign: full [echolink] callsign (incl. -L/-R). */
        bridge_el_format_callsign10(ysf_cs, cfg->echolink.callsign);
        if (peer_ysf_open(&bridge_el.ysf, cfg->ysf_host, cfg->ysf_port, ysf_cs,
                          (uint8_t)cfg->dgid) < 0)
            return 1;
        LOG_INFO("bridge running (EchoLink<->YSF via vocoder %s:%d)\n",
                 cfg->vocoder.host, cfg->vocoder.port);
    }

    while (keep_running) {
        int from_dmr = 0, from_ysf = 0, len;
        time_t now;

        service_alarm();
        peer_el_tick(&bridge_el.el);
        if (use_dmr)
            peer_dmr_tick(&bridge_el.dmr);
        if (use_ysf)
            peer_ysf_tick(&bridge_el.ysf);

        now = time(NULL);
        if ((cfg->aliases.stale_minutes > 0 || cfg->aliases.reload_minutes > 0)
            && now - last_alias_poll >= 60) {
            last_alias_poll = now;
            if (ysf2dmr_aliases_maybe_refresh(&cfg->aliases, &g_aliases) > 0)
                bridge_el.aliases = g_aliases;
        }

        peer_el_poll(&bridge_el.el, 5);
        if (use_dmr)
            bridge_el_process_el_audio(&bridge_el);
        else
            bridge_el_process_el_to_ysf(&bridge_el);

        if (use_dmr) {
            len = peer_dmr_poll(&bridge_el.dmr, 5, &from_dmr);
            if (from_dmr && len == 55 && memcmp(bridge_el.dmr.buf, "DMRD", 4) == 0)
                bridge_el_on_dmrd(&bridge_el, bridge_el.dmr.buf, len);
        }
        if (use_ysf) {
            len = peer_ysf_poll(&bridge_el.ysf, 5, &from_ysf);
            if (from_ysf && len == 155)
                bridge_el_on_ysfd(&bridge_el, bridge_el.ysf.buf, len);
        }
        bridge_el_tick(&bridge_el);
    }

    peer_el_on_sigint(&bridge_el.el);
    if (use_dmr)
        peer_dmr_on_sigint(&bridge_el.dmr);
    if (use_ysf)
        peer_ysf_on_sigint(&bridge_el.ysf);
    vocoder_close(&bridge_el.voc);
    return 0;
}

int main(int argc, char **argv)
{
    ysf2dmr_config_t cfg;
    char err[256];
    int rc;

    memset(&bridge, 0, sizeof(bridge));
    memset(&bridge_el, 0, sizeof(bridge_el));
    bridge.dmr.sock = -1;
    bridge.ysf.sock = -1;
    bridge_el.dmr.sock = -1;
    bridge_el.ysf.sock = -1;
    bridge_el.el.rtp_sock = -1;

    log_set_level(LOG_LEVEL_INFO);

    if (parse_args(argc, argv) != 0)
        return 0;

    if (resolve_config(argc, argv, &cfg, err, sizeof(err)) != 0) {
        if (err[0])
            LOG_ERROR("%s\n", err);
        usage(argv[0]);
        return 1;
    }
    if (ysf2dmr_config_valid(&cfg, err, sizeof(err)) != 0) {
        LOG_ERROR("%s\n", err);
        return 1;
    }

    ysf2dmr_config_apply_log_levels(&cfg);

    print_credits(stdout);
    printf("\n");
    printf("Mode: %s\n", ysf2dmr_mode_name(cfg.mode));
    printf("Identity: %s (%d)\n", cfg.callsign, cfg.dmrid);
    if (cfg.description[0])
        printf("Description: %s\n", cfg.description);
    if (cfg.location[0])
        printf("Location: %s\n", cfg.location);
    if (cfg.mode == YSF2DMR_MODE_YSF_DMR || cfg.mode == YSF2DMR_MODE_ECHOLINK_YSF)
        printf("YSF: %s:%d DGID %d\n", cfg.ysf_host, cfg.ysf_port, cfg.dgid);
    if (cfg.mode == YSF2DMR_MODE_YSF_DMR || cfg.mode == YSF2DMR_MODE_ECHOLINK_DMR)
        printf("DMR: %s:%d voice TG %d OPTIONS=%s (%s)\n",
               cfg.dmr_host, cfg.dmr_port, cfg.dmr_tg, cfg.dmr_options, cfg.callsign);
    if (cfg.mode != YSF2DMR_MODE_YSF_DMR) {
        printf("EchoLink: %s bind %s", cfg.echolink.callsign, cfg.echolink.bind_addr);
        if (cfg.echolink.host[0])
            printf(" host %s", cfg.echolink.host);
        if (cfg.echolink.gain != 1.0f)
            printf(" gain %.3f", (double)cfg.echolink.gain);
        printf("\n");
        printf("Vocoder: %s:%d\n", cfg.vocoder.host, cfg.vocoder.port);
    }

    LOG_INFO("log levels app=%s el=%s dmr=%s ysf=%s voc=%s\n",
             log_level_name(log_get_channel_level(LOG_CH_APP)),
             log_level_name(log_get_channel_level(LOG_CH_ECHOLINK)),
             log_level_name(log_get_channel_level(LOG_CH_DMR)),
             log_level_name(log_get_channel_level(LOG_CH_YSF)),
             log_level_name(log_get_channel_level(LOG_CH_VOCODER)));

    if (ysf2dmr_aliases_load(&cfg.aliases, &g_aliases) != 0)
        LOG_WARNING("alias load failed; talker lookup may be limited\n");

    signal(SIGINT, on_signal);
    signal(SIGALRM, on_signal);
    alarm(5);

    if (cfg.mode == YSF2DMR_MODE_YSF_DMR)
        rc = run_ysf_dmr(&cfg);
    else
        rc = run_echolink(&cfg);

    LOG_INFO("shutting down ysf2dmrcon\n");
    alarm(0);
    bridge.aliases = NULL;
    bridge_el.aliases = NULL;
    ysf2dmr_aliases_free(g_aliases);
    g_aliases = NULL;

    return rc;
}
