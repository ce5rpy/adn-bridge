/*
 * Unit tests for the local ALSA peer -- VOX state machine (pure function,
 * synthetic PCM, no hardware) plus a best-effort open()/write_pcm() smoke
 * test against ALSA's "null" plugin when built with WITH_ALSA (works without
 * a real sound card; see docs-priv/alsa-peer-fase0-1-scaffold-plan.md).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "peer_alsa.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

static void fill_pcm(int16_t *pcm, int n, int16_t amplitude)
{
    int i;

    for (i = 0; i < n; i++)
        pcm[i] = amplitude;
}

static void default_vox_cfg(adn_bridge_peer_alsa_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->vox_threshold = 500;
    cfg->vox_attack_ms = 50;
    cfg->vox_hang_ms = 200;
    cfg->tx_cooldown_ms = 300;
}

static int test_vox_fsm(void)
{
    adn_bridge_peer_alsa_t cfg;
    peer_alsa_vox_t vox;
    int16_t silence[ALSA_PCM_SAMPLES];
    int16_t loud[ALSA_PCM_SAMPLES];
    int checks = 0, fails = 0;
    int active;

    default_vox_cfg(&cfg);
    fill_pcm(silence, ALSA_PCM_SAMPLES, 0);
    fill_pcm(loud, ALSA_PCM_SAMPLES, 2000); /* rms == amplitude for a constant signal, well above threshold */
    memset(&vox, 0, sizeof(vox));

    /* t=0: silence, still IDLE. */
    active = peer_alsa_vox_update(&vox, silence, ALSA_PCM_SAMPLES, 0, &cfg);
    checks++;
    if (active != 0 || vox.state != ALSA_VOX_IDLE) {
        fprintf(stderr, "FAIL: t=0 silence should stay IDLE/inactive\n");
        fails++;
    }

    /* t=10: loud, arms but attack debounce (50ms) hasn't elapsed -> inactive. */
    active = peer_alsa_vox_update(&vox, loud, ALSA_PCM_SAMPLES, 10, &cfg);
    checks++;
    if (active != 0 || vox.state != ALSA_VOX_ARMED) {
        fprintf(stderr, "FAIL: t=10 loud should ARM but stay inactive (attack debounce)\n");
        fails++;
    }

    /* t=40: still loud, elapsed since armed = 30ms < 50ms -> still inactive. */
    active = peer_alsa_vox_update(&vox, loud, ALSA_PCM_SAMPLES, 40, &cfg);
    checks++;
    if (active != 0 || vox.state != ALSA_VOX_ARMED) {
        fprintf(stderr, "FAIL: t=40 loud should still be ARMED (30ms < 50ms attack)\n");
        fails++;
    }

    /* t=70: elapsed since armed = 60ms >= 50ms -> TX, active. */
    active = peer_alsa_vox_update(&vox, loud, ALSA_PCM_SAMPLES, 70, &cfg);
    checks++;
    if (active != 1 || vox.state != ALSA_VOX_TX) {
        fprintf(stderr, "FAIL: t=70 loud should cross into TX/active\n");
        fails++;
    }

    /* t=250: silence, but within hang (250-100=150ms < 200ms) -> still active. */
    active = peer_alsa_vox_update(&vox, silence, ALSA_PCM_SAMPLES, 250, &cfg);
    checks++;
    if (active != 1 || vox.state != ALSA_VOX_TX) {
        fprintf(stderr, "FAIL: t=250 silence within hang should stay active\n");
        fails++;
    }

    /* t=310: silence, hang elapsed (310-100=210ms >= 200ms) -> COOLDOWN, inactive. */
    active = peer_alsa_vox_update(&vox, silence, ALSA_PCM_SAMPLES, 310, &cfg);
    checks++;
    if (active != 0 || vox.state != ALSA_VOX_COOLDOWN) {
        fprintf(stderr, "FAIL: t=310 should end the call into COOLDOWN\n");
        fails++;
    }

    /* t=400: loud again, but still cooling down (400 < 610) -> residual mic ignored. */
    active = peer_alsa_vox_update(&vox, loud, ALSA_PCM_SAMPLES, 400, &cfg);
    checks++;
    if (active != 0 || vox.state != ALSA_VOX_COOLDOWN) {
        fprintf(stderr, "FAIL: t=400 loud during cooldown must be ignored\n");
        fails++;
    }

    /* t=620: cooldown elapsed (>= 610), silent -> back to IDLE. */
    active = peer_alsa_vox_update(&vox, silence, ALSA_PCM_SAMPLES, 620, &cfg);
    checks++;
    if (active != 0 || vox.state != ALSA_VOX_IDLE) {
        fprintf(stderr, "FAIL: t=620 cooldown elapsed should return to IDLE\n");
        fails++;
    }

    /* t=630: loud again -- a fresh call can arm normally after returning to IDLE. */
    active = peer_alsa_vox_update(&vox, loud, ALSA_PCM_SAMPLES, 630, &cfg);
    checks++;
    if (active != 0 || vox.state != ALSA_VOX_ARMED) {
        fprintf(stderr, "FAIL: t=630 should arm a fresh call after cooldown\n");
        fails++;
    }

    printf("test_vox_fsm: %d checks, %d failures\n", checks, fails);
    return fails;
}

