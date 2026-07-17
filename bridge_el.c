/*
 * EchoLink <-> DMR/YSF voice bridge (PCM + remote AMBE vocoder).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "bridge_el.h"
#include "config.h"
#include "hbp/dmr_codec.h"
#include "log.h"
#include "mmdvm/modeconv_wrap.h"
#include "mmdvm/ysfpayload_wrap.h"
#include "ysf_fich.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DMR_FRAME_MS   60
#define YSF_FRAME_MS   90 /* same pace as DMR→YSF (bridge.c / DMR2YSF) */
#define CONNECT_PTT_MS 1000
/* End EL->DMR/YSF after this much without inbound EL PCM (key-down silence
 * must still hold / activate the TG; hang follows PCM presence, not RMS). */
#define EL_HANG_MS     700
/* After EL->DMR/YSF end, ignore residual conference PCM (no phantom reopen). */
#define EL_TX_COOLDOWN_MS 800
/* DMR/YSF->EL without VTERM/EOT used to leave call_active=2 and block EL TX. */
#define DMR_RX_HANG_MS 1500
#define YSF_RX_HANG_MS 1500
#define DMRD_FT_DATA_SYNC  2U
#define DMRD_FT_VOICE_SYNC 1U
#define DMRD_DTYPE_VHEAD   1U
#define DMRD_DTYPE_VTERM   2U

#define YSF_DT_VD_MODE2       0x02U
#define YSF_FI_HEADER         0x00U
#define YSF_FI_COMMUNICATIONS 0x01U
#define YSF_FI_TERMINATOR     0x02U
#define YSF_FICH_FT           6U
#define YSF_FICH_CM           0U
#define YSF_WIRE_DST_ALL      "ALL       "
#define YSF_SYNC_BYTES        "\xD4\x71\xC9\x63\x4D"

static const uint8_t YSF_DCH_DT1[10] = {1U, 34U, 97U, 95U, 43U, 3U, 17U, 0U, 0U, 0U};
static const uint8_t YSF_DCH_DT2[10] = {0U, 0U, 0U, 0U, 108U, 32U, 28U, 32U, 3U, 8U};

static const uint8_t MS_SOURCED_AUDIO_SYNC[7] =
    {0x07U, 0xF7U, 0xD5U, 0xDDU, 0x57U, 0xDFU, 0xD0U};
static const uint8_t DMR_SYNC_MASK[7] =
    {0x0FU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xF0U};
static const uint8_t DMR_SILENCE_DATA[33] =
    {0xB9U, 0xE8U, 0x81U, 0x52U, 0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU, 0xB9U, 0xE8U,
     0x81U, 0x52U, 0x60U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x73U, 0x00U,
     0x2AU, 0x6BU, 0xB9U, 0xE8U, 0x81U, 0x52U, 0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU};

static void stamp_now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static int pcm_rms16(const int16_t *pcm, int n)
{
    long long acc = 0;
    int i;

    if (n <= 0)
        return 0;
    for (i = 0; i < n; i++)
        acc += (long)pcm[i] * (long)pcm[i];
    return (int)sqrt((double)acc / (double)n);
}

static int dbg_periodic(int *n)
{
    (*n)++;
    return (*n <= 2 || (*n % 20) == 0);
}

static void dmrd_parse_b15(uint8_t b15, uint8_t *ft, uint8_t *dtype)
{
    *ft = (b15 >> 4) & 0x03U;
    *dtype = b15 & 0x0fU;
}

static int dmrd_is_header(const uint8_t *pkt, int len)
{
    uint8_t ft, dtype;

    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0)
        return 0;
    if (pkt[15] & 0x40)
        return 0;
    dmrd_parse_b15(pkt[15], &ft, &dtype);
    return ft == DMRD_FT_DATA_SYNC && dtype == DMRD_DTYPE_VHEAD;
}

static int dmrd_is_terminator(const uint8_t *pkt, int len)
{
    uint8_t ft, dtype;

    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0)
        return 0;
    dmrd_parse_b15(pkt[15], &ft, &dtype);
    return ft == DMRD_FT_DATA_SYNC && dtype == DMRD_DTYPE_VTERM;
}

static int dmrd_is_voice(const uint8_t *pkt, int len)
{
    uint8_t ft, dtype;

    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0)
        return 0;
    if (pkt[15] & 0x40)
        return 0;
    dmrd_parse_b15(pkt[15], &ft, &dtype);
    return ft <= 1U;
}

static const char *dmrd_class_label(const uint8_t *pkt, int len)
{
    if (dmrd_is_header(pkt, len))
        return "VHEAD";
    if (dmrd_is_terminator(pkt, len))
        return "VTERM";
    if (dmrd_is_voice(pkt, len))
        return "VOICE";
    return "OTHER";
}

static int ms_elapsed(const struct timespec *since, int interval_ms)
{
    struct timespec now;
    long elapsed;

    clock_gettime(CLOCK_MONOTONIC, &now);
    elapsed = (now.tv_sec - since->tv_sec) * 1000L
            + (now.tv_nsec - since->tv_nsec) / 1000000L;
    return elapsed >= interval_ms;
}

static long ms_since(const struct timespec *since)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - since->tv_sec) * 1000L
         + (now.tv_nsec - since->tv_nsec) / 1000000L;
}

static uint32_t new_stream_id(void)
{
    static int seeded;
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = 1;
    }
    return (uint32_t)rand() | 1U;
}

static int dmr_id_rf24(int dmrid)
{
    return (dmrid > 99999999) ? dmrid / 100 : dmrid;
}

static void dmr_id_to_bytes3(int dmrid, uint8_t out[3])
{
    int id = dmr_id_rf24(dmrid);
    out[0] = (uint8_t)((id >> 16) & 0xff);
    out[1] = (uint8_t)((id >> 8) & 0xff);
    out[2] = (uint8_t)(id & 0xff);
}

/* EchoLink/YSF callsign before '-' or '/' (CA5RPY-L → CA5RPY), same as YSF→DMR. */
static void bridge_el_callsign_base(const char *src, char out[16])
{
    int i, j = 0;

    out[0] = '\0';
    if (!src)
        return;
    for (i = 0; src[i] && j < 15; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c == ' ' || c == '\t') {
            if (j == 0)
                continue;
            break;
        }
        if (c == '-' || c == '/')
            break;
        out[j++] = (char)toupper(c);
    }
    out[j] = '\0';
}

/* Space-padded base callsign for alias / numeric id (same shape as YSF→DMR). */
static void bridge_el_format_base10(char out[10], const char *base)
{
    int i;

    memset(out, ' ', 10);
    if (!base)
        return;
    for (i = 0; base[i] && i < 10; i++)
        out[i] = (char)toupper((unsigned char)base[i]);
}

