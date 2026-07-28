/*
 * DMR wire adapter: parse only, hand off to media_core.
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "adapters/dmr.h"

#include "log.h"
#include "media/bridge_util.h"
#include "session/dmr_tx.h"
#include "session/dmr_wire.h"
#include "talker_alias.h"

#include <string.h>

void adapter_dmr_on_wire(media_core_t *core, int src_router_id, peer_dmr_t *dmr,
                        const uint8_t *pkt, int len)
{
    media_bus_frame_t frame;
    int rf, dst;

    (void)dmr; /* identity resolution moved to core — only needed on call-begin */

    if (len == DMRA_PACKET_LEN && memcmp(pkt, "DMRA", 4) == 0) {
        int block_id;
        uint8_t payload7[7];

        if (!dmra_parse_packet(pkt, len, &rf, &block_id, payload7))
            return;
        memset(&frame, 0, sizeof(frame));
        frame.kind = MEDIA_FRAME_SIDECHAIN;
        frame.codec = CODEC_DMR_AMBE;
        frame.payload.dmra.rf = rf;
        frame.payload.dmra.block_id = block_id;
        memcpy(frame.payload.dmra.block7, payload7, 7);
        media_core_ingress(core, src_router_id, &frame);
        return;
    }

    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0) {
        const char *cmd = hbp_cmd_label(pkt, len);
        char hex[140];
        uint32_t tail_id = 0;

        hbp_wire_hex(pkt, len, hex, sizeof(hex));
        if (hbp_wire_tail_id(pkt, len, cmd, &tail_id)) {
            LOG_DMR_DEBUG("DMR RX %s len=%d id=%u hex=%s (ignored)\n",
                          cmd, len, tail_id, hex);
        } else {
            LOG_DMR_DEBUG("DMR RX %s len=%d hex=%s (ignored)\n", cmd, len, hex);
        }
        return;
    }

    rf = (pkt[5] << 16) | (pkt[6] << 8) | pkt[7];
    dst = (pkt[8] << 16) | (pkt[9] << 8) | pkt[10];

    memset(&frame, 0, sizeof(frame));
    frame.codec = CODEC_DMR_AMBE;
    frame.meta.stream_id = *(const uint32_t *)(pkt + 16);
    frame.meta.talker_id = rf; /* raw rf; core resolves identity only on call-begin */
    frame.wire_seq = pkt[4];
    frame.wire_dst = dst;

    if (dmrd_is_header(pkt, len)) {
        frame.kind = MEDIA_FRAME_CALL_BEGIN;
        frame.wire_dtype = DMRD_DTYPE_VHEAD;
    } else if (dmrd_is_terminator(pkt, len)) {
        frame.kind = MEDIA_FRAME_CALL_END;
        frame.wire_dtype = DMRD_DTYPE_VTERM;
    } else if (dmrd_is_voice(pkt, len)) {
        uint8_t ft, dtype;

        dmrd_parse_b15(pkt[15], &ft, &dtype);
        frame.kind = MEDIA_FRAME_VOICE;
        frame.wire_dtype = dtype;
        frame.wire_dmr_ft = ft;
        memcpy(frame.payload.dmr_voice33, pkt + 20, 33);
    } else {
        static int other_log;
        if (bridge_dbg_periodic(&other_log)) {
            uint8_t ft, dtype;
            dmrd_parse_b15(pkt[15], &ft, &dtype);
            LOG_DMR_WARNING("DMR RX unclassified b15=0x%02x (ft=%u dtype=%u)\n",
                            pkt[15], (unsigned)ft, (unsigned)dtype);
        }
        return;
    }

    media_core_ingress(core, src_router_id, &frame);
}

void adapter_dmr_egress_dmrd(const dmr_tx_args_t *args, uint8_t frame_type,
                             const uint8_t *voice33)
{
    dmr_tx_send(args, frame_type, voice33);
}
