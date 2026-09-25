/*
 * Local ALSA sound-card peer -- PTT via software VOX (RMS) or a hardware
 * COR/PTT switch on a GPIO pin (libgpiod, WITH_GPIOD build).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PEER_ALSA_H
#define PEER_ALSA_H

#include <stdint.h>

#include "config.h"

#define ALSA_PCM_RATE    8000
#define ALSA_PCM_SAMPLES 160 /* 20 ms @ 8 kHz, matches CODEC_PCM_SAMPLES */

typedef enum {
    ALSA_VOX_IDLE = 0,
    ALSA_VOX_ARMED,   /* RMS above threshold, waiting out vox_attack_ms debounce */
    ALSA_VOX_TX,      /* on-air; read_pcm() hands out captured frames */
    ALSA_VOX_COOLDOWN /* just ended; mic ignored for tx_cooldown_ms */
} peer_alsa_vox_state_t;

typedef struct {
    peer_alsa_vox_state_t state;
    uint32_t armed_since_ms;    /* CLOCK_MONOTONIC ms when RMS first crossed threshold */
    uint32_t last_active_ms;    /* last ms the RMS was above threshold, while TX */
    uint32_t cooldown_until_ms;
    /* Diagnostics only (set every call, read by nothing but the log lines in
     * peer_alsa_vox_update itself) -- calibrating vox_threshold in the field
     * needs visibility into the actual RMS the mic/device is producing. */
    int      last_rms;
    uint32_t last_heartbeat_ms;
} peer_alsa_vox_t;

/* Debounced state of a GPIO COR/PTT input pin -- alternative to VOX when
 * [peer.*] ptt_type=gpio. Pure state, no I/O (see peer_alsa_cor_update). */
typedef struct {
    int raw_active;       /* last raw (pre-debounce) read, normalized 0/1 */
    int debounced_active; /* stable, debounced state -- this is "on air?" */
    uint32_t change_ms;   /* CLOCK_MONOTONIC ms when raw_active last flipped */
} peer_alsa_cor_t;

typedef struct {
    adn_bridge_peer_alsa_t cfg;
    peer_alsa_vox_t vox;
    peer_alsa_cor_t cor;
    int open;
    /* snd_pcm_t* when built with WITH_ALSA; kept void* so the struct layout
     * (and anything embedding it, e.g. media_peer_slot_t) doesn't change
     * shape depending on the build flag. Same story for the two gpiod_*
     * pointers under WITH_GPIOD. */
    void *capture_pcm;
    void *playback_pcm;
    void *gpio_chip;    /* struct gpiod_chip* */
    void *gpio_request; /* struct gpiod_line_request* */
    int16_t pcm_in[ALSA_PCM_SAMPLES];
    int pcm_in_count;
} peer_alsa_t;

int peer_alsa_open(peer_alsa_t *p, const adn_bridge_peer_alsa_t *cfg);
void peer_alsa_close(peer_alsa_t *p);
/* Reserved for future PTT-out timing; no-op today. */
void peer_alsa_tick(peer_alsa_t *p);
/* Non-blocking read of one ALSA capture period; runs the VOX FSM and stages
 * the frame in pcm_in when on-air. Returns samples available (>0), or 0. */
int peer_alsa_poll(peer_alsa_t *p, int timeout_ms);
/* Read up to max_samples of inbound PCM (8 kHz s16 LE) staged by the last
 * poll(). Returns samples read. */
int peer_alsa_read_pcm(peer_alsa_t *p, int16_t *pcm, int max_samples);
/* Write `samples` of PCM to the playback device. Returns samples written. */
int peer_alsa_write_pcm(peer_alsa_t *p, const int16_t *pcm, int samples);
/* No-op: ALSA playback has no outbound accumulation buffer at this layer
 * (unlike EchoLink's RTP packetizer). Kept for API symmetry with peer_echolink. */
void peer_alsa_flush_pcm(peer_alsa_t *p);
/* Drop any staged inbound PCM and reset VOX to IDLE (call end — avoid
 * residual silence/echo restarting TX). */
void peer_alsa_drop_pcm_in(peer_alsa_t *p);
void peer_alsa_on_sigint(peer_alsa_t *p);

/* Pure VOX state-machine step, exposed for unit testing without hardware.
 * Returns 1 if this frame should be treated as on-air (TX active), 0
 * otherwise. IDLE->ARMED->TX needs vox_attack_ms of continuous RMS above
 * threshold; TX->COOLDOWN->IDLE needs vox_hang_ms of silence, then
 * tx_cooldown_ms with the mic ignored (residual echo/relay tail). */
int peer_alsa_vox_update(peer_alsa_vox_t *vox, const int16_t *pcm, int samples,
                          uint32_t now_ms, const adn_bridge_peer_alsa_t *cfg);

/* Pure COR debounce step, exposed for unit testing without hardware.
 * raw_active is the just-read (undebounced) pin state (1 = active per
 * cfg->cor_active_low's polarity, 0 = inactive). Returns the debounced
 * "on air?" state -- flips only after cor_debounce_ms of a stable new
 * reading, filtering switch/relay bounce on the raw signal. */
int peer_alsa_cor_update(peer_alsa_cor_t *cor, int raw_active, uint32_t now_ms,
                          const adn_bridge_peer_alsa_t *cfg);

#endif