static int test_cor_debounce(void)
{
    adn_bridge_peer_alsa_t cfg;
    peer_alsa_cor_t cor;
    int checks = 0, fails = 0;
    int active;

    memset(&cfg, 0, sizeof(cfg));
    cfg.cor_debounce_ms = 30;
    memset(&cor, 0, sizeof(cor));

    /* t=0: pin goes active, but debounce (30ms) hasn't elapsed -> inactive. */
    active = peer_alsa_cor_update(&cor, 1, 0, &cfg);
    checks++;
    if (active != 0) {
        fprintf(stderr, "FAIL: t=0 pin active should stay inactive (debounce)\n");
        fails++;
    }

    /* t=10: still raw-active, only 10ms since it flipped -> still inactive. */
    active = peer_alsa_cor_update(&cor, 1, 10, &cfg);
    checks++;
    if (active != 0) {
        fprintf(stderr, "FAIL: t=10 should still be inactive (10ms < 30ms)\n");
        fails++;
    }

    /* t=35: 35ms of stable raw-active >= 30ms -> debounced active. */
    active = peer_alsa_cor_update(&cor, 1, 35, &cfg);
    checks++;
    if (active != 1) {
        fprintf(stderr, "FAIL: t=35 should cross into active\n");
        fails++;
    }

    /* t=40: pin released, but debounce hasn't elapsed -> stays active. */
    active = peer_alsa_cor_update(&cor, 0, 40, &cfg);
    checks++;
    if (active != 1) {
        fprintf(stderr, "FAIL: t=40 release should stay active (debounce)\n");
        fails++;
    }

    /* t=50: only 10ms since release -> still active. */
    active = peer_alsa_cor_update(&cor, 0, 50, &cfg);
    checks++;
    if (active != 1) {
        fprintf(stderr, "FAIL: t=50 should still be active (10ms < 30ms)\n");
        fails++;
    }

    /* t=75: 35ms of stable raw-inactive >= 30ms -> debounced inactive. */
    active = peer_alsa_cor_update(&cor, 0, 75, &cfg);
    checks++;
    if (active != 0) {
        fprintf(stderr, "FAIL: t=75 should cross into inactive\n");
        fails++;
    }

    /* Bounce filtering: raw flips back and forth faster than debounce_ms,
     * so the debounced state must never move (a chattering switch/relay). */
    active = peer_alsa_cor_update(&cor, 1, 100, &cfg); /* flips 0->1 */
    checks++;
    if (active != 0) {
        fprintf(stderr, "FAIL: t=100 bounce start should stay inactive\n");
        fails++;
    }
    active = peer_alsa_cor_update(&cor, 0, 110, &cfg); /* flips 1->0 before 30ms */
    checks++;
    if (active != 0) {
        fprintf(stderr, "FAIL: t=110 bounce should stay inactive\n");
        fails++;
    }
    active = peer_alsa_cor_update(&cor, 1, 115, &cfg); /* flips 0->1 again */
    checks++;
    if (active != 0) {
        fprintf(stderr, "FAIL: t=115 bounce should still be inactive (never stable 30ms)\n");
        fails++;
    }

    printf("test_cor_debounce: %d checks, %d failures\n", checks, fails);
    return fails;
}

#ifdef WITH_ALSA
/* ALSA's "null" plugin (discard on playback, zero samples on capture) works
 * without any real sound card, so this exercises the actual open/read/write
 * path even in a headless CI box -- see aplay -L | grep -A1 '^null'. */
static int test_open_null_device(void)
{
    peer_alsa_t p;
    adn_bridge_peer_alsa_t cfg;
    int16_t silence[ALSA_PCM_SAMPLES];
    int checks = 0, fails = 0;
    int n;

    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.capture_device, "null");
    strcpy(cfg.playback_device, "null");
    cfg.gain = 1.0f;
    cfg.vox_threshold = 500;
    cfg.vox_attack_ms = 80;
    cfg.vox_hang_ms = 700;
    cfg.tx_cooldown_ms = 800;

    checks++;
    if (peer_alsa_open(&p, &cfg) != 0) {
        fprintf(stderr, "FAIL: peer_alsa_open() on ALSA null device should succeed\n");
        fails++;
        printf("test_open_null_device: %d checks, %d failures\n", checks, fails);
        return fails;
    }

    fill_pcm(silence, ALSA_PCM_SAMPLES, 0);
    n = peer_alsa_write_pcm(&p, silence, ALSA_PCM_SAMPLES);
    checks++;
    if (n <= 0) {
        fprintf(stderr, "FAIL: peer_alsa_write_pcm() to null device should accept samples\n");
        fails++;
    }

    /* Capture on "null" always yields zero samples -- just confirm the
     * non-blocking poll doesn't crash or hang; no assertion on VOX here. */
    peer_alsa_poll(&p, 5);
    checks++;

    peer_alsa_close(&p);
    printf("test_open_null_device: %d checks, %d failures\n", checks, fails);
    return fails;
}
#else
static int test_open_without_alsa(void)
{
    peer_alsa_t p;
    adn_bridge_peer_alsa_t cfg;
    int checks = 0, fails = 0;

    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.capture_device, "default");
    strcpy(cfg.playback_device, "default");
    cfg.gain = 1.0f;
    cfg.vox_threshold = 500;
    cfg.vox_attack_ms = 80;
    cfg.vox_hang_ms = 700;
    cfg.tx_cooldown_ms = 800;

    checks++;
    if (peer_alsa_open(&p, &cfg) == 0) {
        fprintf(stderr, "FAIL: peer_alsa_open() without WITH_ALSA should fail cleanly\n");
        fails++;
    }

    printf("test_open_without_alsa: %d checks, %d failures\n", checks, fails);
    return fails;
}
#endif

int main(void)
{
    int fails = 0;

    fails += test_vox_fsm();
    fails += test_cor_debounce();
#ifdef WITH_ALSA
    fails += test_open_null_device();
#else
    fails += test_open_without_alsa();
#endif

    return fails ? 1 : 0;
}
