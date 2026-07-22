/*
 * adn-bridge — YSF/EchoLink <-> DMR voice bridge.
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

#include "bridge.h"
#include "bridge_el.h"
#include "config.h"
#include "engine.h"
#include "log.h"
#include "aliases.h"
#include "peer_dmr.h"
#include "peer_ysf.h"
#include "peer_echolink.h"
#include "talker_alias.h"
#include "vocoder.h"

#define ADN_BRIDGE_VERSION "0.3.1"

static adn_bridge_t bridge;
static bridge_el_t bridge_el;
static adn_bridge_aliases_t *g_aliases;
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
    fprintf(out, "adn-bridge %s — YSF/EchoLink <-> DMR voice bridge\n", ADN_BRIDGE_VERSION);
    fprintf(out, "Copyright (C) 2026 Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>\n");
    fprintf(out, "License: GPL-3.0-or-later (see LICENSE)\n");
}

static void usage(const char *prog)
{
    print_credits(stderr);
    fprintf(stderr,
            "\nUsage:\n"
            "  %s -c config.ini\n"
            "  %s config.ini\n"
            "  %s -h | --help     show this help\n"
            "  %s -v | --version  print version\n"
            "\n"
            "INI [bridge] mode=ysf-dmr|echolink-dmr|echolink-ysf\n"
            "  templates: examples/*.example.ini → copy to config/\n",
            prog, prog, prog, prog);
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
            printf("adn-bridge %s\n", ADN_BRIDGE_VERSION);
            return 1;
        }
    }
    return 0;
}

static int resolve_config(int argc, char **argv, adn_bridge_config_t *cfg, char *err, size_t errlen)
{
    const char *ini_path = NULL;

    if (argc == 1) {
        err[0] = '\0';
        return -1; /* main prints usage */
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

    if (adn_bridge_config_load(ini_path, cfg, err, errlen) != 0)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    adn_bridge_config_t cfg;
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

    if (argc < 2) {
        usage(argv[0]);
        return 0;
    }

    if (parse_args(argc, argv) != 0)
        return 0;

    if (resolve_config(argc, argv, &cfg, err, sizeof(err)) != 0) {
        if (err[0])
            LOG_ERROR("%s\n", err);
        usage(argv[0]);
        return 1;
    }
    if (adn_bridge_config_valid(&cfg, err, sizeof(err)) != 0) {
        LOG_ERROR("%s\n", err);
        return 1;
    }

    adn_bridge_config_apply_log_levels(&cfg);

    print_credits(stdout);
    printf("\n");
    printf("Mode: %s\n", adn_bridge_mode_name(cfg.mode));
    if (cfg.mode == ADN_BRIDGE_MODE_ECHOLINK_YSF) {
        printf("Identity: %s (EchoLink; no DMR peer)\n", cfg.echolink.callsign);
    } else {
        printf("Identity: %s (%d)\n", cfg.callsign, cfg.dmrid);
        if (cfg.description[0])
            printf("Description: %s\n", cfg.description);
        if (cfg.location[0])
            printf("Location: %s\n", cfg.location);
    }
    if (cfg.mode == ADN_BRIDGE_MODE_YSF_DMR || cfg.mode == ADN_BRIDGE_MODE_ECHOLINK_YSF)
        printf("YSF: %s:%d DGID %d\n", cfg.ysf_host, cfg.ysf_port, cfg.dgid);
    if (cfg.mode == ADN_BRIDGE_MODE_YSF_DMR || cfg.mode == ADN_BRIDGE_MODE_ECHOLINK_DMR)
        printf("DMR: %s:%d voice TG %d OPTIONS=%s (%s)\n",
               cfg.dmr_host, cfg.dmr_port, cfg.dmr_tg, cfg.dmr_options, cfg.callsign);
    if (cfg.mode != ADN_BRIDGE_MODE_YSF_DMR) {
        printf("EchoLink: %s bind %s", cfg.echolink.callsign, cfg.echolink.bind_addr);
        if (cfg.echolink.host[0])
            printf(" host %s", cfg.echolink.host);
        if (cfg.echolink.gain != 1.0f)
            printf(" gain %.3f", (double)cfg.echolink.gain);
        printf("\n");
        printf("Vocoder: %s:%d\n", cfg.vocoder.host, cfg.vocoder.port);
    }
    if (cfg.peer_count > 0) {
        int i;
        printf("Peers (%d enabled / %d):\n",
               adn_bridge_config_enabled_peer_count(&cfg), cfg.peer_count);
        for (i = 0; i < cfg.peer_count; i++) {
            const char *type = "?";
            if (cfg.peers[i].type == ADN_BRIDGE_PEER_TYPE_DMR)
                type = "dmr";
            else if (cfg.peers[i].type == ADN_BRIDGE_PEER_TYPE_YSF)
                type = "ysf";
            else if (cfg.peers[i].type == ADN_BRIDGE_PEER_TYPE_ECHOLINK)
                type = "echolink";
            printf("  %s type=%s %s\n", cfg.peers[i].name, type,
                   cfg.peers[i].enabled ? "enabled" : "disabled");
        }
    }

    LOG_INFO("log levels app=%s el=%s dmr=%s ysf=%s voc=%s\n",
             log_level_name(log_get_channel_level(LOG_CH_APP)),
             log_level_name(log_get_channel_level(LOG_CH_ECHOLINK)),
             log_level_name(log_get_channel_level(LOG_CH_DMR)),
             log_level_name(log_get_channel_level(LOG_CH_YSF)),
             log_level_name(log_get_channel_level(LOG_CH_VOCODER)));

    if (adn_bridge_aliases_load(&cfg.aliases, &g_aliases) != 0)
        LOG_WARNING("alias load failed; talker lookup may be limited\n");

    signal(SIGINT, on_signal);
    signal(SIGALRM, on_signal);
    alarm(5);

    {
        engine_host_t host = {
            .keep_running = &keep_running,
            .service_alarm = service_alarm,
            .aliases = &g_aliases,
        };

        rc = engine_run(&host, &cfg, &bridge, &bridge_el);
    }

    LOG_INFO("shutting down adn-bridge\n");
    alarm(0);
    bridge.aliases = NULL;
    bridge_el.aliases = NULL;
    adn_bridge_aliases_free(g_aliases);
    g_aliases = NULL;

    return rc;
}
