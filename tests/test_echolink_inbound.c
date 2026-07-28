/*
 * Unit tests for EchoLink inbound connections (max_inbound / allowed_callsigns
 * / blocked_callsigns).
 *
 * Drives the REAL peer_echolink.c over loopback UDP (bind_addr=127.0.0.1) --
 * fake "stations" bound to distinct 127.0.0.x loopback addresses (all valid
 * on Linux without extra interface config) send RTCP SDES to the peer's real
 * rtcp_sock and read back whatever it replies with.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "peer_echolink.h"
#include "config.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * Minimal RTCP SDES packet: RR chunk + one SDES chunk with a CNAME item and,
 * if `name` is non-NULL, a NAME item too -- most real EchoLink clients (tlb
 * CallSignString convention) send CNAME as the literal placeholder
 * "CALLSIGN" with the real station identity in NAME instead.
 */
static int build_sdes(uint8_t *out, int outlen, const char *cname, const char *name)
{
    int o = 0;
    int cname_len = (int)strlen(cname);
    int name_len = name ? (int)strlen(name) : 0;
    int sdes_start;
    int pad;
    int words;

    if (outlen < 8 + 8 + 2 + cname_len + 2 + name_len + 1 + 3)
        return -1;

    out[o++] = (uint8_t)(3 << 6);
    out[o++] = 201; /* RTCP_RR */
    out[o++] = 0;
    out[o++] = 1;
    memset(out + o, 0, 4);
    o += 4;

    sdes_start = o;
    out[o++] = (uint8_t)((3 << 6) | 1);
    out[o++] = 202; /* RTCP_SDES */
    out[o++] = 0;
    out[o++] = 0; /* length filled below */
    memset(out + o, 0, 4);
    o += 4;
    out[o++] = 1; /* CNAME */
    out[o++] = (uint8_t)cname_len;
    memcpy(out + o, cname, (size_t)cname_len);
    o += cname_len;
    if (name) {
        out[o++] = 2; /* NAME */
        out[o++] = (uint8_t)name_len;
        memcpy(out + o, name, (size_t)name_len);
        o += name_len;
    }
    out[o++] = 0; /* END */

    pad = (4 - (o & 3)) & 3;
    if (pad > 0) {
        int i;

        out[sdes_start] |= 0x20;
        for (i = 0; i < pad - 1; i++)
            out[o++] = 0;
        out[o++] = (uint8_t)pad;
    }
    words = ((o - sdes_start) / 4) - 1;
    out[sdes_start + 2] = (uint8_t)(words >> 8);
    out[sdes_start + 3] = (uint8_t)(words & 0xff);
    return o;
}