static int bridge_el_callsign10_to_dmrid(const char cs[10])
{
    char digits[16];
    int i, j = 0, has_digit = 0;

    for (i = 0; i < 10; i++) {
        if (cs[i] == ' ')
            continue;
        if (cs[i] >= '0' && cs[i] <= '9') {
            has_digit = 1;
            if (j < 15)
                digits[j++] = cs[i];
        } else {
            return 0;
        }
    }
    if (!has_digit || j == 0)
        return 0;
    digits[j] = '\0';
    return atoi(digits);
}

/*
 * Resolve EchoLink remote talker (inbound SDES / conference) like YSF→DMR:
 *   callsign base → numeric id or subscriber alias → DMR RF id.
 * Unknown talker falls back to bridge [dmr] callsign + dmrid.
 * EL→YSF keeps the full wire callsign (incl. -L/-R) in net_src for the radio.
 */
static void bridge_el_resolve_el_talker(bridge_el_t *b)
{
    const char *raw = peer_el_remote_talker(&b->el);
    char base[16];
    char talker10[10];
    int id = 0;

    if (!raw || !raw[0])
        raw = b->el.callsign;
    bridge_el_callsign_base(raw, base);
    if (!base[0]) {
        bridge_el_callsign_base(b->el.callsign, base);
        raw = b->el.callsign;
    }

    bridge_el_format_base10(talker10, base);
    id = bridge_el_callsign10_to_dmrid(talker10);
    if (id <= 0 && base[0] && b->aliases)
        id = ysf2dmr_alias_lookup_id(b->aliases, base);

    if (b->mode == YSF2DMR_MODE_ECHOLINK_YSF) {
        /* Full EchoLink callsign on YSF wire (e.g. CA5RPY-L). */
        bridge_el_format_callsign10(b->net_src, raw);
        b->el_rf_id = id > 0 ? id : b->bridge_dmrid;
        LOG_YSF_INFO("EL->YSF talker raw=%s base=%s\n", raw, base[0] ? base : "?");
        return;
    }

    /* EL→DMR: same callsign→id path as YSF→DMR (bridge_assign_ysf_talker). */
    if (id > 0) {
        memcpy(b->net_src, talker10, 10);
        b->el_rf_id = id;
        LOG_DMR_INFO("EL->DMR talker %s -> id %d (alias)\n", base, id);
        return;
    }

    /* Unknown: cross on bridge identity (same policy as YSF→DMR). */
    if (b->dmr.dmrid > 0) {
        memcpy(b->net_src, b->dmr.callsign, 10);
        b->el_rf_id = b->dmr.dmrid;
        LOG_DMR_INFO("EL->DMR talker %s unknown -> bridge %.10s id %d\n",
                     base[0] ? base : "?", b->net_src, b->el_rf_id);
        return;
    }
    if (b->bridge_dmrid > 0) {
        b->el_rf_id = b->bridge_dmrid;
        LOG_DMR_INFO("EL->DMR talker %s unknown -> bridge id %d\n",
                     base[0] ? base : "?", b->el_rf_id);
        return;
    }
    b->el_rf_id = 0;
    LOG_DMR_WARNING("EL->DMR talker %s: no DMR id (alias miss, no bridge dmrid)\n",
                    base[0] ? base : "?");
}

static void bridge_el_send_dmrd(bridge_el_t *b, uint8_t frame_type, const uint8_t *voice33)
{
    uint8_t pkt[55];
    uint8_t rf[3];
    int rf_id = (b->el_rf_id > 0) ? b->el_rf_id : b->dmr.dmrid;
    int src_id;

    if (rf_id <= 0) {
        LOG_DMR_WARNING("DMR TX skipped: bridge dmrid not configured\n");
        return;
    }
    src_id = dmr_id_rf24(rf_id);
    dmr_id_to_bytes3(rf_id, rf);

    memcpy(pkt, "DMRD", 4);
    pkt[4] = b->dmr_seq++;
    pkt[5] = rf[0];
    pkt[6] = rf[1];
    pkt[7] = rf[2];
    pkt[8] = (b->dmr.tg >> 16) & 0xff;
    pkt[9] = (b->dmr.tg >> 8) & 0xff;
    pkt[10] = (b->dmr.tg >> 0) & 0xff;
    pkt[11] = (b->dmr.dmrid >> 24) & 0xff;
    pkt[12] = (b->dmr.dmrid >> 16) & 0xff;
    pkt[13] = (b->dmr.dmrid >> 8) & 0xff;
    pkt[14] = (b->dmr.dmrid >> 0) & 0xff;
    pkt[15] = frame_type;
    *(uint32_t *)(pkt + 16) = b->dmr_stream_id;

    memcpy(buf, pkt, 55);
    rx_srcid = src_id;
    tx_tgid = b->dmr.tg;

    if (((frame_type >> 4) & 0x03U) == DMRD_FT_DATA_SYNC) {
        generate_header();
        memcpy(pkt + 20, buf + 20, 33);
    } else if (voice33) {
        int i;
        memcpy(pkt + 20, voice33, 33);
        if (((frame_type >> 4) & 0x03U) == DMRD_FT_VOICE_SYNC) {
            for (i = 0; i < 7; i++)
                pkt[20 + 13 + i] = (uint8_t)((pkt[20 + 13 + i] & ~DMR_SYNC_MASK[i])
                                             | MS_SOURCED_AUDIO_SYNC[i]);
            encode_embedded_data();
        } else {
            uint8_t lcss;
            memcpy(buf + 20, pkt + 20, 33);
            lcss = get_embedded_data(buf + 20, frame_type & 0x0f);
            get_emb_data(buf + 20, lcss);
            memcpy(pkt + 20, buf + 20, 33);
        }
    } else {
        memset(pkt + 20, 0, 33);
    }

    peer_dmr_send(&b->dmr, pkt, 55);
    stamp_now(&b->last_dmr_tx);
    {
        static int tx_log;
        uint8_t ft, dtype;

        dmrd_parse_b15(frame_type, &ft, &dtype);
        if (dbg_periodic(&tx_log)
            || (ft == DMRD_FT_DATA_SYNC
                && (dtype == DMRD_DTYPE_VHEAD || dtype == DMRD_DTYPE_VTERM))) {
            LOG_DMR_DEBUG("DMR TX %s b15=0x%02x rf=%d gw=%d tg=%d seq=%u stream=0x%08x\n",
                      dmrd_class_label(pkt, 55), frame_type, src_id, b->dmr.dmrid,
                      b->dmr.tg, (unsigned)pkt[4], (unsigned)b->dmr_stream_id);
        }
    }
}

