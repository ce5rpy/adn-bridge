/*
 * EchoLink <-> DMR voice bridge (PCM + remote AMBE vocoder).
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

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DMR_FRAME_MS   55
#define CONNECT_PTT_MS 1000
#define DMRD_FT_DATA_SYNC  2U
#define DMRD_FT_VOICE_SYNC 1U
#define DMRD_DTYPE_VHEAD   1U
#define DMRD_DTYPE_VTERM   2U

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

static void bridge_el_send_dmrd(bridge_el_t *b, uint8_t frame_type, const uint8_t *voice33)
{
    uint8_t pkt[55];
    uint8_t rf[3];
    int rf_id = b->dmr.dmrid;
    int src_id;

    if (rf_id <= 0) {
        LOG_WARNING("DMR TX skipped: bridge dmrid not configured\n");
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
            LOG_DEBUG("DMR TX %s b15=0x%02x rf=%d gw=%d tg=%d seq=%u stream=0x%08x\n",
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
        int i;
        b->call_active = 1;
        b->dmr_stream_id = new_stream_id();
        b->dmr_seq = 0;
        b->dmr_voice_frames = 0;
        modeconv_reset();
        for (i = 0; i < 3; i++)
            bridge_el_send_dmrd(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD),
                                NULL);
        b->el.rtp_rx_packets = 0;
        LOG_INFO("EL->DMR call start (TG %d)\n", b->dmr.tg);
    }

    b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4)) : (slot_bit | n));
    bridge_el_send_dmrd(b, b15, voice33);
    b->dmr_voice_frames++;
}

static void bridge_el_end_dmr_call(bridge_el_t *b)
{
    uint8_t slot_bit = b->dmr_slot_bit;

    if (b->call_active != 1)
        return;
    while ((b->dmr_voice_frames % 6) != 0) {
        uint8_t n = (uint8_t)(b->dmr_voice_frames % 6);
        bridge_el_send_dmrd(b, (uint8_t)(slot_bit | n), DMR_SILENCE_DATA);
        b->dmr_voice_frames++;
    }
    bridge_el_send_dmrd(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                        DMR_SILENCE_DATA);
    LOG_INFO("EL->DMR call end (%d DMR frames out, el_rtp_rx=%u)\n",
             b->dmr_voice_frames, b->el.rtp_rx_packets);
    b->call_active = 0;
    b->dmr_voice_frames = 0;
    b->el_ambe_count = 0;
    modeconv_reset();
}

void bridge_el_init(bridge_el_t *b, int mode, const char *dmr_options,
                    ysf2dmr_aliases_t *aliases)
{
    memset(b, 0, sizeof(*b));
    b->mode = mode;
    b->aliases = aliases;
    b->dmr_slot_bit = 0x80;
    (void)dmr_options;
    modeconv_init();
    stamp_now(&b->last_dmr_tx);
}

void bridge_el_process_el_audio(bridge_el_t *b)
{
    int16_t pcm[160];
    uint8_t ambe[7];
    uint8_t voice33[33];
    int n;
    int enc_fail_streak = 0;

    if (b->mode != YSF2DMR_MODE_ECHOLINK_DMR)
        return;
    if (b->call_active == 2)
        return; /* DMR has the slot */

    while ((n = peer_el_read_pcm(&b->el, pcm, 160)) > 0) {
        int i;
        for (i = 0; i < n && b->pcm_el_acc_n < 160; i++)
            b->pcm_el_acc[b->pcm_el_acc_n++] = pcm[i];
        if (b->pcm_el_acc_n < 160)
            continue;
        if (vocoder_encode(&b->voc, b->pcm_el_acc, ambe) != 0) {
            static int voc_enc_fail;
            b->pcm_el_acc_n = 0;
            enc_fail_streak++;
            if (dbg_periodic(&voc_enc_fail))
                LOG_WARNING("EL->DMR vocoder encode failed\n");
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
                LOG_DEBUG("EL->DMR ambe raw=%02x%02x%02x%02x%02x%02x%02x "
                          "%02x%02x%02x%02x%02x%02x%02x "
                          "%02x%02x%02x%02x%02x%02x%02x\n",
                          b->el_ambe_buf[0][0], b->el_ambe_buf[0][1], b->el_ambe_buf[0][2],
                          b->el_ambe_buf[0][3], b->el_ambe_buf[0][4], b->el_ambe_buf[0][5],
                          b->el_ambe_buf[0][6],
                          b->el_ambe_buf[1][0], b->el_ambe_buf[1][1], b->el_ambe_buf[1][2],
                          b->el_ambe_buf[1][3], b->el_ambe_buf[1][4], b->el_ambe_buf[1][5],
                          b->el_ambe_buf[1][6],
                          b->el_ambe_buf[2][0], b->el_ambe_buf[2][1], b->el_ambe_buf[2][2],
                          b->el_ambe_buf[2][3], b->el_ambe_buf[2][4], b->el_ambe_buf[2][5],
                          b->el_ambe_buf[2][6]);
            }
        }
        modeconv_put_ambe7(b->el_ambe_buf[0]);
        modeconv_put_ambe7(b->el_ambe_buf[1]);
        modeconv_put_ambe7(b->el_ambe_buf[2]);
        b->el_ambe_count = 0;
        if (modeconv_get_dmr(voice33) == MODECONV_TAG_DATA)
            bridge_el_emit_dmr_voice(b, voice33);
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
        LOG_DEBUG("DMR RX ignore len=%d (expected DMRD 55)\n", len);
        return;
    }

    rf = (pkt[5] << 16) | (pkt[6] << 8) | pkt[7];
    dst = (pkt[8] << 16) | (pkt[9] << 8) | pkt[10];
    dmrd_parse_b15(pkt[15], &ft, &dtype);
    if (dbg_periodic(&rx_log) || dmrd_is_header(pkt, len) || dmrd_is_terminator(pkt, len)) {
        LOG_DEBUG("DMR RX %s b15=0x%02x rf=%d dst=%d gw=%u seq=%u stream=0x%08x active=%d\n",
                  dmrd_class_label(pkt, len), pkt[15], rf, dst,
                  (unsigned)((pkt[11] << 24) | (pkt[12] << 16) | (pkt[13] << 8) | pkt[14]),
                  (unsigned)pkt[4],
                  (unsigned)*(const uint32_t *)(pkt + 16),
                  b->call_active);
    }

    if (dmrd_is_header(pkt, len)) {
        uint32_t sid = *(const uint32_t *)(pkt + 16);

        /* Duplicate VHEAD on the active stream — do not reset counters/RTP. */
        if (b->call_active == 2 && sid == b->dmr_rx_stream_id)
            return;
        if (b->call_active == 1)
            bridge_el_end_dmr_call(b);
        if (b->call_active == 2) {
            peer_el_flush_pcm(&b->el);
            LOG_INFO("DMR->EL call end (%d voice frames in, el_rtp_tx=%u) — replaced by new stream\n",
                     b->dmr_voice_frames, b->el.rtp_tx_packets);
        }
        b->call_active = 2;
        b->dmr_rx_stream_id = sid;
        b->dmr_voice_frames = 0;
        b->el.rtp_tx_packets = 0;
        LOG_INFO("DMR->EL call start (TG %d)\n", b->dmr.tg);
        return;
    }
    if (dmrd_is_terminator(pkt, len)) {
        if (b->call_active == 2) {
            peer_el_flush_pcm(&b->el);
            LOG_INFO("DMR->EL call end (%d voice frames in, el_rtp_tx=%u)\n",
                     b->dmr_voice_frames, b->el.rtp_tx_packets);
            b->call_active = 0;
            b->dmr_voice_frames = 0;
            b->dmr_rx_stream_id = 0;
        }
        return;
    }
    if (b->call_active != 2)
        return;

    b->dmr_voice_frames++;
    modeconv_dmr33_to_ambe(pkt + 20, ambe);
    {
        static int dmr_tc_log;
        if (dbg_periodic(&dmr_tc_log)) {
            LOG_DEBUG("DMR->EL ambe raw=%02x%02x%02x%02x%02x%02x%02x "
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
                LOG_WARNING("DMR->EL vocoder decode failed\n");
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
    LOG_INFO("DMR connect PTT end (TG %d, %d voice frames)\n",
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
    LOG_INFO("DMR connect PTT start (TG %d, %d ms)\n", b->dmr.tg, CONNECT_PTT_MS);
}

static void bridge_el_emit_connect_ptt(bridge_el_t *b)
{
    uint8_t slot_bit = b->dmr_slot_bit;
    int i;

    if (!b->connect_ptt_active)
        return;

    if (b->connect_ptt_phase == 0) {
        for (i = 0; i < 3; i++)
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

void bridge_el_tick(bridge_el_t *b)
{
    bridge_el_poll_connect_ptt(b);

    /*
     * End EL->DMR after RTP silence — must use last_rtp_rx, not pcm_in_count.
     * The ring is drained every loop, so pcm_in_count==0 does not mean idle.
     */
    if (b->call_active == 1) {
        time_t now = time(NULL);
        if (b->el.last_rtp_rx == 0)
            b->el.last_rtp_rx = now;
        else if (now - b->el.last_rtp_rx >= 2)
            bridge_el_end_dmr_call(b);
    }
}
