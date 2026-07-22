/*
 * YSF <-> DMR adapter.
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "adapters/dmr.h"
#include "adapters/ysf.h"
#include "log.h"
#include "media/bridge_util.h"
#include "media/call_meta.h"
#include "media/identity.h"
#include "media/router.h"
#include "mmdvm/modeconv_wrap.h"
#include "peer_ysf.h"
#include "session/ysf_tx.h"
#include "ysf_fich.h"
#include <string.h>
#include <time.h>

#define ADAPTER_META(b) ((bridge_call_meta_t *)&(b)->net_src)
#define YSF_DT_VD_MODE2 0x02U

static void adapter_ysf_router_release_active(adn_bridge_t *b)
{
    int active;

    if (!b->router)
        return;
    active = media_router_active_ingress(b->router);
    if (active >= 0)
        media_router_ingress_end(b->router, active);
}

static int adapter_ysf_router_take(adn_bridge_t *b, int peer_id)
{
    int active;

    if (!b->router || peer_id < 0)
        return 1;
    active = media_router_active_ingress(b->router);
    if (active >= 0 && active != peer_id)
        media_router_ingress_end(b->router, active);
    if (!media_router_ingress_allowed(b->router, peer_id))
        return 0;
    media_router_ingress_begin(b->router, peer_id);
    return 1;
}

static const char *modeconv_tag_name(unsigned int tag)
{
    switch (tag) {
    case MODECONV_TAG_HEADER: return "HEADER";
    case MODECONV_TAG_DATA:   return "DATA";
    case MODECONV_TAG_EOT:    return "EOT";
    default:                  return "NODATA";
    }
}

static int adapter_ysf_talker_ready(const adn_bridge_t *b)
{
    return b->ysf_rf_id > 0 || b->dmr.dmrid > 0;
}

static int adapter_ysf_resolve_header(adn_bridge_t *b, const uint8_t *pkt)
{
    identity_ysf_ctx_t ctx = {
        b->aliases,
        b->dmr.dmrid,
        b->dmr.callsign,
    };

    return identity_resolve_ysf_header(ADAPTER_META(b), &b->ysf_rf_id, &ctx, pkt);
}

void adapter_ysf_dmr_reset_call(adn_bridge_t *b)
{
    adapter_ysf_router_release_active(b);
    b->call_active = 0;
    b->dmr_voice_frames = 0;
    b->dmr_tx_frames = 0;
    b->dmr_dmrd_other = 0;
    b->ysf_voice_frames = 0;
    b->ysf_rf_id = 0;
    b->ysf_cnt = 0;
    b->dmr_last_dtype = 0;
    memset(&b->dmra, 0, sizeof(b->dmra));
    memset(b->net_src, ' ', 10);
    memset(b->net_dst, ' ', 10);
    modeconv_reset();
}

static int adapter_ysf_send(adn_bridge_t *b, uint8_t fi, uint8_t ft, uint8_t cm,
                             uint8_t fich_fn, uint8_t net_cnt, const uint8_t *payload120,
                             const uint8_t csd1[20], const uint8_t csd2[20])
{
    ysf_tx_args_t args = {
        .peer = &b->ysf,
        .repeater_callsign = b->ysf.callsign,
        .meta = ADAPTER_META(b),
        .last_tx = &b->last_ysf_tx,
        .ysf_fn = &b->ysf_fn,
        .dgid_cfg = b->ysf.dgid,
    };

    return ysf_tx_send(&args, fi, ft, cm, fich_fn, net_cnt, payload120, csd1, csd2);
}

int adapter_ysf_emit_from_conv_ysf(adn_bridge_t *b)
{
    uint8_t payload[120];
    unsigned int tag;
    static int drain_log;

    memset(payload, 0, sizeof(payload));
    tag = modeconv_get_ysf(payload);
    if (tag == MODECONV_TAG_NODATA)
        return 0;

    if (bridge_dbg_periodic(&drain_log))
        LOG_YSF_DEBUG("ModeConv->YSF %s (dmr_in=%d ysf_cnt=%d)\n",
            modeconv_tag_name(tag), b->dmr_voice_frames, b->ysf_cnt);

    if (tag == MODECONV_TAG_HEADER) {
        uint8_t csd1[20], csd2[20];

        b->ysf_cnt = 0;
        ysf_tx_fill_csd(ADAPTER_META(b), csd1, csd2);
        adapter_ysf_send(b, YSF_FI_HEADER, YSF_FICH_FT, YSF_FICH_CM, 0, 0, NULL, csd1, csd2);
        b->ysf_cnt = 1;
        return 1;
    }
    if (tag == MODECONV_TAG_EOT) {
        uint8_t csd1[20], csd2[20];

        ysf_tx_fill_csd(ADAPTER_META(b), csd1, csd2);
        adapter_ysf_send(b, YSF_FI_TERMINATOR, YSF_FICH_FT, YSF_FICH_CM, 0,
                         b->ysf_cnt, NULL, csd1, csd2);
        if (b->dmr_dmrd_other)
            LOG_DMR_INFO("DMR->YSF call end (%d voice frames in, %d other DMRD)\n",
                     b->dmr_voice_frames, b->dmr_dmrd_other);
        else
            LOG_DMR_INFO("DMR->YSF call end (%d voice frames in)\n", b->dmr_voice_frames);
        adapter_ysf_dmr_reset_call(b);
        return 1;
    }
    if (tag == MODECONV_TAG_DATA) {
        uint8_t fn = (uint8_t)((b->ysf_cnt - 1U) % (YSF_FICH_FT + 1U));
        uint8_t net = (uint8_t)((b->ysf_cnt & 0x7FU) << 1);

        adapter_ysf_send(b, YSF_FI_COMMUNICATIONS, YSF_FICH_FT, YSF_FICH_CM,
                         fn, net, payload, NULL, NULL);
        b->ysf_cnt++;
        return 1;
    }
    return 0;
}

void adapter_ysf_on_ysfd_ysf(adn_bridge_t *b, const uint8_t *pkt, int len)
{
    uint8_t fi, fn, ft, cm, dt;
    char rpt[11], src[11];
    uint8_t rx_dgid;

    if (len != 155 || memcmp(pkt, "YSFD", 4) != 0) {
        LOG_YSF_DEBUG("YSF RX ignore len=%d tag=%.4s\n", len, pkt);
        return;
    }

    if (ysf_fich_decode_fields(pkt, &fi, &fn, &ft, &cm, &dt) != 0) {
        LOG_YSF_DEBUG("YSF RX FICH decode failed\n");
        return;
    }

    rx_dgid = ysf_fich_get_dgid();
    identity_dbg_label10(rpt, pkt + 4);
    identity_dbg_label10(src, pkt + 14);
    LOG_YSF_DEBUG("YSF RX %s fi=%u(%s) fn=%u ft=%u cm=%u dt=%u dgid=%u rpt=%s src=%s fn48=%u\n",
        ysf_tx_fi_name(fi), (unsigned)fi, ysf_tx_fi_name(fi), (unsigned)fn, (unsigned)ft,
        (unsigned)cm, (unsigned)dt, (unsigned)rx_dgid, rpt, src, (unsigned)pkt[48]);

    /* DG-ID 0 is untagged/open traffic: the reflector only relays what our
     * activated room should hear, so accept it; skip only other rooms (>=1). */
    if (b->ysf.dgid >= 1U && rx_dgid >= 1U && rx_dgid != b->ysf.dgid) {
        LOG_YSF_DEBUG("YSF RX skip DGID %u (want %u)\n",
            (unsigned)rx_dgid, (unsigned)b->ysf.dgid);
        return;
    }

    if (fi == YSF_FI_HEADER) {
        if (!adapter_ysf_resolve_header(b, pkt) || !adapter_ysf_talker_ready(b)) {
            LOG_YSF_WARNING("YSF ignored HEADER: no talker DMR id\n");
            return;
        }
        LOG_YSF_DEBUG("YSF process HEADER -> ModeConv (talker id %d)\n", b->ysf_rf_id);
        if (!b->call_active) {
            adapter_dmr_abort_connect_ptt_ysf(b);
            if (!adapter_ysf_router_take(b, b->router_peer_ysf))
                return;
            b->call_active = 1;
            b->dmr_stream_id = bridge_new_stream_id();
            b->dmr_seq = 0;
            b->dmr_tx_frames = 0;
            b->dmr_voice_frames = 0;
            b->ysf_fn = pkt[48];
            modeconv_reset();
            LOG_DMR_INFO("YSF->DMR call start (TG %d, talker %.10s id %d)\n",
                     b->dmr.tg, b->net_src, b->ysf_rf_id);
        }
        modeconv_put_ysf_header();
        return;
    }

    if (fi == YSF_FI_TERMINATOR) {
        LOG_YSF_DEBUG("YSF process EOT -> ModeConv (voice_in=%d)\n", b->ysf_voice_frames);
        if (b->call_active) {
            modeconv_put_ysf_eot();
            LOG_DMR_INFO("YSF->DMR terminator (%d voice frames in)\n", b->ysf_voice_frames);
        }
        return;
    }

    if (fi == YSF_FI_COMMUNICATIONS) {
        if (!adapter_ysf_talker_ready(b)) {
            LOG_YSF_WARNING("YSF ignored VOICE: no talker id (missing HEADER?)\n");
            return;
        }
        if (dt != YSF_DT_VD_MODE2) {
            LOG_YSF_DEBUG("YSF VOICE dt=%u (HP3ICC; YSF2DMR expects dt=2, using repack+putYSF)\n",
                      (unsigned)dt);
        }
        LOG_YSF_DEBUG("YSF process VOICE fn=%u -> ModeConv (talker id %d voice_in=%d)\n",
            (unsigned)fn, b->ysf_rf_id, b->ysf_voice_frames);
        if (!b->call_active) {
            adapter_dmr_abort_connect_ptt_ysf(b);
            if (!adapter_ysf_router_take(b, b->router_peer_ysf))
                return;
            b->call_active = 1;
            b->dmr_stream_id = bridge_new_stream_id();
            b->dmr_seq = 0;
            b->dmr_tx_frames = 0;
            b->dmr_voice_frames = 0;
            b->ysf_voice_frames = 0;
            b->ysf_fn = pkt[48];
            modeconv_reset();
            LOG_DMR_INFO("YSF->DMR call start (TG %d, talker %.10s id %d)\n",
                     b->dmr.tg, b->net_src, b->ysf_rf_id);
        }
        {
            uint8_t scratch[120];

            modeconv_put_ysf_payload(ysf_tx_modeconv_chunk(pkt, scratch));
        }
        b->ysf_voice_frames++;
        return;
    }

    LOG_YSF_WARNING("YSF unhandled fi=%u ft=%u cm=%u dt=%u\n",
                (unsigned)fi, (unsigned)ft, (unsigned)cm, (unsigned)dt);
}

