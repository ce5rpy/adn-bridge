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
#include "adapters/el.h"
#include "config.h"
#include "hbp/dmr_codec.h"
#include "log.h"
#include "media/bridge_util.h"
#include "media/call_meta.h"
#include "media/identity.h"
#include "mmdvm/modeconv_wrap.h"
#include "session/dmr_tx.h"
#include "session/dmr_wire.h"
#include "session/ysf_tx.h"
#include "ysf_fich.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BRIDGE_CALL_META(b) ((bridge_call_meta_t *)&(b)->net_src)

#define DMR_FRAME_MS   60
#define YSF_FRAME_MS   90 /* same pace as DMR→YSF (bridge.c / DMR2YSF) */
#define CONNECT_PTT_MS 500
#define DMR_CLEAR_DYNAMIC_TG 4000

static int bridge_el_tx_tg(const bridge_el_t *b)
{
    if (b->connect_ptt_active && b->connect_ptt_tg > 0)
        return b->connect_ptt_tg;
    return b->dmr.tg;
}
/* End EL->DMR/YSF after this much without inbound EL PCM (key-down silence
 * must still hold / activate the TG; hang follows PCM presence, not RMS). */
#define EL_HANG_MS     700
/* After EL->DMR/YSF end, ignore residual conference PCM (no phantom reopen). */
#define EL_TX_COOLDOWN_MS 800
/* DMR/YSF->EL without VTERM/EOT used to leave call_active=2 and block EL TX. */
#define DMR_RX_HANG_MS 1500
#define YSF_RX_HANG_MS 1500

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

/* Scale PCM in place; clamp to int16. gain==1.0 is a no-op. */
static void pcm_apply_gain(int16_t *pcm, int n, float gain)
{
    int i;

    if (!pcm || n <= 0 || gain == 1.0f)
        return;
    for (i = 0; i < n; i++) {
        float v = (float)pcm[i] * gain;

        if (v > 32767.0f)
            v = 32767.0f;
        else if (v < -32768.0f)
            v = -32768.0f;
        pcm[i] = (int16_t)v;
    }
}

static void bridge_el_send_dmrd(bridge_el_t *b, uint8_t frame_type, const uint8_t *voice33)
{
    dmr_tx_args_t args;
    int rf_id = (b->el_rf_id > 0) ? b->el_rf_id : b->dmr.dmrid;

    args.peer = &b->dmr;
    args.bridge_dmrid = b->dmr.dmrid;
    args.talker_rf_id = rf_id;
    args.tx_tg = bridge_el_tx_tg(b);
    args.seq = &b->dmr_seq;
    args.stream_id = b->dmr_stream_id;
    args.last_tx = &b->last_dmr_tx;
    dmr_tx_send(&args, frame_type, voice33);
}