static void bridge_el_emit_dmr_voice(bridge_el_t *b, const uint8_t voice33[33])
{
    uint8_t slot_bit = b->dmr_slot_bit;
    uint8_t n = (uint8_t)(b->dmr_voice_frames % 6);
    uint8_t b15;

    if (!b->call_active) {
        b->call_active = 1;
        b->dmr_stream_id = new_stream_id();
        b->dmr_seq = 0;
        b->dmr_voice_frames = 0;
        modeconv_reset();
        bridge_el_resolve_el_talker(b);
        /* One VHEAD only: identical repeats are counted as loss by adn-server
         * PacketControl (duplicate CRC / lastData) and also create SEQ gaps. */
        bridge_el_send_dmrd(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD),
                            NULL);
        b->el.rtp_rx_packets = 0;
        LOG_DMR_INFO("EL->DMR call start (TG %d, src %.10s id %d)\n",
                     b->dmr.tg, b->net_src, b->el_rf_id);
    }

    b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4)) : (slot_bit | n));
    bridge_el_send_dmrd(b, b15, voice33);
    b->dmr_voice_frames++;
}

/* Begin paced teardown: pad to superframe then VTERM at DMR_FRAME_MS.
 * Bursting pads+VTERM in one tick was counted as SEQ/rate stress on short calls. */
static void bridge_el_begin_dmr_end(bridge_el_t *b)
{
    if (b->call_active != 1 || b->dmr_ending)
        return;
    b->dmr_ending = 1;
    b->el_ambe_count = 0;
    b->pcm_el_acc_n = 0;
    peer_el_drop_pcm_in(&b->el);
    modeconv_reset();
    /* Allow first pad/VTERM on the next tick immediately. */
    b->last_dmr_tx.tv_sec = 0;
    b->last_dmr_tx.tv_nsec = 0;
}

static void bridge_el_finish_dmr_end(bridge_el_t *b)
{
    LOG_DMR_INFO("EL->DMR call end (%d DMR frames out, el_rtp_rx=%u, seq=%u)\n",
             b->dmr_voice_frames, b->el.rtp_rx_packets, (unsigned)b->dmr_seq);
    b->call_active = 0;
    b->dmr_ending = 0;
    b->dmr_voice_frames = 0;
    b->el_ambe_count = 0;
    b->pcm_el_acc_n = 0;
    b->el_speech_run = 0;
    b->el_rf_id = 0;
    peer_el_drop_pcm_in(&b->el);
    modeconv_reset();
    stamp_now(&b->last_el_tx_end);
}

static void bridge_el_pace_dmr_end(bridge_el_t *b)
{
    uint8_t slot_bit = b->dmr_slot_bit;

    if (!b->dmr_ending || b->call_active != 1)
        return;
    if (!ms_elapsed(&b->last_dmr_tx, DMR_FRAME_MS))
        return;

    if ((b->dmr_voice_frames % 6) != 0) {
        uint8_t n = (uint8_t)(b->dmr_voice_frames % 6);
        uint8_t b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4))
                                       : (slot_bit | n));
        bridge_el_send_dmrd(b, b15, DMR_SILENCE_DATA);
        b->dmr_voice_frames++;
        return;
    }
    bridge_el_send_dmrd(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                        DMR_SILENCE_DATA);
    bridge_el_finish_dmr_end(b);
}

/* Immediate VTERM (no pad burst) — used when DMR RX preempts EL TX. */
static void bridge_el_end_dmr_call(bridge_el_t *b)
{
    uint8_t slot_bit = b->dmr_slot_bit;

    if (b->call_active != 1 && !b->dmr_ending)
        return;
    if (b->call_active == 1) {
        bridge_el_send_dmrd(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                            DMR_SILENCE_DATA);
    }
    bridge_el_finish_dmr_end(b);
}

void bridge_el_init(bridge_el_t *b, int mode, const char *dmr_options,
                    ysf2dmr_aliases_t *aliases, int bridge_dmrid)
{
    memset(b, 0, sizeof(*b));
    b->mode = mode;
    b->aliases = aliases;
    b->bridge_dmrid = bridge_dmrid;
    b->dmr_slot_bit = 0x80;
    memcpy(b->net_dst, YSF_WIRE_DST_ALL, 10);
    (void)dmr_options;
    modeconv_init();
    stamp_now(&b->last_dmr_tx);
    stamp_now(&b->last_dmr_rx);
    stamp_now(&b->last_ysf_tx);
    stamp_now(&b->last_el_speech);
    /* Cooldown inactive until first call ends (ms_since is large). */
    b->last_el_tx_end.tv_sec = 0;
    b->last_el_tx_end.tv_nsec = 0;
}

