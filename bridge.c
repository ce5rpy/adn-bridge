/*
 * YSF <-> DMR voice bridge shim (Phase 2 → adapters).
 */
#include "bridge.h"
#include "adapters/dmr.h"
#include "adapters/ysf.h"
#include "codecs/ysf_ambe.h"
#include "log.h"
#include "media/bridge_util.h"
#include "peer_dmr.h"
#include "peer_ysf.h"

#include <string.h>
#include <time.h>

#define YSF_FRAME_MS 90
#define DMR_FRAME_MS 55

void bridge_bind_router(adn_bridge_t *b, media_router_t *router,
                        int dmr_id, int ysf_id)
{
    b->router = router;
    b->router_peer_dmr = dmr_id;
    b->router_peer_ysf = ysf_id;
}

void bridge_init(adn_bridge_t *b, const char *dmr_options,
                 adn_bridge_aliases_t *aliases, int default_ysf_dmrid,
                 int clear_dynamic_tg)
{
    memset(b, 0, sizeof(*b));
    b->aliases = aliases;
    b->default_ysf_dmrid = default_ysf_dmrid;
    b->clear_dynamic_tg = clear_dynamic_tg ? 1 : 0;
    b->dmr_slot_bit = adapter_dmr_slot_bit_from_options(dmr_options);
    memset(b->net_src, ' ', 10);
    memset(b->net_dst, ' ', 10);
    memcpy(b->net_dst, "ALL       ", 10);
    codec_ysf_ambe_init();
    bridge_stamp_now(&b->last_dmr_tx);
    bridge_stamp_now(&b->last_ysf_tx);
}

void bridge_on_dmrd(adn_bridge_t *b, const uint8_t *pkt, int len)
{
    adapter_dmr_on_dmrd_ysf(b, pkt, len);
}

void bridge_on_dmra(adn_bridge_t *b, const uint8_t *pkt, int len)
{
    adapter_dmr_on_dmra_ysf(b, pkt, len);
}

void bridge_on_ysfd(adn_bridge_t *b, const uint8_t *pkt, int len)
{
    adapter_ysf_on_ysfd_ysf(b, pkt, len);
}

void bridge_abort_connect_ptt(adn_bridge_t *b)
{
    adapter_dmr_abort_connect_ptt_ysf(b);
}

void bridge_tick(adn_bridge_t *b)
{
    static time_t last_stall;

    adapter_dmr_poll_connect_ptt_ysf(b);

    if (!peer_dmr_connected(&b->dmr) || !peer_ysf_linked(&b->ysf)) {
        time_t now = time(NULL);
        if (log_channel_enabled(LOG_CH_DMR, LOG_LEVEL_DEBUG) && (now - last_stall >= 15 || last_stall == 0)) {
            LOG_YSF_DEBUG("tick idle: dmr=%s ysf=%s call_active=%d ptt=%d\n",
                peer_dmr_connected(&b->dmr) ? "up" : "down",
                peer_ysf_linked(&b->ysf) ? "up" : "down",
                b->call_active, b->connect_ptt_active);
            last_stall = now;
        }
        return;
    }

    last_stall = 0;

    if (!b->connect_ptt_active && bridge_ms_elapsed(&b->last_dmr_tx, DMR_FRAME_MS))
        adapter_dmr_emit_from_conv_ysf(b);

    if (bridge_ms_elapsed(&b->last_ysf_tx, YSF_FRAME_MS))
        (void)adapter_ysf_emit_from_conv_ysf(b);
}
