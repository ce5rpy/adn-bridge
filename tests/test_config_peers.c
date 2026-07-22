/*
 * Unit tests for [peer.*] config and legacy synthesis.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <stdio.h>
#include <string.h>

static int expect_synth(int mode, int want_count, int want_types[])
{
    adn_bridge_config_t cfg;
    int i;

    adn_bridge_config_init(&cfg);
    cfg.mode = mode;
    adn_bridge_config_synthesize_peers(&cfg);
    if (cfg.peer_count != want_count)
        return 1;
    for (i = 0; i < want_count; i++) {
        if (cfg.peers[i].type != want_types[i] || !cfg.peers[i].enabled)
            return 2 + i;
    }
    if (adn_bridge_config_enabled_peer_count(&cfg) != want_count)
        return 10;
    return 0;
}

static int test_explicit_peers(void)
{
    const char *ini =
        "[bridge]\n"
        "mode = ysf-dmr\n"
        "[peer.fusion]\n"
        "type = ysf\n"
        "enabled = true\n"
        "[peer.master]\n"
        "type = dmr\n"
        "enabled = true\n"
        "[dmr]\n"
        "callsign = N0CALL\n"
        "dmrid = 1\n"
        "host = m.example\n"
        "port = 62031\n"
        "tg = 1\n"
        "password = x\n"
        "[ysf]\n"
        "host = y.example\n"
        "port = 42000\n"
        "dgid = 0\n";
    FILE *fp;
    adn_bridge_config_t cfg;
    char err[128];

    fp = fopen("/tmp/adn-test-peers.ini", "w");
    if (!fp)
        return 20;
    fputs(ini, fp);
    fclose(fp);

    if (adn_bridge_config_load("/tmp/adn-test-peers.ini", &cfg, err, sizeof(err)) != 0)
        return 21;
    if (cfg.peer_count != 2)
        return 22;
    if (strcmp(cfg.peers[0].name, "fusion") != 0
        || cfg.peers[0].type != ADN_BRIDGE_PEER_TYPE_YSF)
        return 23;
    if (strcmp(cfg.peers[1].name, "master") != 0
        || cfg.peers[1].type != ADN_BRIDGE_PEER_TYPE_DMR)
        return 24;
    if (adn_bridge_config_valid(&cfg, err, sizeof(err)) != 0)
        return 25;
    return 0;
}

int main(void)
{
    int ysf_dmr[] = { ADN_BRIDGE_PEER_TYPE_YSF, ADN_BRIDGE_PEER_TYPE_DMR };
    int el_dmr[] = { ADN_BRIDGE_PEER_TYPE_ECHOLINK, ADN_BRIDGE_PEER_TYPE_DMR };
    int el_ysf[] = { ADN_BRIDGE_PEER_TYPE_ECHOLINK, ADN_BRIDGE_PEER_TYPE_YSF };
    int rc;

    rc = expect_synth(ADN_BRIDGE_MODE_YSF_DMR, 2, ysf_dmr);
    if (rc != 0) {
        fprintf(stderr, "test_config_peers: ysf-dmr synth failed (%d)\n", rc);
        return 1;
    }
    rc = expect_synth(ADN_BRIDGE_MODE_ECHOLINK_DMR, 2, el_dmr);
    if (rc != 0) {
        fprintf(stderr, "test_config_peers: el-dmr synth failed (%d)\n", rc);
        return 2;
    }
    rc = expect_synth(ADN_BRIDGE_MODE_ECHOLINK_YSF, 2, el_ysf);
    if (rc != 0) {
        fprintf(stderr, "test_config_peers: el-ysf synth failed (%d)\n", rc);
        return 3;
    }
    rc = test_explicit_peers();
    if (rc != 0) {
        fprintf(stderr, "test_config_peers: explicit peers failed (%d)\n", rc);
        return 4;
    }

    printf("test_config_peers: ok\n");
    return 0;
}