void bridge_el_process_el_audio(bridge_el_t *b)
{
    int16_t pcm[160];
    uint8_t ambe[7];
    int n;
    int enc_fail_streak = 0;

    if (b->mode != YSF2DMR_MODE_ECHOLINK_DMR)
        return;
    if (b->call_active == 2 || b->dmr_ending)
        return; /* DMR has the slot, or paced teardown in progress */

    while ((n = peer_el_read_pcm(&b->el, pcm, 160)) > 0) {
        int i;
        int rms;
        int in_cooldown;

        for (i = 0; i < n && b->pcm_el_acc_n < 160; i++)
            b->pcm_el_acc[b->pcm_el_acc_n++] = pcm[i];
        if (b->pcm_el_acc_n < 160)
            continue;

        rms = pcm_rms16(b->pcm_el_acc, 160);
        /* Any inbound EL PCM (incl. key-down silence) refreshes hang + may start TX. */
        stamp_now(&b->last_el_speech);

        in_cooldown = (b->last_el_tx_end.tv_sec || b->last_el_tx_end.tv_nsec)
                      && ms_since(&b->last_el_tx_end) < EL_TX_COOLDOWN_MS;
        if (!b->call_active) {
            if (in_cooldown) {
                static int drop_dbg;
                if (++drop_dbg <= 3 || (drop_dbg % 50) == 0)
                    LOG_EL_DEBUG("echolink: post-TX cooldown drop rms=%d\n", rms);
                b->pcm_el_acc_n = 0;
                b->el_ambe_count = 0;
                b->el_speech_run = 0;
                continue;
            }
            if (!b->el_speech_run) {
                LOG_EL_INFO("echolink: EL audio rms=%d — starting EL TX path "
                            "(TG activate)\n", rms);
                b->el_speech_run = 1;
            }
        }

        if (vocoder_encode(&b->voc, b->pcm_el_acc, ambe) != 0) {
            static int voc_enc_fail;
            b->pcm_el_acc_n = 0;
            enc_fail_streak++;
            if (dbg_periodic(&voc_enc_fail))
                LOG_DMR_WARNING("EL->DMR vocoder encode failed\n");
            /* Drop queued PCM so one wedged emu does not stall the main loop. */
            if (enc_fail_streak >= 3) {
                while (peer_el_read_pcm(&b->el, pcm, 160) > 0)
                    ;
                b->pcm_el_acc_n = 0;
                b->el_ambe_count = 0;
                break;
            }
            continue;
        }
        enc_fail_streak = 0;
        b->pcm_el_acc_n = 0;
        memcpy(b->el_ambe_buf[b->el_ambe_count], ambe, 7);
        b->el_ambe_count++;
        if (b->el_ambe_count < 3)
            continue;
        {
            static int el_tc_log;
            if (dbg_periodic(&el_tc_log)) {
                LOG_DMR_DEBUG("EL->DMR ambe raw=%02x%02x%02x%02x%02x%02x%02x "
                          "%02x%02x%02x%02x%02x%02x%02x "
                          "%02x%02x%02x%02x%02x%02x%02x (rms=%d)\n",
                          b->el_ambe_buf[0][0], b->el_ambe_buf[0][1], b->el_ambe_buf[0][2],
                          b->el_ambe_buf[0][3], b->el_ambe_buf[0][4], b->el_ambe_buf[0][5],
                          b->el_ambe_buf[0][6],
                          b->el_ambe_buf[1][0], b->el_ambe_buf[1][1], b->el_ambe_buf[1][2],
                          b->el_ambe_buf[1][3], b->el_ambe_buf[1][4], b->el_ambe_buf[1][5],
                          b->el_ambe_buf[1][6],
                          b->el_ambe_buf[2][0], b->el_ambe_buf[2][1], b->el_ambe_buf[2][2],
                          b->el_ambe_buf[2][3], b->el_ambe_buf[2][4], b->el_ambe_buf[2][5],
                          b->el_ambe_buf[2][6], rms);
            }
        }
        /* Queue into ModeConv; tick paces UDP TX at DMR_FRAME_MS. */
        modeconv_put_ambe7(b->el_ambe_buf[0]);
        modeconv_put_ambe7(b->el_ambe_buf[1]);
        modeconv_put_ambe7(b->el_ambe_buf[2]);
        b->el_ambe_count = 0;
    }
}

void bridge_el_on_dmrd(bridge_el_t *b, const uint8_t *pkt, int len)
{
    uint8_t ft, dtype;
    uint8_t ambe[3][7];
    int16_t pcm[160];
    int i;
    int rf, dst;
    static int rx_log;

    if (b->mode != YSF2DMR_MODE_ECHOLINK_DMR)
        return;
    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0) {
        LOG_DMR_DEBUG("DMR RX ignore len=%d (expected DMRD 55)\n", len);
        return;
    }

    rf = (pkt[5] << 16) | (pkt[6] << 8) | pkt[7];
    dst = (pkt[8] << 16) | (pkt[9] << 8) | pkt[10];
    dmrd_parse_b15(pkt[15], &ft, &dtype);
    if (dbg_periodic(&rx_log) || dmrd_is_header(pkt, len) || dmrd_is_terminator(pkt, len)) {
        LOG_DMR_DEBUG("DMR RX %s b15=0x%02x rf=%d dst=%d gw=%u seq=%u stream=0x%08x active=%d\n",
                  dmrd_class_label(pkt, len), pkt[15], rf, dst,
                  (unsigned)((pkt[11] << 24) | (pkt[12] << 16) | (pkt[13] << 8) | pkt[14]),
                  (unsigned)pkt[4],
                  (unsigned)*(const uint32_t *)(pkt + 16),
                  b->call_active);
    }

    if (dmrd_is_header(pkt, len)) {
        uint32_t sid = *(const uint32_t *)(pkt + 16);

        /* Duplicate VHEAD on the active stream — do not reset counters/RTP. */
        if (b->call_active == 2 && sid == b->dmr_rx_stream_id) {
            stamp_now(&b->last_dmr_rx);
            return;
        }
        if (b->call_active == 1)
            bridge_el_end_dmr_call(b);
        if (b->call_active == 2) {
            peer_el_flush_pcm(&b->el);
            LOG_DMR_INFO("DMR->EL call end (%d voice frames in, el_rtp_tx=%u) — replaced by new stream\n",
                     b->dmr_voice_frames, b->el.rtp_tx_packets);
        }
        b->call_active = 2;
        b->dmr_rx_stream_id = sid;
        b->dmr_voice_frames = 0;
        b->el.rtp_tx_packets = 0;
        stamp_now(&b->last_dmr_rx);
        LOG_DMR_INFO("DMR->EL call start (TG %d)\n", b->dmr.tg);
        return;
    }
    if (dmrd_is_terminator(pkt, len)) {
        if (b->call_active == 2) {
            peer_el_flush_pcm(&b->el);
            LOG_DMR_INFO("DMR->EL call end (%d voice frames in, el_rtp_tx=%u)\n",
                     b->dmr_voice_frames, b->el.rtp_tx_packets);
            b->call_active = 0;
            b->dmr_voice_frames = 0;
            b->dmr_rx_stream_id = 0;
        }
        return;
    }
    if (b->call_active != 2)
        return;

    stamp_now(&b->last_dmr_rx);
    b->dmr_voice_frames++;
    modeconv_dmr33_to_ambe(pkt + 20, ambe);
    {
        static int dmr_tc_log;
        if (dbg_periodic(&dmr_tc_log)) {
            LOG_DMR_DEBUG("DMR->EL ambe raw=%02x%02x%02x%02x%02x%02x%02x "
                      "%02x%02x%02x%02x%02x%02x%02x "
                      "%02x%02x%02x%02x%02x%02x%02x (voice_in=%d)\n",
                      ambe[0][0], ambe[0][1], ambe[0][2], ambe[0][3],
                      ambe[0][4], ambe[0][5], ambe[0][6],
                      ambe[1][0], ambe[1][1], ambe[1][2], ambe[1][3],
                      ambe[1][4], ambe[1][5], ambe[1][6],
                      ambe[2][0], ambe[2][1], ambe[2][2], ambe[2][3],
                      ambe[2][4], ambe[2][5], ambe[2][6],
                      b->dmr_voice_frames);
        }
    }
    for (i = 0; i < 3; i++) {
        if (vocoder_decode(&b->voc, ambe[i], pcm) != 0) {
            static int voc_fail;
            if (dbg_periodic(&voc_fail))
                LOG_DMR_WARNING("DMR->EL vocoder decode failed\n");
            continue;
        }
        peer_el_write_pcm(&b->el, pcm, 160);
    }
}

