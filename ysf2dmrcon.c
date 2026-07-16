/*
 * ysf2dmrcon — YSF reflector <-> DMR server voice bridge.
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
#include <unistd.h>
#include <limits.h>

#include "bridge.h"
#include "config.h"
#include "log.h"
#include "aliases.h"
#include "peer_dmr.h"
#include "talker_alias.h"

#define YSF2DMR_VERSION "0.0.1"

static ysf2dmr_bridge_t bridge;
static ysf2dmr_aliases_t *g_aliases;
static volatile sig_atomic_t keep_running = 1;

static void on_signal(int sig)
{
    if (sig == SIGINT)
        keep_running = 0;
    if (sig == SIGALRM) {
        peer_dmr_on_alarm(&bridge.dmr);
        peer_ysf_on_alarm(&bridge.ysf);
        alarm(5);
    }
}

static void print_credits(FILE *out)
{
    fprintf(out, "ysf2dmrcon %s — YSF <-> DMR voice bridge\n", YSF2DMR_VERSION);
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
            "INI sections [dmr], [ysf], [aliases], [log] — see ysf2dmrcon.example.ini\n",
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

int main(int argc, char **argv)
{
    ysf2dmr_config_t cfg;
    char err[256];

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

    log_set_level(cfg.log_level);

    print_credits(stdout);
    printf("\n");
    printf("Identity: %s (%d)\n", cfg.callsign, cfg.dmrid);
    if (cfg.description[0])
        printf("Description: %s\n", cfg.description);
    if (cfg.location[0])
        printf("Location: %s\n", cfg.location);
    printf("YSF: %s:%d DGID %d\n", cfg.ysf_host, cfg.ysf_port, cfg.dgid);
    printf("DMR: %s:%d voice TG %d OPTIONS=%s (%s)\n",
           cfg.dmr_host, cfg.dmr_port, cfg.dmr_tg, cfg.dmr_options, cfg.callsign);

    LOG_INFO("log level %s\n", log_level_name(log_get_level()));

    if (ysf2dmr_aliases_load(&cfg.aliases, &g_aliases) != 0)
        LOG_WARNING("alias load failed; YSF talker lookup may be limited\n");

    bridge_init(&bridge, cfg.dmr_options, g_aliases, cfg.default_ysf_dmrid);

    if (peer_ysf_open(&bridge.ysf, cfg.ysf_host, cfg.ysf_port, cfg.callsign, (uint8_t)cfg.dgid) < 0)
        return 1;
    if (peer_dmr_open(&bridge.dmr, cfg.dmr_host, cfg.dmr_port, cfg.callsign,
                      cfg.dmrid, cfg.dmr_tg, cfg.dmr_options,
                      cfg.dmr_password,
                      cfg.description, cfg.location) < 0)
        return 1;

    signal(SIGINT, on_signal);
    signal(SIGALRM, on_signal);
    alarm(5);

    LOG_INFO("bridge running (YSF<->DMR voice via ModeConv)\n");
    if (cfg.dmr_options[0])
        LOG_INFO("YSF DGID %d; DMR RPTO on login; connect PTT TG %d (1s)\n",
                 cfg.dgid, cfg.dmr_tg);
    else
        LOG_INFO("YSF DGID %d; DMR no RPTO; connect PTT TG %d (1s)\n",
                 cfg.dgid, cfg.dmr_tg);

    while (keep_running) {
        int from_dmr = 0, from_ysf = 0, len;

        peer_dmr_tick(&bridge.dmr);
        peer_ysf_tick(&bridge.ysf);

        /* Short poll timeouts: the voice pacing in bridge_tick() needs the
         * loop to spin every few ms; 50ms blocks starved DMR TX to ~75% of
         * real time and the hotspot jitter buffer underran (garbled audio). */
        len = peer_dmr_poll(&bridge.dmr, 5, &from_dmr);
        if (from_dmr && len > 0) {
            if (len == 55 && memcmp(bridge.dmr.buf, "DMRD", 4) == 0)
                bridge_on_dmrd(&bridge, bridge.dmr.buf, len);
            else if (len == DMRA_PACKET_LEN && memcmp(bridge.dmr.buf, "DMRA", 4) == 0)
                bridge_on_dmra(&bridge, bridge.dmr.buf, len);
            else
                LOG_DEBUG("DMR RX %d bytes (not DMRD): %.4s\n", len, bridge.dmr.buf);
        }

        len = peer_ysf_poll(&bridge.ysf, 5, &from_ysf);
        if (from_ysf && len > 0) {
            if (len == 155)
                bridge_on_ysfd(&bridge, bridge.ysf.buf, len);
            else
                LOG_DEBUG("YSF RX %d bytes (not YSFD): %.4s\n", len, bridge.ysf.buf);
        }

        bridge_tick(&bridge);
    }

    LOG_INFO("shutting down ysf2dmrcon\n");
    alarm(0);
    peer_dmr_on_sigint(&bridge.dmr);
    peer_ysf_on_sigint(&bridge.ysf);
    bridge.aliases = NULL;
    ysf2dmr_aliases_free(g_aliases);
    g_aliases = NULL;

    return 0;
}
