/*
 * Shared DMRD TX build/send (bridge.c + bridge_el.c).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "session/dmr_tx.h"

#include "hbp/dmr_codec.h"
#include "log.h"
#include "media/bridge_util.h"
#include "session/dmr_wire.h"

#include <string.h>

void dmr_tx_send(const dmr_tx_args_t *a, uint8_t frame_type, const uint8_t *voice33)
{
    uint8_t pkt[55];
    uint8_t rf[3];
    int rf_id;
    int src_id;

    if (!a || !a->peer || !a->seq || !a->last_tx || !a->emb_raw)
        return;

    rf_id = a->talker_rf_id;
    if (rf_id <= 0) {
        LOG_DMR_WARNING("DMR TX skipped: bridge dmrid not configured\n");
        return;
    }
    src_id = dmr_id_rf24(rf_id);
    dmr_id_to_bytes3(rf_id, rf);

    memcpy(pkt, "DMRD", 4);
    pkt[4] = (*a->seq)++;
    pkt[5] = rf[0];
    pkt[6] = rf[1];
    pkt[7] = rf[2];
    pkt[8] = (a->tx_tg >> 16) & 0xff;
    pkt[9] = (a->tx_tg >> 8) & 0xff;
    pkt[10] = (a->tx_tg >> 0) & 0xff;
    tx_tgid = a->tx_tg;
    pkt[11] = (a->bridge_dmrid >> 24) & 0xff;
    pkt[12] = (a->bridge_dmrid >> 16) & 0xff;
    pkt[13] = (a->bridge_dmrid >> 8) & 0xff;
    pkt[14] = (a->bridge_dmrid >> 0) & 0xff;
    pkt[15] = frame_type;
    *(uint32_t *)(pkt + 16) = a->stream_id;

    memcpy(buf, pkt, 55);
    rx_srcid = src_id;

    if (((frame_type >> 4) & 0x03U) == DMRD_FT_DATA_SYNC) {
        generate_header();
        memcpy(pkt + 20, buf + 20, 33);
    } else if (voice33) {
        int i;

        memcpy(pkt + 20, voice33, 33);
        if (((frame_type >> 4) & 0x03U) == DMRD_FT_VOICE_SYNC) {
            for (i = 0; i < 7; i++)
                pkt[20 + 13 + i] = (uint8_t)((pkt[20 + 13 + i] & ~DMR_SYNC_MASK[i])
                                             | DMR_MS_SOURCED_AUDIO_SYNC[i]);
            encode_embedded_data(a->emb_raw);
        } else {
            uint8_t lcss;

            memcpy(buf + 20, pkt + 20, 33);
            lcss = get_embedded_data(buf + 20, frame_type & 0x0f, a->emb_raw);
            get_emb_data(buf + 20, lcss);
            memcpy(pkt + 20, buf + 20, 33);
        }
    } else {
        memset(pkt + 20, 0, 33);
    }

    peer_dmr_send(a->peer, pkt, 55);
    bridge_stamp_now(a->last_tx);
    {
        static int tx_log;
        uint8_t ft, dtype;

        dmrd_parse_b15(frame_type, &ft, &dtype);
        if (bridge_dbg_periodic(&tx_log)
            || (ft == DMRD_FT_DATA_SYNC
                && (dtype == DMRD_DTYPE_VHEAD || dtype == DMRD_DTYPE_VTERM))) {
            LOG_DMR_DEBUG("DMR TX %s b15=0x%02x rf=%d gw=%d tg=%d seq=%u stream=0x%08x\n",
                          dmrd_class_label(pkt, 55), frame_type, src_id, a->bridge_dmrid,
                          a->tx_tg, (unsigned)pkt[4], (unsigned)a->stream_id);
        }
    }
}