/* Same connect-PTT flow as bridge.c (YSF<->DMR). */
static void bridge_el_connect_ptt_finish(bridge_el_t *b)
{
    uint8_t slot_bit = b->dmr_slot_bit;

    while ((b->connect_ptt_voice_frames % 6) != 0) {
        uint8_t n = (uint8_t)(b->connect_ptt_voice_frames % 6);
        bridge_el_send_dmrd(b, (uint8_t)(slot_bit | n), DMR_SILENCE_DATA);
        b->connect_ptt_voice_frames++;
    }
    bridge_el_send_dmrd(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                        DMR_SILENCE_DATA);
    LOG_DMR_INFO("DMR connect PTT end (TG %d, %d voice frames)\n",
             b->dmr.tg, b->connect_ptt_voice_frames);
    b->connect_ptt_active = 0;
    b->connect_ptt_phase = 0;
    b->connect_ptt_voice_frames = 0;
}

static void bridge_el_start_connect_ptt(bridge_el_t *b)
{
    if (b->call_active || b->connect_ptt_active)
        return;
    if (b->dmr.tg <= 0)
        return;

    b->connect_ptt_active = 1;
    b->connect_ptt_phase = 0;
    b->connect_ptt_voice_frames = 0;
    b->dmr_stream_id = new_stream_id();
    b->dmr_seq = 0;
    stamp_now(&b->connect_ptt_start);
    stamp_now(&b->last_dmr_tx);
    b->last_dmr_tx.tv_sec = 0; /* force first emit immediately */
    LOG_DMR_INFO("DMR connect PTT start (TG %d, %d ms)\n", b->dmr.tg, CONNECT_PTT_MS);
}

static void bridge_el_emit_connect_ptt(bridge_el_t *b)
{
    uint8_t slot_bit = b->dmr_slot_bit;

    if (!b->connect_ptt_active)
        return;

    if (b->connect_ptt_phase == 0) {
        bridge_el_send_dmrd(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD),
                            NULL);
        b->connect_ptt_phase = 1;
        stamp_now(&b->connect_ptt_start);
        return;
    }

    if (ms_since(&b->connect_ptt_start) >= CONNECT_PTT_MS) {
        bridge_el_connect_ptt_finish(b);
        return;
    }

    {
        uint8_t n = (uint8_t)(b->connect_ptt_voice_frames % 6);
        uint8_t b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4))
                                       : (slot_bit | n));
        bridge_el_send_dmrd(b, b15, DMR_SILENCE_DATA);
        b->connect_ptt_voice_frames++;
    }
}

static void bridge_el_poll_connect_ptt(bridge_el_t *b)
{
    int connected = peer_dmr_connected(&b->dmr);

    if (connected && !b->dmr_was_connected)
        bridge_el_start_connect_ptt(b);
    if (!connected) {
        b->connect_ptt_active = 0;
        b->connect_ptt_phase = 0;
        b->connect_ptt_voice_frames = 0;
    }
    b->dmr_was_connected = connected;

    if (b->connect_ptt_active && ms_elapsed(&b->last_dmr_tx, DMR_FRAME_MS))
        bridge_el_emit_connect_ptt(b);
}

/* ---- EchoLink <-> YSF (framing mirrors bridge.c DMR2YSF path) ---- */

static const char *ysf_fi_name(uint8_t fi)
{
    switch (fi) {
    case YSF_FI_HEADER:         return "HDR";
    case YSF_FI_COMMUNICATIONS: return "VOICE";
    case YSF_FI_TERMINATOR:     return "EOT";
    default:                    return "?";
    }
}

static void dbg_label10(char out[11], const uint8_t raw[10])
{
    int i;

    for (i = 0; i < 10; i++)
        out[i] = (raw[i] >= 32 && raw[i] < 127) ? (char)raw[i] : '.';
    out[10] = '\0';
}

void bridge_el_format_callsign10(char out[10], const char *src)
{
    int i, j = 0;

    memset(out, ' ', 10);
    if (!src)
        return;
    /* Keep full EchoLink callsign including -L/-R (e.g. CE5RPY-L). */
    for (i = 0; src[i] && j < 10; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c == ' ' || c == '\t')
            continue;
        out[j++] = (char)toupper(c);
    }
}

/* DMR2YSF/YSF2DMR default RadioID in CSD/DCH (not configurable). */
static void radio_id_to_dch5(uint8_t out[5])
{
    memset(out, '*', 5);
}

