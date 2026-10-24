/*
 * Unit tests for the HBP client login: drives peer_dmr through RPTL/RPTK against
 * a fake master on localhost and checks the RPTC it sends, field by field.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "peer_dmr.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int checks, fails;

static void check_field(const char *what, const uint8_t *got, const char *want)
{
    size_t n = strlen(want);

    checks++;
    if (memcmp(got, want, n) != 0) {
        fprintf(stderr, "FAIL: %s = '%.*s', want '%s'\n", what, (int)n, (const char *)got, want);
        fails++;
    }
}

/* Returns the RPTC length (0 on failure); the packet is left in rptc. */
static int login_until_rptc(const char *callsign, const char *freq, uint8_t rptc[512])
{
    int master = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {0}, client;
    socklen_t alen = sizeof(addr), clen = sizeof(client);
    struct timeval tv = {1, 0};
    peer_dmr_t p;
    int from_peer, n = 0;

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (master < 0 || bind(master, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        getsockname(master, (struct sockaddr *)&addr, &alen) < 0)
        return 0;
    setsockopt(master, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (peer_dmr_open(&p, "127.0.0.1", ntohs(addr.sin_port), callsign, 7300001, 91,
                      NULL, "passw0rd", NULL, NULL, freq) == 0) {
        peer_dmr_tick(&p);
        if (recvfrom(master, rptc, 512, 0, (struct sockaddr *)&client, &clen) == 8) {
            sendto(master, "RPTACK\x01\x02\x03\x04", 10, 0, (struct sockaddr *)&client, clen);
            peer_dmr_poll(&p, 1000, &from_peer);
            if (recvfrom(master, rptc, 512, 0, NULL, NULL) == 40) {
                sendto(master, "RPTACK", 6, 0, (struct sockaddr *)&client, clen);
                peer_dmr_poll(&p, 1000, &from_peer);
                n = (int)recvfrom(master, rptc, 512, 0, NULL, NULL);
            }
        }
    }
    peer_dmr_close(&p);
    close(master);
    if (n != 302 || memcmp(rptc, "RPTC", 4) != 0) {
        fprintf(stderr, "FAIL: no 302-byte RPTC (got %d)\n", n);
        fails++;
        return 0;
    }
    return n;
}

int main(void)
{
    uint8_t rptc[512];

    if (login_until_rptc("ce5abc-1", "445000", rptc)) {
        check_field("callsign", rptc + 8, "CE5ABC  ");
        check_field("rx freq", rptc + 16, "000445000");
        check_field("tx freq", rptc + 25, "000445000");
        check_field("latitude", rptc + 38, "0.000000");
        check_field("longitude", rptc + 46, "00.000000");
        check_field("height", rptc + 55, "000");
        check_field("slots", rptc + 97, "4");
        checks++;
        for (int i = 0; i < 8; i++)
            if (rptc[222 + i] < '0' || rptc[222 + i] > '9') {
                fprintf(stderr, "FAIL: software_id '%.20s' does not start with YYYYMMDD\n", (const char *)rptc + 222);
                fails++;
                break;
            }
        check_field("software_id suffix", rptc + 230, "_adn-bridge ");
        check_field("package_id", rptc + 262, "MMDVM ");
    }
    if (login_until_rptc("CE5ABC", NULL, rptc))
        check_field("freq when unset", rptc + 16, "000000000");
    if (login_until_rptc("CE5ABC", "1296000000", rptc))
        check_field("freq over 9 digits", rptc + 16, "000000000");

    printf("HBP client RPTC: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