static void bridge_el_emit_dmr_voice(bridge_el_t *b, const uint8_t voice33[33])
{
    uint8_t slot_bit = b->dmr_slot_bit;
    uint8_t n = (uint8_t)(b->dmr_voice_frames % 6);
    uint8_t b15;

    if (!b->call_active) {
        b->call_active = 1;
        b->dmr_stream_id = bridge_new_stream_id();
        b->dmr_seq = 0;
        b->dmr_voice_frames = 0;
        modeconv_reset();
        adapter_el_resolve_talker(b);
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
    peer_el_clear_remote_talker(&b->el);
    modeconv_reset();
    bridge_stamp_now(&b->last_el_tx_end);
}

static void bridge_el_pace_dmr_end(bridge_el_t *b)
{
    uint8_t slot_bit = b->dmr_slot_bit;

    if (!b->dmr_ending || b->call_active != 1)
        return;
    if (!bridge_ms_elapsed(&b->last_dmr_tx, DMR_FRAME_MS))
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
                    adn_bridge_aliases_t *aliases, int bridge_dmrid,
                    float el_pcm_gain, int clear_dynamic_tg)
{
    memset(b, 0, sizeof(*b));
    b->mode = mode;
    b->aliases = aliases;
    b->bridge_dmrid = bridge_dmrid;
    b->el_pcm_gain = (el_pcm_gain > 0.0f && el_pcm_gain <= 4.0f) ? el_pcm_gain : 1.0f;
    b->clear_dynamic_tg = clear_dynamic_tg ? 1 : 0;
    b->dmr_slot_bit = 0x80;
    memcpy(b->net_dst, YSF_WIRE_DST_ALL, 10);
    (void)dmr_options;
    modeconv_init();
    bridge_stamp_now(&b->last_dmr_tx);
    bridge_stamp_now(&b->last_dmr_rx);
    bridge_stamp_now(&b->last_ysf_tx);
    bridge_stamp_now(&b->last_el_speech);
    /* Cooldown inactive until first call ends (bridge_ms_since is large). */
    b->last_el_tx_end.tv_sec = 0;
    b->last_el_tx_end.tv_nsec = 0;
}

void bridge_el_process_el_audio(bridge_el_t *b)
{
    int16_t pcm[160];
    uint8_t ambe[7];
    int n;
    int enc_fail_streak = 0;

    if (b->mode != ADN_BRIDGE_MODE_ECHOLINK_DMR)
        return;
    /* Mirror YSF↔DMR: do not queue EL audio toward DMR while HBP is down. */
    if (!peer_dmr_connected(&b->dmr))
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
        bridge_stamp_now(&b->last_el_speech);

        in_cooldown = (b->last_el_tx_end.tv_sec || b->last_el_tx_end.tv_nsec)
                      && bridge_ms_since(&b->last_el_tx_end) < EL_TX_COOLDOWN_MS;
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

        pcm_apply_gain(b->pcm_el_acc, 160, b->el_pcm_gain);
        if (vocoder_encode(&b->voc, b->pcm_el_acc, ambe) != 0) {
            static int voc_enc_fail;
            b->pcm_el_acc_n = 0;
            enc_fail_streak++;
            if (bridge_dbg_periodic(&voc_enc_fail))
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
            if (bridge_dbg_periodic(&el_tc_log)) {
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

    if (b->mode != ADN_BRIDGE_MODE_ECHOLINK_DMR)
        return;
    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0) {
        LOG_DMR_DEBUG("DMR RX ignore len=%d (expected DMRD 55)\n", len);
        return;
    }

    rf = (pkt[5] << 16) | (pkt[6] << 8) | pkt[7];
    dst = (pkt[8] << 16) | (pkt[9] << 8) | pkt[10];
    dmrd_parse_b15(pkt[15], &ft, &dtype);
    if (bridge_dbg_periodic(&rx_log) || dmrd_is_header(pkt, len) || dmrd_is_terminator(pkt, len)) {
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
            bridge_stamp_now(&b->last_dmr_rx);
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
        bridge_stamp_now(&b->last_dmr_rx);
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

    bridge_stamp_now(&b->last_dmr_rx);
    b->dmr_voice_frames++;
    modeconv_dmr33_to_ambe(pkt + 20, ambe);
    {
        static int dmr_tc_log;
        if (bridge_dbg_periodic(&dmr_tc_log)) {
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
            if (bridge_dbg_periodic(&voc_fail))
                LOG_DMR_WARNING("DMR->EL vocoder decode failed\n");
            continue;
        }
        peer_el_write_pcm(&b->el, pcm, 160);
    }
}

/* Same connect-PTT flow as bridge.c (YSF<->DMR). */
static void bridge_el_connect_ptt_begin_stream(bridge_el_t *b, int tg, int clearing)
{
    b->connect_ptt_active = 1;
    b->connect_ptt_phase = 0;
    b->connect_ptt_voice_frames = 0;
    b->connect_ptt_tg = tg;
    b->connect_ptt_clearing = clearing ? 1 : 0;
    b->dmr_stream_id = bridge_new_stream_id();
    b->dmr_seq = 0;
    bridge_stamp_now(&b->connect_ptt_start);
    bridge_stamp_now(&b->last_dmr_tx);
    b->last_dmr_tx.tv_sec = 0; /* force first emit immediately */
    LOG_DMR_INFO("DMR connect PTT start (TG %d, %d ms)%s\n",
                 tg, CONNECT_PTT_MS,
                 clearing ? " [clear dynamic]" : "");
}

static void bridge_el_connect_ptt_finish(bridge_el_t *b)
{
    uint8_t slot_bit = b->dmr_slot_bit;
    int ended_tg = b->connect_ptt_tg > 0 ? b->connect_ptt_tg : b->dmr.tg;
    int was_clearing = b->connect_ptt_clearing;

    while ((b->connect_ptt_voice_frames % 6) != 0) {
        uint8_t n = (uint8_t)(b->connect_ptt_voice_frames % 6);
        bridge_el_send_dmrd(b, (uint8_t)(slot_bit | n), DMR_SILENCE_DATA);
        b->connect_ptt_voice_frames++;
    }
    bridge_el_send_dmrd(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                        DMR_SILENCE_DATA);
    LOG_DMR_INFO("DMR connect PTT end (TG %d, %d voice frames)\n",
                 ended_tg, b->connect_ptt_voice_frames);

    if (was_clearing && b->dmr.tg > 0) {
        /* Next: activate configured talkgroup. */
        bridge_el_connect_ptt_begin_stream(b, b->dmr.tg, 0);
        return;
    }

    b->connect_ptt_active = 0;
    b->connect_ptt_phase = 0;
    b->connect_ptt_voice_frames = 0;
    b->connect_ptt_tg = 0;
    b->connect_ptt_clearing = 0;
}

static void bridge_el_start_connect_ptt(bridge_el_t *b)
{
    if (b->call_active || b->connect_ptt_active)
        return;
    if (b->dmr.tg <= 0)
        return;

    if (b->clear_dynamic_tg)
        bridge_el_connect_ptt_begin_stream(b, DMR_CLEAR_DYNAMIC_TG, 1);
    else
        bridge_el_connect_ptt_begin_stream(b, b->dmr.tg, 0);
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
        bridge_stamp_now(&b->connect_ptt_start);
        return;
    }

    if (bridge_ms_since(&b->connect_ptt_start) >= CONNECT_PTT_MS) {
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

static void bridge_el_abort_el_to_dmr(bridge_el_t *b)
{
    /* Silent abort on DMR drop — do not emit VTERM into a dead/reconnecting session. */
    if (b->call_active != 1 && !b->dmr_ending)
        return;
    LOG_DMR_INFO("EL->DMR aborted — DMR peer down (was call_active=%d ending=%d)\n",
                 b->call_active, b->dmr_ending);
    b->call_active = 0;
    b->dmr_ending = 0;
    b->dmr_voice_frames = 0;
    b->el_ambe_count = 0;
    b->pcm_el_acc_n = 0;
    b->el_speech_run = 0;
    b->el_rf_id = 0;
    peer_el_drop_pcm_in(&b->el);
    peer_el_clear_remote_talker(&b->el);
    modeconv_reset();
    bridge_stamp_now(&b->last_el_tx_end);
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
        b->connect_ptt_tg = 0;
        b->connect_ptt_clearing = 0;
        bridge_el_abort_el_to_dmr(b);
    }
    b->dmr_was_connected = connected;

    if (b->connect_ptt_active && bridge_ms_elapsed(&b->last_dmr_tx, DMR_FRAME_MS))
        bridge_el_emit_connect_ptt(b);
}

/* ---- EchoLink <-> YSF (framing mirrors bridge.c DMR2YSF path) ---- */

static int bridge_el_send_ysfd(bridge_el_t *b, uint8_t fi, uint8_t ft, uint8_t cm,
                               uint8_t fich_fn, uint8_t net_cnt,
                               const uint8_t *payload120,
                               const uint8_t csd1[20], const uint8_t csd2[20])
{
    ysf_tx_args_t args = {
        .peer = &b->ysf,
        .repeater_callsign = b->ysf.callsign,
        .meta = BRIDGE_CALL_META(b),
        .last_tx = &b->last_ysf_tx,
        .ysf_fn = NULL,
        .dgid_cfg = b->ysf.dgid,
    };

    return ysf_tx_send(&args, fi, ft, cm, fich_fn, net_cnt, payload120, csd1, csd2);
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
        ysf_tx_fill_csd(BRIDGE_CALL_META(b), csd1, csd2);
        bridge_el_send_ysfd(b, YSF_FI_HEADER, YSF_FICH_FT, YSF_FICH_CM, 0, 0,
                            NULL, csd1, csd2);
        b->ysf_cnt = 1;
        return 1;
    }
    if (tag == MODECONV_TAG_EOT) {
        uint8_t csd1[20], csd2[20];

        ysf_tx_fill_csd(BRIDGE_CALL_META(b), csd1, csd2);
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
        peer_el_clear_remote_talker(&b->el);
        modeconv_reset();
        bridge_stamp_now(&b->last_el_tx_end);
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
     * Previous QSO talker is cleared on call end; late SDES → re-HEADER.
     */
    adapter_el_resolve_talker(b);
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

/* Late SDES user talker: radios lock HEADER — re-queue HEADER with new CSD. */
static void bridge_el_ysf_reheader_if_talker_changed(bridge_el_t *b)
{
    const char *raw;
    char want[10];
    char prev[10];

    if (b->call_active != 1 || b->ysf_ending)
        return;
    raw = peer_el_remote_talker(&b->el);
    if (!raw || !raw[0])
        return;
    bridge_el_format_callsign10(want, raw);
    if (memcmp(want, b->net_src, 10) == 0)
        return;
    memcpy(prev, b->net_src, 10);
    adapter_el_resolve_talker(b);
    if (memcmp(prev, b->net_src, 10) == 0)
        return;
    modeconv_put_dmr_header();
    LOG_YSF_INFO("EL->YSF re-HEADER talker %.10s -> %.10s\n", prev, b->net_src);
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
                if (bridge_dbg_periodic(&voc_fail))
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

    if (b->mode != ADN_BRIDGE_MODE_ECHOLINK_YSF)
        return;
    if (b->call_active == 2 || b->ysf_ending)
        return; /* YSF RX has the slot, or EOT drain in progress */

    /* SDES talker often arrives after first RTP — re-HEADER so radios update. */
    if (b->call_active == 1)
        bridge_el_ysf_reheader_if_talker_changed(b);

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
        bridge_stamp_now(&b->last_el_speech);

        in_cooldown = (b->last_el_tx_end.tv_sec || b->last_el_tx_end.tv_nsec)
                      && bridge_ms_since(&b->last_el_tx_end) < EL_TX_COOLDOWN_MS;
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

        pcm_apply_gain(b->pcm_el_acc, 160, b->el_pcm_gain);
        if (vocoder_encode(&b->voc, b->pcm_el_acc, ambe) != 0) {
            static int voc_enc_fail;

            b->pcm_el_acc_n = 0;
            enc_fail_streak++;
            if (bridge_dbg_periodic(&voc_enc_fail))
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
            if (bridge_dbg_periodic(&el_ysf_log)) {
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

    if (b->mode != ADN_BRIDGE_MODE_ECHOLINK_YSF)
        return;
    if (len != 155 || memcmp(pkt, "YSFD", 4) != 0)
        return;
    if (ysf_fich_decode_fields(pkt, &fi, &fn, &ft, &cm, &dt) != 0) {
        LOG_YSF_DEBUG("YSF RX FICH decode failed\n");
        return;
    }

    rx_dgid = ysf_fich_get_dgid();
    identity_dbg_label10(rpt, pkt + 4);
    identity_dbg_label10(src, pkt + 14);
    LOG_YSF_DEBUG("YSF RX %s fi=%u fn=%u ft=%u cm=%u dt=%u dgid=%u rpt=%s src=%s\n",
              ysf_tx_fi_name(fi), (unsigned)fi, (unsigned)fn, (unsigned)ft,
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
        bridge_stamp_now(&b->last_dmr_rx); /* reuse: last peer->EL activity */
        modeconv_reset();
        modeconv_put_ysf_header();
        adapter_el_set_ysf_talker_name(b);
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
        adapter_el_set_ysf_talker_name(b);
        LOG_YSF_INFO("YSF->EL call start (src %.10s, no HEADER)\n", b->net_src);
    }
    if (dt != YSF_DT_VD_MODE2) {
        LOG_YSF_DEBUG("YSF VOICE dt=%u (expect VD2; using repack+putYSF)\n",
                  (unsigned)dt);
    }
    bridge_stamp_now(&b->last_dmr_rx);
    modeconv_put_ysf_payload(ysf_tx_modeconv_chunk(pkt, scratch));
    b->ysf_voice_frames++;
    bridge_el_drain_ysf_to_el_pcm(b);
}

static void bridge_el_pace_dmr_tx(bridge_el_t *b)
{
    uint8_t voice33[33];

    if (b->mode != ADN_BRIDGE_MODE_ECHOLINK_DMR)
        return;
    if (b->call_active == 2 || b->connect_ptt_active || b->dmr_ending)
        return;
    /* Pace only after the call has started; first frame may start immediately. */
    if (b->call_active == 1 && !bridge_ms_elapsed(&b->last_dmr_tx, DMR_FRAME_MS))
        return;
    if (modeconv_get_dmr(voice33) == MODECONV_TAG_DATA)
        bridge_el_emit_dmr_voice(b, voice33);
}

void bridge_el_tick(bridge_el_t *b)
{
    if (b->mode == ADN_BRIDGE_MODE_ECHOLINK_DMR) {
        bridge_el_poll_connect_ptt(b);
        if (peer_dmr_connected(&b->dmr)) {
            if (b->dmr_ending)
                bridge_el_pace_dmr_end(b);
            else
                bridge_el_pace_dmr_tx(b);
        }
    }

    /* EL→YSF: one YSFD every 90 ms (identical pacing to DMR→YSF). */
    if (b->mode == ADN_BRIDGE_MODE_ECHOLINK_YSF && b->call_active == 1
        && bridge_ms_elapsed(&b->last_ysf_tx, YSF_FRAME_MS))
        (void)bridge_el_emit_ysf_from_conv(b);

    /* End EL->DMR/YSF when inbound EL PCM stops (silence still holds while RTP). */
    if (b->call_active == 1 && !b->dmr_ending && !b->ysf_ending
        && bridge_ms_since(&b->last_el_speech) >= EL_HANG_MS) {
        if (b->mode == ADN_BRIDGE_MODE_ECHOLINK_DMR)
            bridge_el_begin_dmr_end(b);
        else if (b->mode == ADN_BRIDGE_MODE_ECHOLINK_YSF)
            bridge_el_end_ysf_call(b);
    }

    /*
     * DMR/YSF->EL: if the stream dies without VTERM/EOT, release the
     * half-duplex lock so EL TX can run again.
     */
    if (b->call_active == 2) {
        if (b->mode == ADN_BRIDGE_MODE_ECHOLINK_DMR
            && bridge_ms_since(&b->last_dmr_rx) >= DMR_RX_HANG_MS) {
            peer_el_flush_pcm(&b->el);
            LOG_DMR_INFO("DMR->EL call end (%d voice frames in, el_rtp_tx=%u) — RX hangtime\n",
                         b->dmr_voice_frames, b->el.rtp_tx_packets);
            b->call_active = 0;
            b->dmr_voice_frames = 0;
            b->dmr_rx_stream_id = 0;
        } else if (b->mode == ADN_BRIDGE_MODE_ECHOLINK_YSF
                   && bridge_ms_since(&b->last_dmr_rx) >= YSF_RX_HANG_MS) {
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
