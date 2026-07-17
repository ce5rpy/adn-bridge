/*
 * DV3000 / AMBEServer UDP client (host:port).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "vocoder.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DV3K_START       0x61
#define DV3K_TYPE_CTRL   0x00
#define DV3K_TYPE_AMBE   0x01
#define DV3K_TYPE_AUDIO  0x02
#define DV3K_AMBE_FIELD  0x01
#define DV3K_AUDIO_FIELD 0x00

static const uint8_t DV3K_PRODID_REQ[] = { 0x61, 0x00, 0x01, 0x00, 0x30 };
/* RATET 34 (0x22) = 2450 bps speech, FEC=0 (49-bit frames).
 *
 * md380-emu (and Analog_Bridge soft path for 49-bit) expects the 49 bits in
 * DMR interleaved order on the wire, then deinterleaves before decode.
 * ModeConv / mbelib use deinterleaved (raw) 49-bit — convert at this boundary.
 *
 * Note: Analog_Bridge's useEmulator=true path prefers AMBE72 (RATET 33 /
 * RATEP 3600x2450, 9-byte FEC frames). We stay on 49-bit+interleave49 for now.
 */
static const uint8_t DV3K_RATET_DMR[] = { 0x61, 0x00, 0x02, 0x00, 0x09, 0x22 };

/* Same matrix as md380-emu interleave49 / deinterleave49. */
static const uint8_t INTERLEAVE49_MATRIX[49] = {
    0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 41, 43, 45, 47,
    1, 4, 7, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40, 42, 44, 46, 48,
    2, 5, 8, 11, 14, 17, 20, 23, 26, 29, 32, 35, 38
};

static unsigned g_enc_ok, g_enc_fail, g_dec_ok, g_dec_fail;
static int g_enc_log_n, g_dec_log_n;
static int g_consec_fail;

static int dbg_periodic(int *n)
{
    (*n)++;
    return (*n <= 3 || (*n % 25) == 0);
}

static double pcm_rms(const int16_t *pcm, int n)
{
    double acc = 0.0;
    int i;

    if (n <= 0)
        return 0.0;
    for (i = 0; i < n; i++)
        acc += (double)pcm[i] * (double)pcm[i];
    return sqrt(acc / (double)n);
}