/* Trim YSF/DMR wire callsign (10 chars, space-padded) for EchoLink SDES NAME. */
static void wire_call_to_cstr(char out[16], const char src[10])
{
    int i, n = 0;

    out[0] = '\0';
    if (!src)
        return;
    for (i = 0; i < 10 && n < 15; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c == '\0')
            break;
        if (c == ' ' || c == '\t') {
            if (n == 0)
                continue;
            break;
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
}

static void bridge_el_set_ysf_talker_on_el(bridge_el_t *b)
{
    char talker[16];
    char name[32];

    wire_call_to_cstr(talker, b->net_src);
    if (!talker[0]) {
        peer_el_set_talker_name(&b->el, NULL);
        return;
    }
    /* Conference-style NAME: "CE5RPY-L (HP3ICC)" — clients parse talker in parens. */
    snprintf(name, sizeof(name), "%s (%s)", b->el.callsign, talker);
    peer_el_set_talker_name(&b->el, name);
}

static int ysf_pkt_has_rf_sync(const uint8_t *pkt155)
{
    static const uint8_t sync[5] = {0xD4U, 0x71U, 0xC9U, 0x63U, 0x4DU};
    return memcmp(pkt155 + YSF_FICH_OFFSET_NET, sync, 5) == 0;
}

static void ysf_net_to_rf120(const uint8_t *pkt155, uint8_t rf120[120])
{
    const uint8_t *net = pkt155 + YSF_FICH_OFFSET_NET;

    memset(rf120, 0, 120);
    memcpy(rf120, YSF_SYNC_BYTES, 5);
    memcpy(rf120 + 5, net, 115);
}

static const uint8_t *ysf_modeconv_chunk(const uint8_t *pkt155, uint8_t scratch[120])
{
    if (ysf_pkt_has_rf_sync(pkt155))
        return pkt155 + YSF_FICH_OFFSET_NET;
    ysf_net_to_rf120(pkt155, scratch);
    return scratch;
}

static void bridge_el_fill_ysf_csd(bridge_el_t *b, uint8_t csd1[20], uint8_t csd2[20])
{
    /* Byte-identical to bridge_fill_ysf_csd / DMR2YSF: *****|RadioID|src. */
    uint8_t rid[5];

    memset(csd2, ' ', 20);
    memset(csd1, '*', 5);
    radio_id_to_dch5(rid);
    memcpy(csd1 + 5, rid, 5);
    memcpy(csd1 + 10, b->net_src, 10);
}

static void bridge_el_apply_ysf_dch_slot(uint8_t *payload, uint8_t fn, bridge_el_t *b)
{
    /* Byte-identical FN slots to bridge_apply_ysf_dch_slot / DMR2YSF. */
    uint8_t dch[10];
    uint8_t rid[5];

    memset(dch, ' ', 10);
    radio_id_to_dch5(rid);
    switch (fn) {
    case 0:
        memset(dch, '*', 5);
        memcpy(dch + 5, rid, 5);
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    case 1:
        memcpy(dch, b->net_src, 10);
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    case 2:
        memcpy(dch, b->net_dst, 10);
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    case 5:
        memset(dch, ' ', 5);
        memcpy(dch + 5, rid, 5);
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    case 6:
        memcpy(dch, YSF_DCH_DT1, 10);
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    case 7:
        memcpy(dch, YSF_DCH_DT2, 10);
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    default:
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    }
}

static void bridge_el_fill_ysfd_headers(uint8_t *frame, bridge_el_t *b)
{
    /* Same YSFD envelope as bridge_fill_ysfd_headers. */
    memcpy(frame, "YSFD", 4);
    memcpy(frame + 4, b->ysf.callsign, 10);
    memcpy(frame + 14, b->net_src, 10);
    memcpy(frame + 24, YSF_WIRE_DST_ALL, 10);
    frame[34] = 0;
}

static int bridge_el_send_ysfd(bridge_el_t *b, uint8_t fi, uint8_t ft, uint8_t cm,
                               uint8_t fich_fn, uint8_t net_cnt,
                               const uint8_t *payload120,
                               const uint8_t csd1[20], const uint8_t csd2[20])
{
    uint8_t frame[155];

    bridge_el_fill_ysfd_headers(frame, b);
    frame[34] = net_cnt;
    /* Wire layout: sync at +35, FICH at +40 — same order as bridge_send_ysfd. */
    if (fi == YSF_FI_HEADER || fi == YSF_FI_TERMINATOR) {
        memset(frame + YSF_FICH_OFFSET_NET, 0, 120);
        memcpy(frame + YSF_FICH_OFFSET_NET, YSF_SYNC_BYTES, 5);
        ysf_fich_encode_outbound(frame + YSF_FICH_OFFSET_RX, fich_fn, fi, ft, cm);
        if (csd1 && csd2)
            ysf_payload_write_header(frame + YSF_FICH_OFFSET_NET, csd1, csd2);
    } else {
        memcpy(frame + YSF_FICH_OFFSET_NET, payload120, 120);
        memcpy(frame + YSF_FICH_OFFSET_NET, YSF_SYNC_BYTES, 5);
        /* YSF2DMR: sync, DCH slot, FICH */
        bridge_el_apply_ysf_dch_slot(frame + YSF_FICH_OFFSET_NET, fich_fn, b);
        ysf_fich_encode_outbound(frame + YSF_FICH_OFFSET_RX, fich_fn, fi, ft, cm);
    }
    peer_ysf_send_ysfd(&b->ysf, frame, 155);
    stamp_now(&b->last_ysf_tx);
    {
        static int tx_log;
        uint8_t wire_fi, wire_fn, wire_ft, wire_cm, wire_dt;
        int log_tx = (fi == YSF_FI_HEADER || fi == YSF_FI_TERMINATOR
                      || (fi == YSF_FI_COMMUNICATIONS && fich_fn <= 1U)
                      || dbg_periodic(&tx_log));

        if (log_tx
            && ysf_fich_decode_fields(frame, &wire_fi, &wire_fn, &wire_ft,
                                      &wire_cm, &wire_dt) == 0) {
            LOG_YSF_DEBUG("YSF TX %s fi=%u ft=%u fn=%u cm=%u dt=%u net=%3u src=%.10s "
                      "dst=%.10s dgid_cfg=%u\n",
                      ysf_fi_name(fi), (unsigned)wire_fi, (unsigned)ft,
                      (unsigned)wire_fn, (unsigned)wire_cm, (unsigned)wire_dt,
                      (unsigned)net_cnt, frame + 14, frame + 24,
                      (unsigned)b->ysf.dgid);
        }
    }
    return 1;
}

/* Drain one ModeConv YSF tag — same control flow as bridge_emit_ysf_from_conv. */
static int bridge_el_emit_ysf_from_conv(bridge_el_t *b)
{
    uint8_t payload[120];
    unsigned int tag;

    memset(payload, 0, sizeof(payload));
    tag = modeconv_get_ysf(payload);
    if (tag == MODECONV_TAG_NODATA)
        return 0;

    if (tag == MODECONV_TAG_HEADER) {
        uint8_t csd1[20], csd2[20];

        b->ysf_cnt = 0;
        bridge_el_fill_ysf_csd(b, csd1, csd2);
        bridge_el_send_ysfd(b, YSF_FI_HEADER, YSF_FICH_FT, YSF_FICH_CM, 0, 0,
                            NULL, csd1, csd2);
        b->ysf_cnt = 1;
        return 1;
    }
    if (tag == MODECONV_TAG_EOT) {
        uint8_t csd1[20], csd2[20];

        bridge_el_fill_ysf_csd(b, csd1, csd2);
        bridge_el_send_ysfd(b, YSF_FI_TERMINATOR, YSF_FICH_FT, YSF_FICH_CM, 0,
                            b->ysf_cnt, NULL, csd1, csd2);
        LOG_YSF_INFO("EL->YSF call end (%d voice frames out, el_rtp_rx=%u)\n",
                     b->ysf_voice_frames, b->el.rtp_rx_packets);
        b->call_active = 0;
        b->ysf_ending = 0;
        b->ysf_voice_frames = 0;
        b->ysf_ambe_count = 0;
        b->ysf_cnt = 0;
        b->pcm_el_acc_n = 0;
        b->el_speech_run = 0;
        b->el_rf_id = 0;
        peer_el_drop_pcm_in(&b->el);
        modeconv_reset();
        stamp_now(&b->last_el_tx_end);
        return 1;
    }
    if (tag == MODECONV_TAG_DATA) {
        uint8_t fn = (uint8_t)((b->ysf_cnt - 1U) % (YSF_FICH_FT + 1U));
        uint8_t net = (uint8_t)((b->ysf_cnt & 0x7FU) << 1);

        bridge_el_send_ysfd(b, YSF_FI_COMMUNICATIONS, YSF_FICH_FT, YSF_FICH_CM,
                            fn, net, payload, NULL, NULL);
        b->ysf_voice_frames++;
        b->ysf_cnt++;
        return 1;
    }
    return 0;
}

static void bridge_el_end_ysf_call(bridge_el_t *b)
{
    if (b->call_active != 1 || b->ysf_ending)
        return;
    /* Same as DMR→YSF: queue ModeConv EOT; HEADER/CSD/EOT leave on paced emit. */
    b->ysf_ending = 1;
    modeconv_put_dmr_eot();
}

static void bridge_el_begin_el_to_ysf(bridge_el_t *b)
{
    /*
     * Identity slots identical to DMR→YSF:
     *   CSD/DCH RadioID = *****
     *   wire dst = ALL
     *   net_src = remote EchoLink talker (inbound SDES), else connected node
     * HEADER is queued like putDMRHeader; CSD bytes filled on emit.
     */
    bridge_el_resolve_el_talker(b);
    memcpy(b->net_dst, YSF_WIRE_DST_ALL, 10);
    b->call_active = 1;
    b->ysf_ending = 0;
    b->ysf_voice_frames = 0;
    b->ysf_ambe_count = 0;
    b->pcm_el_acc_n = 0;
    b->ysf_cnt = 0;
    modeconv_reset();
    modeconv_put_dmr_header();
    b->el.rtp_rx_packets = 0;
    LOG_YSF_INFO("EL->YSF call start (src %.10s)\n", b->net_src);
}

static void bridge_el_drain_ysf_to_el_pcm(bridge_el_t *b)
{
    uint8_t voice33[33];
    uint8_t ambe[3][7];
    int16_t pcm[160];
    unsigned int tag;
    int i;

    while ((tag = modeconv_get_dmr(voice33)) != MODECONV_TAG_NODATA) {
        if (tag != MODECONV_TAG_DATA)
            continue;
        modeconv_dmr33_to_ambe(voice33, ambe);
        for (i = 0; i < 3; i++) {
            if (vocoder_decode(&b->voc, ambe[i], pcm) != 0) {
                static int voc_fail;
                if (dbg_periodic(&voc_fail))
                    LOG_YSF_WARNING("YSF->EL vocoder decode failed\n");
                continue;
            }
            peer_el_write_pcm(&b->el, pcm, 160);
        }
    }
}

void bridge_el_process_el_to_ysf(bridge_el_t *b)
{
    int16_t pcm[160];
    uint8_t ambe[7];
    int n;
    int enc_fail_streak = 0;

    if (b->mode != YSF2DMR_MODE_ECHOLINK_YSF)
        return;
    if (b->call_active == 2 || b->ysf_ending)
        return; /* YSF RX has the slot, or EOT drain in progress */

    while ((n = peer_el_read_pcm(&b->el, pcm, 160)) > 0) {
        int i;
        int rms;
        int in_cooldown;

        for (i = 0; i < n && b->pcm_el_acc_n < 160; i++)
            b->pcm_el_acc[b->pcm_el_acc_n++] = pcm[i];
        if (b->pcm_el_acc_n < 160)
            continue;

        rms = pcm_rms16(b->pcm_el_acc, 160);
        /* Any inbound EL PCM (incl. silence) refreshes hang + may start TX. */
        stamp_now(&b->last_el_speech);

        in_cooldown = (b->last_el_tx_end.tv_sec || b->last_el_tx_end.tv_nsec)
                      && ms_since(&b->last_el_tx_end) < EL_TX_COOLDOWN_MS;
        if (!b->call_active) {
            if (in_cooldown) {
                static int drop_dbg;
                if (++drop_dbg <= 3 || (drop_dbg % 50) == 0)
                    LOG_EL_DEBUG("echolink: post-TX cooldown drop rms=%d (YSF)\n",
                                 rms);
                b->pcm_el_acc_n = 0;
                b->ysf_ambe_count = 0;
                b->el_speech_run = 0;
                continue;
            }
            if (!b->el_speech_run) {
                LOG_EL_INFO("echolink: EL audio rms=%d — starting EL->YSF path\n",
                            rms);
                b->el_speech_run = 1;
            }
        }

        if (vocoder_encode(&b->voc, b->pcm_el_acc, ambe) != 0) {
            static int voc_enc_fail;

            b->pcm_el_acc_n = 0;
            enc_fail_streak++;
            if (dbg_periodic(&voc_enc_fail))
                LOG_YSF_WARNING("EL->YSF vocoder encode failed\n");
            if (enc_fail_streak >= 3) {
                while (peer_el_read_pcm(&b->el, pcm, 160) > 0)
                    ;
                b->pcm_el_acc_n = 0;
                b->ysf_ambe_count = 0;
                break;
            }
            continue;
        }
        enc_fail_streak = 0;
        b->pcm_el_acc_n = 0;

        if (!b->call_active)
            bridge_el_begin_el_to_ysf(b);

        memcpy(b->ysf_ambe_buf[b->ysf_ambe_count], ambe, 7);
        b->ysf_ambe_count++;
        if (b->ysf_ambe_count < 5)
            continue;

        {
            static int el_ysf_log;
            if (dbg_periodic(&el_ysf_log)) {
                LOG_YSF_DEBUG("EL->YSF ambe raw=%02x%02x%02x%02x%02x%02x%02x "
                          "(voice_out=%d rms=%d)\n",
                          b->ysf_ambe_buf[0][0], b->ysf_ambe_buf[0][1],
                          b->ysf_ambe_buf[0][2], b->ysf_ambe_buf[0][3],
                          b->ysf_ambe_buf[0][4], b->ysf_ambe_buf[0][5],
                          b->ysf_ambe_buf[0][6], b->ysf_voice_frames, rms);
            }
        }
        /* Queue only — YSFD HEADER/VOICE/EOT leave via paced emit (DMR→YSF). */
        for (i = 0; i < 5; i++)
            modeconv_put_ambe7_ysf(b->ysf_ambe_buf[i]);
        b->ysf_ambe_count = 0;
    }
}

void bridge_el_on_ysfd(bridge_el_t *b, const uint8_t *pkt, int len)
{
    uint8_t fi, fn, ft, cm, dt;
    uint8_t rx_dgid;
    char rpt[11], src[11];
    uint8_t scratch[120];

    if (b->mode != YSF2DMR_MODE_ECHOLINK_YSF)
        return;
    if (len != 155 || memcmp(pkt, "YSFD", 4) != 0)
        return;
    if (ysf_fich_decode_fields(pkt, &fi, &fn, &ft, &cm, &dt) != 0) {
        LOG_YSF_DEBUG("YSF RX FICH decode failed\n");
        return;
    }

    rx_dgid = ysf_fich_get_dgid();
    dbg_label10(rpt, pkt + 4);
    dbg_label10(src, pkt + 14);
    LOG_YSF_DEBUG("YSF RX %s fi=%u fn=%u ft=%u cm=%u dt=%u dgid=%u rpt=%s src=%s\n",
              ysf_fi_name(fi), (unsigned)fi, (unsigned)fn, (unsigned)ft,
              (unsigned)cm, (unsigned)dt, (unsigned)rx_dgid, rpt, src);

    if (b->ysf.dgid >= 1U && rx_dgid >= 1U && rx_dgid != b->ysf.dgid) {
        LOG_YSF_DEBUG("YSF RX skip DGID %u (want %u)\n",
                  (unsigned)rx_dgid, (unsigned)b->ysf.dgid);
        return;
    }

    if (fi == YSF_FI_HEADER) {
        if (b->call_active == 1)
            bridge_el_end_ysf_call(b);
        if (b->call_active == 2) {
            peer_el_flush_pcm(&b->el);
            peer_el_set_talker_name(&b->el, NULL);
            LOG_YSF_INFO("YSF->EL call end (%d voice frames in, el_rtp_tx=%u) "
                     "— replaced by new stream\n",
                     b->ysf_voice_frames, b->el.rtp_tx_packets);
        }
        memcpy(b->net_src, pkt + 14, 10);
        memcpy(b->net_dst, YSF_WIRE_DST_ALL, 10);
        b->call_active = 2;
        b->ysf_voice_frames = 0;
        b->el.rtp_tx_packets = 0;
        stamp_now(&b->last_dmr_rx); /* reuse: last peer->EL activity */
        modeconv_reset();
        modeconv_put_ysf_header();
        bridge_el_set_ysf_talker_on_el(b);
        LOG_YSF_INFO("YSF->EL call start (src %.10s)\n", b->net_src);
        return;
    }

    if (fi == YSF_FI_TERMINATOR) {
        if (b->call_active == 2) {
            modeconv_put_ysf_eot();
            bridge_el_drain_ysf_to_el_pcm(b);
            peer_el_flush_pcm(&b->el);
            peer_el_set_talker_name(&b->el, NULL);
            LOG_YSF_INFO("YSF->EL call end (%d voice frames in, el_rtp_tx=%u)\n",
                     b->ysf_voice_frames, b->el.rtp_tx_packets);
            b->call_active = 0;
            b->ysf_voice_frames = 0;
            modeconv_reset();
        }
        return;
    }

    if (fi != YSF_FI_COMMUNICATIONS)
        return;
    if (b->call_active != 2) {
        /* Late join without HEADER — start on first voice. */
        if (b->call_active == 1)
            bridge_el_end_ysf_call(b);
        memcpy(b->net_src, pkt + 14, 10);
        b->call_active = 2;
        b->ysf_voice_frames = 0;
        b->el.rtp_tx_packets = 0;
        modeconv_reset();
        modeconv_put_ysf_header();
        bridge_el_set_ysf_talker_on_el(b);
        LOG_YSF_INFO("YSF->EL call start (src %.10s, no HEADER)\n", b->net_src);
    }
    if (dt != YSF_DT_VD_MODE2) {
        LOG_YSF_DEBUG("YSF VOICE dt=%u (expect VD2; using repack+putYSF)\n",
                  (unsigned)dt);
    }
    stamp_now(&b->last_dmr_rx);
    modeconv_put_ysf_payload(ysf_modeconv_chunk(pkt, scratch));
    b->ysf_voice_frames++;
    bridge_el_drain_ysf_to_el_pcm(b);
}

static void bridge_el_pace_dmr_tx(bridge_el_t *b)
{
    uint8_t voice33[33];

    if (b->mode != YSF2DMR_MODE_ECHOLINK_DMR)
        return;
    if (b->call_active == 2 || b->connect_ptt_active || b->dmr_ending)
        return;
    /* Pace only after the call has started; first frame may start immediately. */
    if (b->call_active == 1 && !ms_elapsed(&b->last_dmr_tx, DMR_FRAME_MS))
        return;
    if (modeconv_get_dmr(voice33) == MODECONV_TAG_DATA)
        bridge_el_emit_dmr_voice(b, voice33);
}

void bridge_el_tick(bridge_el_t *b)
{
    if (b->mode == YSF2DMR_MODE_ECHOLINK_DMR) {
        bridge_el_poll_connect_ptt(b);
        if (b->dmr_ending)
            bridge_el_pace_dmr_end(b);
        else
            bridge_el_pace_dmr_tx(b);
    }

    /* EL→YSF: one YSFD every 90 ms (identical pacing to DMR→YSF). */
    if (b->mode == YSF2DMR_MODE_ECHOLINK_YSF && b->call_active == 1
        && ms_elapsed(&b->last_ysf_tx, YSF_FRAME_MS))
        (void)bridge_el_emit_ysf_from_conv(b);

    /* End EL->DMR/YSF when inbound EL PCM stops (silence still holds while RTP). */
    if (b->call_active == 1 && !b->dmr_ending && !b->ysf_ending
        && ms_since(&b->last_el_speech) >= EL_HANG_MS) {
        if (b->mode == YSF2DMR_MODE_ECHOLINK_DMR)
            bridge_el_begin_dmr_end(b);
        else if (b->mode == YSF2DMR_MODE_ECHOLINK_YSF)
            bridge_el_end_ysf_call(b);
    }

    /*
     * DMR/YSF->EL: if the stream dies without VTERM/EOT, release the
     * half-duplex lock so EL TX can run again.
     */
    if (b->call_active == 2) {
        if (b->mode == YSF2DMR_MODE_ECHOLINK_DMR
            && ms_since(&b->last_dmr_rx) >= DMR_RX_HANG_MS) {
            peer_el_flush_pcm(&b->el);
            LOG_DMR_INFO("DMR->EL call end (%d voice frames in, el_rtp_tx=%u) — RX hangtime\n",
                         b->dmr_voice_frames, b->el.rtp_tx_packets);
            b->call_active = 0;
            b->dmr_voice_frames = 0;
            b->dmr_rx_stream_id = 0;
        } else if (b->mode == YSF2DMR_MODE_ECHOLINK_YSF
                   && ms_since(&b->last_dmr_rx) >= YSF_RX_HANG_MS) {
            peer_el_flush_pcm(&b->el);
            peer_el_set_talker_name(&b->el, NULL);
            LOG_YSF_INFO("YSF->EL call end (%d voice frames in, el_rtp_tx=%u) — RX hangtime\n",
                         b->ysf_voice_frames, b->el.rtp_tx_packets);
            b->call_active = 0;
            b->ysf_voice_frames = 0;
            modeconv_reset();
        }
    }
}
