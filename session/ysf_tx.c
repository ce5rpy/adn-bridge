/*
 * Shared YSFD TX build/send (bridge.c + bridge_el.c).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "session/ysf_tx.h"

#include "log.h"
#include "media/bridge_util.h"
#include "mmdvm/ysfpayload_wrap.h"
#include "ysf_fich.h"

#include <string.h>

static const uint8_t YSF_SYNC_BYTES[5] = {0xD4U, 0x71U, 0xC9U, 0x63U, 0x4DU};
static const uint8_t YSF_DCH_DT1[10] = {1U, 34U, 97U, 95U, 43U, 3U, 17U, 0U, 0U, 0U};
static const uint8_t YSF_DCH_DT2[10] = {0U, 0U, 0U, 0U, 108U, 32U, 28U, 32U, 3U, 8U};

static const char *ysf_tx_fi_name(uint8_t fi)
{
    switch (fi) {
    case YSF_FI_HEADER:         return "HDR";
    case YSF_FI_COMMUNICATIONS: return "VOICE";
    case YSF_FI_TERMINATOR:     return "EOT";
    default:                    return "?";
    }
}

static void radio_id_to_dch5(uint8_t out[5])
{
    memset(out, '*', 5);
}

static int ysf_pkt_has_rf_sync(const uint8_t *pkt155)
{
    return memcmp(pkt155 + YSF_FICH_OFFSET_NET, YSF_SYNC_BYTES, 5) == 0;
}

static void ysf_net_to_rf120(const uint8_t *pkt155, uint8_t rf120[120])
{
    const uint8_t *net = pkt155 + YSF_FICH_OFFSET_NET;

    memset(rf120, 0, 120);
    memcpy(rf120, YSF_SYNC_BYTES, 5);
    memcpy(rf120 + 5, net, 115);
}

const uint8_t *ysf_tx_modeconv_chunk(const uint8_t *pkt155, uint8_t scratch[120])
{
    if (ysf_pkt_has_rf_sync(pkt155))
        return pkt155 + YSF_FICH_OFFSET_NET;
    ysf_net_to_rf120(pkt155, scratch);
    return scratch;
}

void ysf_tx_fill_csd(const bridge_call_meta_t *meta, uint8_t csd1[20], uint8_t csd2[20])
{
    uint8_t rid[5];

    if (!meta || !csd1 || !csd2)
        return;

    memset(csd2, ' ', 20);
    memset(csd1, '*', 5);
    radio_id_to_dch5(rid);
    memcpy(csd1 + 5, rid, 5);
    memcpy(csd1 + 10, meta->net_src, 10);
}

static void ysf_tx_apply_dch_slot(uint8_t *payload, uint8_t fn, const bridge_call_meta_t *meta)
{
    uint8_t dch[10];
    uint8_t rid[5];

    if (!payload || !meta)
        return;

    memset(dch, ' ', 10);
    radio_id_to_dch5(rid);
    switch (fn) {
    case 0:
        memset(dch, '*', 5);
        memcpy(dch + 5, rid, 5);
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    case 1:
        memcpy(dch, meta->net_src, 10);
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    case 2:
        memcpy(dch, meta->net_dst, 10);
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

static void ysf_tx_fill_headers(uint8_t *frame, const ysf_tx_args_t *args, uint8_t net_cnt)
{
    if (!frame || !args || !args->peer || !args->meta || !args->repeater_callsign)
        return;

    memcpy(frame, "YSFD", 4);
    memcpy(frame + 4, args->repeater_callsign, 10);
    memcpy(frame + 14, args->meta->net_src, 10);
    memcpy(frame + 24, YSF_WIRE_DST_ALL, 10);
    frame[34] = net_cnt;
}

int ysf_tx_send(ysf_tx_args_t *args, uint8_t fi, uint8_t ft, uint8_t cm,
                uint8_t fich_fn, uint8_t net_cnt, const uint8_t *payload120,
                const uint8_t csd1[20], const uint8_t csd2[20])
{
    uint8_t frame[155];

    if (!args || !args->peer || !args->meta || !args->last_tx)
        return 0;

    ysf_tx_fill_headers(frame, args, net_cnt);

    if (fi == YSF_FI_HEADER || fi == YSF_FI_TERMINATOR) {
        memset(frame + YSF_FICH_OFFSET_NET, 0, 120);
        memcpy(frame + YSF_FICH_OFFSET_NET, YSF_SYNC_BYTES, 5);
        ysf_fich_encode_outbound(frame + YSF_FICH_OFFSET_RX, fich_fn, fi, ft, cm);
        if (csd1 && csd2)
            ysf_payload_write_header(frame + YSF_FICH_OFFSET_NET, csd1, csd2);
    } else {
        if (payload120)
            memcpy(frame + YSF_FICH_OFFSET_NET, payload120, 120);
        memcpy(frame + YSF_FICH_OFFSET_NET, YSF_SYNC_BYTES, 5);
        if (!(args->relay_passthrough && payload120)) {
            ysf_tx_apply_dch_slot(frame + YSF_FICH_OFFSET_NET, fich_fn, args->meta);
            ysf_fich_encode_outbound(frame + YSF_FICH_OFFSET_RX, fich_fn, fi, ft, cm);
        }
    }

    peer_ysf_send_ysfd(args->peer, frame, 155);
    bridge_stamp_now(args->last_tx);

    {
        static int tx_log;
        uint8_t wire_fi, wire_fn, wire_ft, wire_cm, wire_dt;
        int log_tx = (fi == YSF_FI_HEADER || fi == YSF_FI_TERMINATOR
                      || (fi == YSF_FI_COMMUNICATIONS && (fich_fn <= 1U))
                      || bridge_dbg_periodic(&tx_log));

        if (log_tx
            && ysf_fich_decode_fields(frame, &wire_fi, &wire_fn, &wire_ft, &wire_cm, &wire_dt) == 0) {
            LOG_YSF_DEBUG("YSF TX %s fi=%u ft=%u fn=%u cm=%u dt=%u net=%3u src=%.10s dst=%.10s dgid_cfg=%u\n",
                          ysf_tx_fi_name(fi), (unsigned)wire_fi, (unsigned)ft, (unsigned)wire_fn,
                          (unsigned)wire_cm, (unsigned)wire_dt,
                          (unsigned)net_cnt, frame + 14, frame + 24, args->dgid_cfg);
        }
    }
    return 1;
}
