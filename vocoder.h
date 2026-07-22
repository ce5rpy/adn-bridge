/*
 * Remote AMBE vocoder client (DV3000 / AMBEServer wire protocol).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef ADN_BRIDGE_VOCODER_H
#define ADN_BRIDGE_VOCODER_H

#include <stdint.h>
#include <netinet/in.h>

#define VOC_PCM_SAMPLES 160 /* 20 ms @ 8 kHz */
#define VOC_AMBE_BYTES  7   /* 49-bit DMR AMBE */

typedef struct {
    int sock;
    struct sockaddr_in peer;
    char host[128];
    int port;
    int ready;
} vocoder_t;

int vocoder_open(vocoder_t *v, const char *host, int port);
void vocoder_close(vocoder_t *v);
int vocoder_is_ready(const vocoder_t *v);
/* PCM s16 LE (host endian) 160 samples -> 7-byte raw (deinterleaved) AMBE. */
int vocoder_encode(vocoder_t *v, const int16_t pcm[VOC_PCM_SAMPLES], uint8_t ambe[VOC_AMBE_BYTES]);
/* 7-byte raw (deinterleaved) AMBE -> PCM s16 LE 160 samples. */
int vocoder_decode(vocoder_t *v, const uint8_t ambe[VOC_AMBE_BYTES], int16_t pcm[VOC_PCM_SAMPLES]);

#endif