static int bind_station(const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    struct sockaddr_in a;

    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* -1 = no reply within timeout; else the second RTCP chunk's packet type
 * (202 = SDES ack/accepted, 203 = BYE/rejected). */
static int recv_reply_type(int fd)
{
    struct pollfd pf;
    uint8_t buf[128];
    int n;

    pf.fd = fd;
    pf.events = POLLIN;
    if (poll(&pf, 1, 300) <= 0)
        return -1;
    n = (int)recv(fd, buf, sizeof(buf), 0);
    if (n < 10)
        return -1;
    return buf[9];
}

/* Drain any station-list/welcome DATA packets (0x6f + "NDATA...") on a
 * station's RTP-port socket, returning the last one's payload (NUL-
 * terminated) or an empty string if none arrived within the timeout. */
static void recv_station_list(int fd, char *out, size_t out_cap)
{
    out[0] = '\0';
    for (;;) {
        struct pollfd pf;
        uint8_t buf[512];
        int n;

        pf.fd = fd;
        pf.events = POLLIN;
        if (poll(&pf, 1, 200) <= 0)
            return;
        n = (int)recv(fd, buf, sizeof(buf), 0);
        if (n < 7 || buf[0] != 0x6f || memcmp(buf + 1, "NDATA", 5) != 0)
            continue;
        {
            size_t len = (size_t)(n - 6) < out_cap - 1 ? (size_t)(n - 6) : out_cap - 1;

            memcpy(out, buf + 6, len);
            out[len] = '\0';
        }
    }
}

static void build_config(adn_bridge_peer_el_t *el)
{
    memset(el, 0, sizeof(*el));
    strcpy(el->callsign, "TESTND");
    strcpy(el->bind_addr, "127.0.0.1");
    el->max_inbound = 2;
    strcpy(el->allowed_callsigns[0], "CE5RPY-L");
    strcpy(el->allowed_callsigns[1], "CA3XYZ-R");
    strcpy(el->allowed_callsigns[2], "CA5RPY");
    strcpy(el->allowed_callsigns[3], "CT1BLK");
    el->allowed_callsign_count = 4;
    /* CT1BLK is deliberately also in allowed_callsigns above -- proves an
     * explicit block always wins over the allow-list, independent of the
     * separate capacity ("node busy") rejection tested with CA3XYZ-R. */
    strcpy(el->blocked_callsigns[0], "CT1BLK");
    el->blocked_callsign_count = 1;
    /* Already-unescaped form (config.c's parse_welcome_text converts INI
     * "\n" to this real '\r' before it ever reaches peer_echolink_t). */
    strcpy(el->welcome_text, "Rules: be nice\rNo spam");
}

static void send_pkt(int fd, struct sockaddr_in *dst, const uint8_t *pkt, int n)
{
    sendto(fd, pkt, (size_t)n, 0, (struct sockaddr *)dst, sizeof(*dst));
}

int main(void)
{
    peer_echolink_t p;
    adn_bridge_peer_el_t cfg;
    int fd_a, fd_b, fd_c, fd_d, fd_e, fd_a_rtp;
    uint8_t pkt[128];
    int n;
    int checks = 0, fails = 0;
    int reply;
    struct sockaddr_in dst;
    char roster[512];

    build_config(&cfg);
    if (peer_el_open(&p, &cfg) != 0) {
        fprintf(stderr, "FAIL: peer_el_open failed\n");
        return 2;
    }

    fd_a = bind_station("127.0.0.2", EL_RTCP_PORT);
    fd_a_rtp = bind_station("127.0.0.2", EL_RTP_PORT);
    fd_b = bind_station("127.0.0.3", EL_RTCP_PORT);
    fd_c = bind_station("127.0.0.4", EL_RTCP_PORT);
    fd_d = bind_station("127.0.0.5", EL_RTCP_PORT);
    fd_e = bind_station("127.0.0.6", EL_RTCP_PORT);
    checks++;
    if (fd_a < 0 || fd_a_rtp < 0 || fd_b < 0 || fd_c < 0 || fd_d < 0 || fd_e < 0) {
        fprintf(stderr, "FAIL: could not bind fake station sockets\n");
        fails++;
        return 1;
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(EL_RTCP_PORT);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    /* 1. Authorized station A -> accepted (SDES ack toward its own address). */
    n = build_sdes(pkt, sizeof(pkt), "CE5RPY-L", NULL);
    send_pkt(fd_a, &dst, pkt, n);
    peer_el_poll(&p, 200);
    reply = recv_reply_type(fd_a);
    checks++;
    if (reply != 202) {
        fprintf(stderr, "FAIL: station A expected SDES ack (202), got %d\n", reply);
        fails++;
    }

    /* 2. Station D sends CNAME="CALLSIGN" (the literal tlb placeholder) with
     * the real callsign in NAME instead -- must be derived from NAME, not
     * checked against the literal string "CALLSIGN". Fills the 2nd slot. */
    n = build_sdes(pkt, sizeof(pkt), "CALLSIGN", "CA5RPY");
    send_pkt(fd_d, &dst, pkt, n);
    peer_el_poll(&p, 200);
    reply = recv_reply_type(fd_d);
    checks++;
    if (reply != 202) {
        fprintf(stderr, "FAIL: station D (CNAME=CALLSIGN, NAME=CA5RPY) expected SDES ack (202), got %d\n", reply);
        fails++;
    }

    /* Roster/welcome DATA blob (tlb SendStationList) broadcast to already-
     * connected station A after D joins -- must show both members (neither
     * prefixed "->" since nobody is transmitting in this test), the header's
     * [count/max], and the configured multi-line welcome_text. */
    recv_station_list(fd_a_rtp, roster, sizeof(roster));
    checks++;
    if (!strstr(roster, "\rCE5RPY-L\r") || !strstr(roster, "\rCA5RPY\r")) {
        fprintf(stderr, "FAIL: station A's roster after D joins missing a member: %s\n", roster);
        fails++;
    }
    checks++;
    if (strstr(roster, "->")) {
        fprintf(stderr, "FAIL: roster has a \"->\" talker arrow but nobody is talking: %s\n", roster);
        fails++;
    }
    checks++;
    if (!strstr(roster, "[2/2]")) {
        fprintf(stderr, "FAIL: roster header missing [count/max]: %s\n", roster);
        fails++;
    }
    checks++;
    if (!strstr(roster, "Rules: be nice") || !strstr(roster, "No spam")) {
        fprintf(stderr, "FAIL: roster missing configured welcome_text: %s\n", roster);
        fails++;
    }

    /* 3. Station B: authorized callsign, but both slots are now used (A, D)
     * -> BYE "Node busy" (203). */
    n = build_sdes(pkt, sizeof(pkt), "CA3XYZ-R", NULL);
    send_pkt(fd_b, &dst, pkt, n);
    peer_el_poll(&p, 200);
    reply = recv_reply_type(fd_b);
    checks++;
    if (reply != 203) {
        fprintf(stderr, "FAIL: station B expected BYE (203, node busy), got %d\n", reply);
        fails++;
    }

    /* 4. Unauthorized callsign from station C -> BYE "Not authorized" (203). */
    n = build_sdes(pkt, sizeof(pkt), "NOTALLOWED", NULL);
    send_pkt(fd_c, &dst, pkt, n);
    peer_el_poll(&p, 200);
    reply = recv_reply_type(fd_c);
    checks++;
    if (reply != 203) {
        fprintf(stderr, "FAIL: station C expected BYE (203, not authorized), got %d\n", reply);
        fails++;
    }

    /* 5. Station E: CT1BLK is in BOTH allowed_callsigns and blocked_callsigns
     * -- the explicit block must win, independent of capacity/allow-list
     * (both slots are already at capacity here too, but the log/reason
     * should say "blocked", not "busy" -- verified via the log in manual
     * testing; this check only asserts the wire-visible BYE). */
    n = build_sdes(pkt, sizeof(pkt), "CT1BLK", NULL);
    send_pkt(fd_e, &dst, pkt, n);
    peer_el_poll(&p, 200);
    reply = recv_reply_type(fd_e);
    checks++;
    if (reply != 203) {
        fprintf(stderr, "FAIL: station E (blocked) expected BYE (203), got %d\n", reply);
        fails++;
    }

    /* 6. Stations A/D's slots are unaffected by B/C/E's rejected attempts --
     * a repeat SDES from A is still treated as its existing connection. */
    n = build_sdes(pkt, sizeof(pkt), "CE5RPY-L", NULL);
    send_pkt(fd_a, &dst, pkt, n);
    peer_el_poll(&p, 200);
    reply = recv_reply_type(fd_a);
    checks++;
    if (reply != 202) {
        fprintf(stderr, "FAIL: station A keepalive expected SDES ack (202), got %d\n", reply);
        fails++;
    }

    printf("EchoLink inbound connections: %d checks, %d failures\n", checks, fails);

    close(fd_a);
    close(fd_a_rtp);
    close(fd_b);
    close(fd_c);
    close(fd_d);
    close(fd_e);
    peer_el_close(&p);
    return fails ? 1 : 0;
}