static void hex7(char *out, size_t outlen, const uint8_t b[7])
{
    snprintf(out, outlen, "%02x%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
}

static void ambe49_bits_from_bytes(const uint8_t in[7], uint8_t bits[49])
{
    int i, j, n = 0;

    for (i = 0; i < 6; i++) {
        for (j = 0; j < 8; j++)
            bits[n++] = (uint8_t)((in[i] >> (7 - j)) & 1);
    }
    bits[48] = (uint8_t)((in[6] >> 7) & 1);
}

static void ambe49_bytes_from_bits(const uint8_t bits[49], uint8_t out[7])
{
    int i, j, n = 0;

    memset(out, 0, 7);
    for (i = 0; i < 6; i++) {
        for (j = 0; j < 8; j++) {
            if (bits[n++])
                out[i] |= (uint8_t)(1u << (7 - j));
        }
    }
    if (bits[48])
        out[6] = 0x80;
}

/* Raw/deinterleaved 49-bit → md380-emu wire (interleaved). */
static void ambe49_interleave(uint8_t frame[7])
{
    uint8_t in_bits[49], out_bits[49];
    int i;

    ambe49_bits_from_bytes(frame, in_bits);
    memset(out_bits, 0, sizeof(out_bits));
    for (i = 0; i < 49; i++)
        out_bits[INTERLEAVE49_MATRIX[i]] = in_bits[i];
    ambe49_bytes_from_bits(out_bits, frame);
}

/* md380-emu wire (interleaved) → raw/deinterleaved 49-bit for ModeConv. */
static void ambe49_deinterleave(uint8_t frame[7])
{
    uint8_t in_bits[49], out_bits[49];
    int i;

    ambe49_bits_from_bytes(frame, in_bits);
    memset(out_bits, 0, sizeof(out_bits));
    for (i = 0; i < 49; i++)
        out_bits[i] = in_bits[INTERLEAVE49_MATRIX[i]];
    ambe49_bytes_from_bits(out_bits, frame);
}

static int voc_recv(vocoder_t *v, uint8_t *buf, int buflen, int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;
    int n;

    FD_ZERO(&rfds);
    FD_SET(v->sock, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    n = select(v->sock + 1, &rfds, NULL, NULL, &tv);
    if (n <= 0)
        return n == 0 ? 0 : -1;
    return (int)recv(v->sock, buf, (size_t)buflen, 0);
}

static void voc_drain(vocoder_t *v)
{
    uint8_t junk[512];

    while (voc_recv(v, junk, (int)sizeof(junk), 0) > 0)
        ;
}

/* Returns response length, 0 on timeout, -1 on I/O/format error. */
static int voc_exchange_once(vocoder_t *v, const uint8_t *req, int reqlen,
                             uint8_t *rsp, int rsplen, int timeout_ms)
{
    int n;

    voc_drain(v);
    if (sendto(v->sock, req, (size_t)reqlen, 0,
               (struct sockaddr *)&v->peer, sizeof(v->peer)) < 0)
        return -1;
    n = voc_recv(v, rsp, rsplen, timeout_ms);
    if (n == 0)
        return 0;
    if (n < 4 || rsp[0] != DV3K_START)
        return -1;
    return n;
}

/* Re-assert DMR 49-bit rate after emu stalls / desync (md380-emu under qemu). */
static int voc_recover_rate(vocoder_t *v)
{
    uint8_t rsp[64];
    int n;

    n = voc_exchange_once(v, DV3K_RATET_DMR, (int)sizeof(DV3K_RATET_DMR),
                          rsp, (int)sizeof(rsp), 300);
    if (n > 0) {
        LOG_WARNING("vocoder: re-RATET 34 after %d consecutive failures\n",
                    g_consec_fail);
        return 0;
    }
    return -1;
}

static int voc_exchange(vocoder_t *v, const uint8_t *req, int reqlen,
                        uint8_t *rsp, int rsplen)
{
    /* Fail faster when the emu is wedged so the bridge loop stays responsive. */
    int timeout_ms = (g_consec_fail >= 2) ? 120 : 400;
    int n = voc_exchange_once(v, req, reqlen, rsp, rsplen, timeout_ms);

    if (n > 0) {
        g_consec_fail = 0;
        return n;
    }
    g_consec_fail++;
    if (g_consec_fail >= 3) {
        if (voc_recover_rate(v) == 0)
            n = voc_exchange_once(v, req, reqlen, rsp, rsplen, 400);
        if (n > 0) {
            g_consec_fail = 0;
            return n;
        }
    }
    return n;
}

int vocoder_open(vocoder_t *v, const char *host, int port)
{
    struct hostent *he;
    uint8_t rsp[256];
    char prod[64];
    int n, i;

    memset(v, 0, sizeof(*v));
    g_enc_ok = g_enc_fail = g_dec_ok = g_dec_fail = 0;
    g_enc_log_n = g_dec_log_n = 0;
    g_consec_fail = 0;
    if (!host || !*host || port <= 0)
        return -1;
    strncpy(v->host, host, sizeof(v->host) - 1);
    v->port = port;

    v->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (v->sock < 0) {
        LOG_ERROR("vocoder: socket failed: %s\n", strerror(errno));
        return -1;
    }

    memset(&v->peer, 0, sizeof(v->peer));
    v->peer.sin_family = AF_INET;
    v->peer.sin_port = htons((uint16_t)port);
    if (inet_aton(host, &v->peer.sin_addr) == 0) {
        he = gethostbyname(host);
        if (!he) {
            LOG_ERROR("vocoder: cannot resolve %s\n", host);
            close(v->sock);
            v->sock = -1;
            return -1;
        }
        memcpy(&v->peer.sin_addr, he->h_addr, (size_t)he->h_length);
    }

    n = voc_exchange(v, DV3K_PRODID_REQ, (int)sizeof(DV3K_PRODID_REQ), rsp, (int)sizeof(rsp));
    if (n <= 0) {
        LOG_ERROR("vocoder: PRODID probe failed (%s:%d)%s\n", host, port,
                  n == 0 ? " timeout" : "");
        close(v->sock);
        v->sock = -1;
        return -1;
    }
    prod[0] = '\0';
    if (n > 5 && rsp[3] == DV3K_TYPE_CTRL && rsp[4] == 0x30) {
        int plen = n - 5;
        if (plen > (int)sizeof(prod) - 1)
            plen = (int)sizeof(prod) - 1;
        for (i = 0; i < plen; i++) {
            char c = (char)rsp[5 + i];
            prod[i] = (c >= 32 && c < 127) ? c : '.';
        }
        prod[plen] = '\0';
    }

    n = voc_exchange(v, DV3K_RATET_DMR, (int)sizeof(DV3K_RATET_DMR), rsp, (int)sizeof(rsp));
    if (n <= 0) {
        LOG_ERROR("vocoder: DMR rate set failed%s\n", n == 0 ? " timeout" : "");
        close(v->sock);
        v->sock = -1;
        return -1;
    }
    v->ready = 1;
    LOG_INFO("vocoder ready at %s:%d (RATET 34 / 49-bit FEC=0, wire=interleave49, api=raw)\n",
             host, port);
    if (prod[0])
        LOG_INFO("vocoder PRODID: %s\n", prod);
    LOG_DEBUG("vocoder: Analog_Bridge soft path often uses AMBE72/RATET33; we use 49-bit+IL49\n");
    return 0;
}

void vocoder_close(vocoder_t *v)
{
    if (v->ready) {
        LOG_INFO("vocoder stats: enc ok/fail=%u/%u dec ok/fail=%u/%u\n",
                 g_enc_ok, g_enc_fail, g_dec_ok, g_dec_fail);
    }
    if (v->sock >= 0)
        close(v->sock);
    v->sock = -1;
    v->ready = 0;
}

int vocoder_encode(vocoder_t *v, const int16_t pcm[VOC_PCM_SAMPLES], uint8_t ambe[VOC_AMBE_BYTES])
{
    uint8_t req[4 + 2 + VOC_PCM_SAMPLES * 2];
    uint8_t rsp[64];
    uint8_t wire[VOC_AMBE_BYTES];
    char raw_h[20], wire_h[20];
    uint16_t plen;
    int i, n, nbytes;
    double rms;

    if (!v->ready)
        return -1;

    rms = pcm_rms(pcm, VOC_PCM_SAMPLES);
    plen = (uint16_t)(2 + VOC_PCM_SAMPLES * 2);
    req[0] = DV3K_START;
    req[1] = (uint8_t)((plen >> 8) & 0xff);
    req[2] = (uint8_t)(plen & 0xff);
    req[3] = DV3K_TYPE_AUDIO;
    req[4] = DV3K_AUDIO_FIELD;
    req[5] = (uint8_t)VOC_PCM_SAMPLES;
    for (i = 0; i < VOC_PCM_SAMPLES; i++) {
        int16_t s = pcm[i];
        req[6 + i * 2] = (uint8_t)((s >> 8) & 0xff);
        req[7 + i * 2] = (uint8_t)(s & 0xff);
    }

    n = voc_exchange(v, req, (int)(4 + plen), rsp, (int)sizeof(rsp));
    if (n == 0) {
        g_enc_fail++;
        if (dbg_periodic(&g_enc_log_n))
            LOG_WARNING("vocoder ENC timeout (pcm_rms=%.0f ok/fail=%u/%u)\n",
                        rms, g_enc_ok, g_enc_fail);
        return -1;
    }
    if (n < 6 || rsp[3] != DV3K_TYPE_AMBE || rsp[4] != DV3K_AMBE_FIELD) {
        g_enc_fail++;
        if (dbg_periodic(&g_enc_log_n))
            LOG_WARNING("vocoder ENC bad rsp n=%d type=0x%02x field=0x%02x (pcm_rms=%.0f)\n",
                        n, n >= 4 ? rsp[3] : 0, n >= 5 ? rsp[4] : 0, rms);
        return -1;
    }
    if (rsp[5] != 49) {
        g_enc_fail++;
        if (dbg_periodic(&g_enc_log_n))
            LOG_WARNING("vocoder ENC unexpected bits=%u (want 49; check RATET)\n",
                        (unsigned)rsp[5]);
        return -1;
    }
    nbytes = (rsp[5] + 7) / 8;
    if (nbytes < VOC_AMBE_BYTES || n < 6 + nbytes) {
        g_enc_fail++;
        return -1;
    }
    memcpy(wire, rsp + 6, VOC_AMBE_BYTES);
    memcpy(ambe, wire, VOC_AMBE_BYTES);
    /* Soft server returns interleaved 49-bit; expose raw to ModeConv. */
    ambe49_deinterleave(ambe);
    g_enc_ok++;
    if (dbg_periodic(&g_enc_log_n)) {
        hex7(wire_h, sizeof(wire_h), wire);
        hex7(raw_h, sizeof(raw_h), ambe);
        LOG_DEBUG("vocoder ENC #%u pcm_rms=%.0f wire=%s raw=%s ok/fail=%u/%u\n",
                  g_enc_ok, rms, wire_h, raw_h, g_enc_ok, g_enc_fail);
    }
    return 0;
}

int vocoder_decode(vocoder_t *v, const uint8_t ambe[VOC_AMBE_BYTES], int16_t pcm[VOC_PCM_SAMPLES])
{
    uint8_t req[4 + 2 + VOC_AMBE_BYTES];
    uint8_t rsp[4 + 2 + VOC_PCM_SAMPLES * 2 + 8];
    uint8_t wire[VOC_AMBE_BYTES];
    char raw_h[20], wire_h[20];
    uint16_t plen;
    int i, n, ns;
    double rms;

    if (!v->ready)
        return -1;

    memcpy(wire, ambe, VOC_AMBE_BYTES);
    ambe49_interleave(wire);

    plen = (uint16_t)(2 + VOC_AMBE_BYTES);
    req[0] = DV3K_START;
    req[1] = (uint8_t)((plen >> 8) & 0xff);
    req[2] = (uint8_t)(plen & 0xff);
    req[3] = DV3K_TYPE_AMBE;
    req[4] = DV3K_AMBE_FIELD;
    req[5] = 49; /* DMR AMBE bits */
    memcpy(req + 6, wire, VOC_AMBE_BYTES);

    n = voc_exchange(v, req, (int)(4 + plen), rsp, (int)sizeof(rsp));
    if (n == 0) {
        g_dec_fail++;
        if (dbg_periodic(&g_dec_log_n)) {
            hex7(raw_h, sizeof(raw_h), ambe);
            hex7(wire_h, sizeof(wire_h), wire);
            LOG_WARNING("vocoder DEC timeout raw=%s wire=%s ok/fail=%u/%u\n",
                        raw_h, wire_h, g_dec_ok, g_dec_fail);
        }
        return -1;
    }
    if (n < 6 || rsp[3] != DV3K_TYPE_AUDIO || rsp[4] != DV3K_AUDIO_FIELD) {
        g_dec_fail++;
        if (dbg_periodic(&g_dec_log_n)) {
            hex7(raw_h, sizeof(raw_h), ambe);
            LOG_WARNING("vocoder DEC bad rsp n=%d type=0x%02x field=0x%02x raw=%s\n",
                        n, n >= 4 ? rsp[3] : 0, n >= 5 ? rsp[4] : 0, raw_h);
        }
        return -1;
    }
    ns = rsp[5];
    if (ns > VOC_PCM_SAMPLES)
        ns = VOC_PCM_SAMPLES;
    if (n < 6 + ns * 2) {
        g_dec_fail++;
        return -1;
    }
    for (i = 0; i < ns; i++) {
        int16_t s = (int16_t)((rsp[6 + i * 2] << 8) | rsp[7 + i * 2]);
        pcm[i] = s;
    }
    for (; i < VOC_PCM_SAMPLES; i++)
        pcm[i] = 0;
    g_dec_ok++;
    rms = pcm_rms(pcm, VOC_PCM_SAMPLES);
    if (dbg_periodic(&g_dec_log_n)) {
        hex7(raw_h, sizeof(raw_h), ambe);
        hex7(wire_h, sizeof(wire_h), wire);
        LOG_DEBUG("vocoder DEC #%u raw=%s wire=%s pcm_rms=%.0f ok/fail=%u/%u\n",
                  g_dec_ok, raw_h, wire_h, rms, g_dec_ok, g_dec_fail);
    }
    return 0;
}
