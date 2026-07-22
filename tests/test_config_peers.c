/*
 * Unit tests for [peer.*] configuration.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <stdio.h>
#include <string.h>

static int write_ini(const char *path, const char *body)
{
    FILE *fp = fopen(path, "w");

    if (!fp)
        return -1;
    fputs(body, fp);
    fclose(fp);
    return 0;
}

static int test_ysf_dmr_peers(void)
{
    const char *ini =
        "[peer.fusion]\n"
        "type = ysf\n"
        "enabled = true\n"
        "host = y.example\n"
        "port = 42000\n"
        "callsign = N0CALL\n"
        "dgid = 1\n"
        "[peer.master]\n"
        "type = dmr\n"
        "enabled = true\n"
        "callsign = N0CALL\n"
        "dmrid = 1234567\n"
        "host = m.example\n"
        "port = 62031\n"
        "tg = 1234\n"
        "password = secret\n"
        "options = TS2=1234;\n";
    adn_bridge_config_t cfg;
    char err[128];
    const adn_bridge_peer_t *dmr;
    const adn_bridge_peer_t *ysf;

    if (write_ini("/tmp/adn-test-peers.ini", ini) != 0)
        return 20;

    if (adn_bridge_config_load("/tmp/adn-test-peers.ini", &cfg, err, sizeof(err)) != 0)
        return 21;
    if (cfg.peer_count != 2)
        return 22;
    if (adn_bridge_config_valid(&cfg, err, sizeof(err)) != 0)
        return 23;
    if (adn_bridge_config_enabled_peer_count(&cfg) != 2)
        return 24;

    dmr = adn_bridge_config_find_peer(&cfg, ADN_BRIDGE_PEER_TYPE_DMR);
    ysf = adn_bridge_config_find_peer(&cfg, ADN_BRIDGE_PEER_TYPE_YSF);
    if (!dmr || !ysf)
        return 25;
    if (adn_bridge_config_layout(&cfg) != ADN_BRIDGE_LAYOUT_YSF_DMR)
        return 28;
    if (strcmp(adn_bridge_layout_name(ADN_BRIDGE_LAYOUT_YSF_DMR), "YSF <-> DMR") != 0)
        return 29;
    if (strcmp(dmr->name, "master") != 0 || dmr->u.dmr.tg != 1234)
        return 26;
    if (strcmp(ysf->name, "fusion") != 0 || ysf->u.ysf.dgid != 1)
        return 27;
    return 0;
}

static int test_reject_legacy_sections(void)
{
    const char *ini =
        "[bridge]\n"
        "mode = ysf-dmr\n"
        "[dmr]\n"
        "callsign = N0CALL\n"
        "dmrid = 1\n"
        "host = m.example\n"
        "port = 62031\n"
        "tg = 1\n"
        "password = x\n";
    adn_bridge_config_t cfg;
    char err[128];

    if (write_ini("/tmp/adn-test-legacy.ini", ini) != 0)
        return 30;
    if (adn_bridge_config_load("/tmp/adn-test-legacy.ini", &cfg, err, sizeof(err)) != 0)
        return 31;
    if (adn_bridge_config_valid(&cfg, err, sizeof(err)) == 0)
        return 32;
    return 0;
}

static int test_el_dmr_with_vocoder(void)
{
    const char *ini =
        "[peer.el]\n"
        "type = echolink\n"
        "enabled = true\n"
        "callsign = N0CALL-L\n"
        "password = secret\n"
        "bind_addr = 127.0.0.1\n"
        "vocoder_host = 127.0.0.1\n"
        "vocoder_port = 2460\n"
        "[peer.master]\n"
        "type = dmr\n"
        "enabled = true\n"
        "callsign = N0CALL\n"
        "dmrid = 1234567\n"
        "host = m.example\n"
        "port = 62031\n"
        "tg = 9\n"
        "password = secret\n";
    adn_bridge_config_t cfg;
    char err[128];
    const adn_bridge_peer_t *el;

    if (write_ini("/tmp/adn-test-el-dmr.ini", ini) != 0)
        return 40;
    if (adn_bridge_config_load("/tmp/adn-test-el-dmr.ini", &cfg, err, sizeof(err)) != 0)
        return 41;
    if (adn_bridge_config_valid(&cfg, err, sizeof(err)) != 0)
        return 42;
    el = adn_bridge_config_find_peer(&cfg, ADN_BRIDGE_PEER_TYPE_ECHOLINK);
    if (!el || strcmp(el->u.el.vocoder_host, "127.0.0.1") != 0
        || el->u.el.vocoder_port != 2460)
        return 43;
    if (!adn_bridge_config_find_peer(&cfg, ADN_BRIDGE_PEER_TYPE_DMR))
        return 44;
    return 0;
}

static int test_reject_single_peer(void)
{
    const char *ini =
        "[peer.master]\n"
        "type = dmr\n"
        "enabled = true\n"
        "callsign = N0CALL\n"
        "dmrid = 1\n"
        "host = m.example\n"
        "port = 62031\n"
        "tg = 1\n"
        "password = x\n";
    adn_bridge_config_t cfg;
    char err[128];

    if (write_ini("/tmp/adn-test-single.ini", ini) != 0)
        return 50;
    if (adn_bridge_config_load("/tmp/adn-test-single.ini", &cfg, err, sizeof(err)) != 0)
        return 51;
    if (adn_bridge_config_valid(&cfg, err, sizeof(err)) == 0)
        return 52;
    return 0;
}

static int test_reject_el_without_vocoder(void)
{
    const char *ini =
        "[peer.el]\n"
        "type = echolink\n"
        "enabled = true\n"
        "callsign = N0CALL-L\n"
        "password = secret\n"
        "bind_addr = 127.0.0.1\n"
        "[peer.master]\n"
        "type = dmr\n"
        "enabled = true\n"
        "callsign = N0CALL\n"
        "dmrid = 1\n"
        "host = m.example\n"
        "port = 62031\n"
        "tg = 1\n"
        "password = x\n";
    adn_bridge_config_t cfg;
    char err[256];

    if (write_ini("/tmp/adn-test-el-no-voc.ini", ini) != 0)
        return 60;
    if (adn_bridge_config_load("/tmp/adn-test-el-no-voc.ini", &cfg, err, sizeof(err)) != 0)
        return 61;
    if (adn_bridge_config_valid(&cfg, err, sizeof(err)) == 0)
        return 62;
    return 0;
}

static int test_reject_vocoder_section(void)
{
    const char *ini =
        "[peer.el]\n"
        "type = echolink\n"
        "enabled = true\n"
        "callsign = N0CALL-L\n"
        "password = secret\n"
        "bind_addr = 127.0.0.1\n"
        "vocoder_host = 127.0.0.1\n"
        "[peer.master]\n"
        "type = dmr\n"
        "enabled = true\n"
        "callsign = N0CALL\n"
        "dmrid = 1\n"
        "host = m.example\n"
        "port = 62031\n"
        "tg = 1\n"
        "password = x\n"
        "[vocoder.default]\n"
        "host = 127.0.0.1\n"
        "port = 2460\n";
    adn_bridge_config_t cfg;
    char err[256];

    if (write_ini("/tmp/adn-test-voc-section.ini", ini) != 0)
        return 70;
    if (adn_bridge_config_load("/tmp/adn-test-voc-section.ini", &cfg, err, sizeof(err)) == 0)
        return 71;
    return 0;
}

static int test_reject_vocoder_on_dmr_peer(void)
{
    const char *ini =
        "[peer.el]\n"
        "type = echolink\n"
        "enabled = true\n"
        "callsign = N0CALL-L\n"
        "password = secret\n"
        "bind_addr = 127.0.0.1\n"
        "vocoder_host = 127.0.0.1\n"
        "[peer.master]\n"
        "type = dmr\n"
        "enabled = true\n"
        "callsign = N0CALL\n"
        "dmrid = 1\n"
        "host = m.example\n"
        "port = 62031\n"
        "tg = 1\n"
        "password = x\n"
        "vocoder_host = 127.0.0.1\n";
    adn_bridge_config_t cfg;
    char err[256];

    if (write_ini("/tmp/adn-test-voc-dmr.ini", ini) != 0)
        return 80;
    if (adn_bridge_config_load("/tmp/adn-test-voc-dmr.ini", &cfg, err, sizeof(err)) == 0)
        return 81;
    return 0;
}

int main(void)
{
    int rc;

    rc = test_ysf_dmr_peers();
    if (rc != 0) {
        fprintf(stderr, "test_config_peers: ysf-dmr peers failed (%d)\n", rc);
        return 1;
    }
    rc = test_reject_legacy_sections();
    if (rc != 0) {
        fprintf(stderr, "test_config_peers: legacy reject failed (%d)\n", rc);
        return 2;
    }
    rc = test_el_dmr_with_vocoder();
    if (rc != 0) {
        fprintf(stderr, "test_config_peers: el+dmr valid failed (%d)\n", rc);
        return 3;
    }
    rc = test_reject_single_peer();
    if (rc != 0) {
        fprintf(stderr, "test_config_peers: single peer reject failed (%d)\n", rc);
        return 4;
    }
    rc = test_reject_el_without_vocoder();
    if (rc != 0) {
        fprintf(stderr, "test_config_peers: el without vocoder reject failed (%d)\n", rc);
        return 5;
    }
    rc = test_reject_vocoder_section();
    if (rc != 0) {
        fprintf(stderr, "test_config_peers: [vocoder] section reject failed (%d)\n", rc);
        return 6;
    }
    rc = test_reject_vocoder_on_dmr_peer();
    if (rc != 0) {
        fprintf(stderr, "test_config_peers: vocoder on dmr reject failed (%d)\n", rc);
        return 7;
    }

    printf("test_config_peers: ok\n");
    return 0;
}
