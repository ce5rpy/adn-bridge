/*
 * YSF wire adapter: parse only, hand off to media_core.
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "adapters/ysf.h"

#include "log.h"
#include "media/identity.h"
#include "media/peer_bus.h"
#include "session/ysf_tx.h"
#include "ysf_fich.h"

#include <string.h>

void adapter_ysf_on_wire(media_core_t *core, int src_router_id, peer_ysf_t *ysf,
                         const uint8_t *pkt, int len)
{
    media_bus_frame_t frame;
    uint8_t fi, fn, ft, cm, dt;
    uint8_t rx_dgid;

    if (len != 155 || memcmp(pkt, "YSFD", 4) != 0) {
        LOG_YSF_DEBUG("YSF RX ignore len=%d tag=%.4s\n", len, pkt);
        return;
    }
    if (ysf_fich_decode_fields(pkt, &fi, &fn, &ft, &cm, &dt) != 0) {
        LOG_YSF_DEBUG("YSF RX FICH decode failed\n");
        return;
    }
    (void)fn;
    rx_dgid = ysf_fich_get_dgid();

    /* DG-ID 0 is untagged/open traffic: accept it; skip only other rooms
     * (>=1). This is a wire rule (which DGID we listen to), not a session
     * decision, so it stays in the adapter. */
    if (ysf->dgid >= 1U && rx_dgid >= 1U && rx_dgid != ysf->dgid) {
        LOG_YSF_DEBUG("YSF RX skip DGID %u (want %u)\n", (unsigned)rx_dgid, (unsigned)ysf->dgid);
        return;
    }

    memset(&frame, 0, sizeof(frame));
    frame.codec = CODEC_YSF_AMBE;
    frame.wire_dtype = dt;
    /* Raw wire src, 10 bytes — fallback identity for EL<->YSF late-join (no
     * HEADER seen yet); HEADER below overrides with CSD-resolved identity. */
    memcpy(frame.meta.netcall.net_src, pkt + 14, 10);

    if (fi == YSF_FI_HEADER) {
        identity_ysf_ctx_t ctx = { core->aliases, 0, NULL };
        peer_dmr_t *dmr = media_peer_bus_primary_dmr(core->bus);
        int rf_id = 0;

        if (dmr) {
            ctx.bridge_dmrid = dmr->dmrid;
            ctx.bridge_callsign = dmr->callsign;
        }
        if (!identity_resolve_ysf_header(&frame.meta.netcall, &rf_id, &ctx, pkt)) {
            LOG_YSF_WARNING("YSF ignored HEADER: no talker DMR id\n");
            return;
        }
        frame.meta.talker_id = rf_id;
        frame.kind = MEDIA_FRAME_CALL_BEGIN;
        media_core_ingress(core, src_router_id, &frame);
        return;
    }

    if (fi == YSF_FI_TERMINATOR) {
        frame.kind = MEDIA_FRAME_CALL_END;
        media_core_ingress(core, src_router_id, &frame);
        return;
    }

    if (fi == YSF_FI_COMMUNICATIONS) {
        uint8_t scratch[120];

        frame.kind = MEDIA_FRAME_VOICE;
        memcpy(frame.payload.ysf_payload120, ysf_tx_modeconv_chunk(pkt, scratch), 120);
        media_core_ingress(core, src_router_id, &frame);
        return;
    }

    LOG_YSF_WARNING("YSF unhandled fi=%u ft=%u cm=%u dt=%u\n",
                    (unsigned)fi, (unsigned)ft, (unsigned)cm, (unsigned)dt);
}
