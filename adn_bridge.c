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

#include "config.h"
#include "engine.h"
#include "log.h"
#include "aliases.h"
#include "media/core.h"
#include "peer_dmr.h"
#include "peer_ysf.h"
#include "peer_echolink.h"
#include "talker_alias.h"
#include "vocoder.h"

#define ADN_BRIDGE_VERSION "0.3.1"

static media_core_t core;
static adn_bridge_aliases_t *g_aliases;
static volatile sig_atomic_t keep_running = 1;
/* Only set flags in the handler — never sendto/log (unsafe with blocking EL dir TCP). */
static volatile sig_atomic_t alarm_pending = 0;
/* SIGUSR2: logrotate-style reopen of the file sink, no restart (no INI
 * reload — SIGHUP is left free for that, unlike adn-server's use of SIGHUP
 * for config reload). */
static volatile sig_atomic_t log_reopen_pending = 0;

static void on_signal(int sig)
{
    if (sig == SIGINT)
        keep_running = 0;
    if (sig == SIGALRM) {
        alarm_pending = 1;
        alarm(5);
    }
    if (sig == SIGUSR2)
        log_reopen_pending = 1;
}

static void service_alarm(void)
{
    if (log_reopen_pending) {
        log_reopen_pending = 0;
        log_reopen_files();
        LOG_INFO("log: file sink reopened (SIGUSR2)\n");
    }
    if (!alarm_pending)
        return;
    alarm_pending = 0;
    engine_service_peer_alarms();
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
            "INI: [peer.*] stanzas, any mix of dmr/ysf/echolink (>=2 enabled)\n"
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

static const char *peer_type_label(adn_bridge_peer_type_t type)
{
    switch (type) {
    case ADN_BRIDGE_PEER_TYPE_DMR:
        return "dmr";
    case ADN_BRIDGE_PEER_TYPE_YSF:
        return "ysf";
    case ADN_BRIDGE_PEER_TYPE_ECHOLINK:
        return "echolink";
    default:
        return "?";
    }
}

static const char *layout_label(const adn_bridge_config_t *cfg)
{
    return adn_bridge_layout_name(cfg);
}

static void print_peer_banner(const adn_bridge_peer_t *p)
{
    if (!p->enabled || !p->type_set)
        return;

    printf("  [peer.%s] type=%s\n", p->name, peer_type_label(p->type));
    if (p->type == ADN_BRIDGE_PEER_TYPE_DMR) {
        const adn_bridge_peer_dmr_t *d = &p->u.dmr;
        printf("    DMR %s:%d %s (%d) TG %d\n",
               d->host, d->port, d->callsign, d->dmrid, d->tg);
    } else if (p->type == ADN_BRIDGE_PEER_TYPE_YSF) {
        const adn_bridge_peer_ysf_t *y = &p->u.ysf;
        printf("    YSF %s:%d %s DGID %d\n",
               y->host, y->port, y->callsign, y->dgid);
    } else if (p->type == ADN_BRIDGE_PEER_TYPE_ECHOLINK) {
        const adn_bridge_peer_el_t *el = &p->u.el;
        printf("    EchoLink %s", el->callsign);
        if (el->host[0])
            printf(" -> %s", el->host);
        if (el->vocoder_host[0])
            printf(" vocoder %s:%d", el->vocoder_host, el->vocoder_port);
        if (el->proxy_server[0])
            printf(" proxy %s:%d", el->proxy_server, el->proxy_port);
        else if (el->bind_addr[0])
            printf(" bind %s", el->bind_addr);
        if (el->gain != 1.0f)
            printf(" gain %.3f", (double)el->gain);
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    adn_bridge_config_t cfg;
    char err[256];
    int rc;

    memset(&core, 0, sizeof(core));

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
    adn_bridge_config_apply_log_output(&cfg);

    print_credits(stdout);
    printf("\n");
    printf("Layout: %s\n", layout_label(&cfg));
    printf("Peers (%d enabled / %d):\n",
           adn_bridge_config_enabled_peer_count(&cfg), cfg.peer_count);
    {
        int i;
        for (i = 0; i < cfg.peer_count; i++)
            print_peer_banner(&cfg.peers[i]);
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
    signal(SIGUSR2, on_signal);
    alarm(5);

    {
        engine_host_t host = {
            .keep_running = &keep_running,
            .service_alarm = service_alarm,
            .aliases = &g_aliases,
        };

        rc = engine_run(&host, &cfg, &core);
    }

    LOG_INFO("shutting down adn-bridge\n");
    alarm(0);
    core.aliases = NULL;
    adn_bridge_aliases_free(g_aliases);
    g_aliases = NULL;

    return rc;
}
