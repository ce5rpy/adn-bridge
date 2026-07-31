/*
 * EchoLink / iLink peer (directory login + RTP/RTCP + GSM PCM).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef PEER_ECHOLINK_H
#define PEER_ECHOLINK_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <netinet/in.h>

#include "config.h"
#include "el_proxy.h"

/* Background directory worker jobs (peer_el_tick schedules; never blocks audio). */
#define EL_DIR_JOB_NONE       0
#define EL_DIR_JOB_LOGIN      1
#define EL_DIR_JOB_LIST       2
#define EL_DIR_JOB_LOGIN_LIST 3

#define EL_RTP_PORT  5198
#define EL_RTCP_PORT 5199
#define EL_DIR_PORT  5200

#define EL_PCM_RATE       8000
#define EL_GSM_FRAME      33
#define EL_GSM_SAMPLES    160
#define EL_RTP_SAMPLES    (4 * EL_GSM_SAMPLES) /* 640 samples / packet */
#define EL_RTP_FRAME_LEN  (12 + 4 * EL_GSM_FRAME)

#define PEER_EL_DISCONNECTED 0
#define PEER_EL_DIR_OK       1
#define PEER_EL_CONNECTED    2

/* Inbound EchoLink connections accepted besides the configured outbound
 * host= (e.g. app users calling in). Hard cap; the configured max_inbound
 * (adn_bridge_peer_el_t) is clamped to this at open time. */
#define EL_MAX_INBOUND 4

typedef struct {
    int used;
    struct sockaddr_in addr;  /* source IP; RTP/RTCP both from this address, fixed ports 5198/5199 */
    char cname[32];
    time_t last_rtcp;         /* last SDES received from this station */
    time_t last_rtp;          /* last RTP received from this station (talker arbitration) */
} el_inbound_t;

typedef struct {
    int rtp_sock;
    int rtcp_sock;
    struct sockaddr_in bind_rtp;
    struct sockaddr_in bind_rtcp;
    struct sockaddr_in peer_rtp;
    struct sockaddr_in peer_rtcp;
    int use_proxy; /* 1 = EchoLink Proxy; no local UDP 5198/5199 */
    el_proxy_t proxy;
    char callsign[16];
    char password[64];
    char bind_addr[64];
    char host[128]; /* node/conference name to connect (not an IP) */
    char qth[32];
    char email[64];
    /* SDES NAME while bridging a remote talker (YSF/DMR→EL); empty = use callsign */
    char talker_name[32];
    /* Which bridge leg (DMR/YSF/...) is currently relaying audio INTO
     * EchoLink, for the roster's "->" indicator. Independent of talker_name
     * above (that's the outbound peer's SDES NAME) -- this only affects the
     * "connected users" list shown to inbound stations. */
    char relay_label[32];
    /* Inbound SDES: peer station (CNAME) and active talker (NAME or CNAME/host). */
    char remote_cname[32];
    char remote_talker[32];
    /* 1 = talker came from NAME parentheses (real user); keep across Conference status. */
    int remote_talker_explicit;
    char directory_servers[ADN_BRIDGE_EL_DIR_MAX][128];
    int directory_server_count;
    int status; /* PEER_EL_* */
    int linked; /* RTCP SDES seen from peer */
    int peer_resolved; /* peer_rtp filled from directory lookup */
    /* Inbound connections (besides the configured outbound peer above). */
    el_inbound_t inbound[EL_MAX_INBOUND];
    int max_inbound;                                   /* copied from config; 0 = disabled */
    char allowed_callsigns[ADN_BRIDGE_EL_ALLOW_MAX][16];
    int allowed_callsign_count;
    /* Checked first; always wins even if also in allowed_callsigns. */
    char blocked_callsigns[ADN_BRIDGE_EL_ALLOW_MAX][16];
    int blocked_callsign_count;
    char welcome_text[512]; /* appended to the roster blob; '\r'-separated lines */
    /* Talker arbitration across all sources (outbound peer + inbound[]):
     * -1 = none, 0 = outbound peer, i+1 = inbound[i]. Prevents two
     * simultaneously-talking EchoLink sources from garbling pcm_in. */
    int talk_src;
    time_t talk_last;
    int login_interval;         /* tlb LoginInterval */
    int station_list_interval;  /* tlb StationListInterval */
    uint16_t rtp_seq;
    uint32_t rtp_ts;
    time_t last_sdes;              /* last SDES we transmitted */
    time_t last_peer_rtcp;         /* last RTCP/SDES received from peer */
    time_t last_dir_login;
    time_t next_login_time;        /* tlb NextLoginTime */
    time_t next_station_list_time; /* tlb NextStationListTime; 0 = disabled */
    time_t last_rtp_rx;            /* last inbound RTP (for hangup, not drained ring) */
    unsigned rtp_tx_packets;
    unsigned rtp_rx_packets;
    /* PCM ring: inbound from EL (decoded GSM) */
    int16_t pcm_in[EL_PCM_RATE * 2];
    int pcm_in_r;
    int pcm_in_w;
    int pcm_in_count;
    /* Outbound PCM waiting to encode/send */
    int16_t pcm_out[EL_RTP_SAMPLES];
    int pcm_out_count;
    uint8_t rx_buf[2048];
    void *gsm_enc;
    void *gsm_dec;
    /* Directory TCP (login / station list) runs off the audio path. */
    pthread_t dir_tid;
    pthread_mutex_t dir_mu;
    pthread_cond_t dir_cv;
    int dir_thread_on;
    int dir_stop;
    int dir_busy;
    int dir_job; /* EL_DIR_JOB_* */
    /* fd of the directory worker's in-flight direct TCP connection (dir_mu-
     * guarded), -1 when none. Lets el_dir_thread_stop() shutdown() it so a
     * blocked connect()/read() returns immediately instead of making Ctrl-C
     * wait out the full directory timeout. */
    int dir_fd;
    /*
     * Proxy demux holds proxy.mu while calling RTCP/RTP handlers. Defer SDES
     * TX until after poll unlocks — otherwise el_send_sdes deadlocks on mu.
     */
    int sdes_reply_pending;
} peer_echolink_t;

int peer_el_open(peer_echolink_t *p, const adn_bridge_peer_el_t *cfg);
void peer_el_close(peer_echolink_t *p);
void peer_el_tick(peer_echolink_t *p);
/* Poll RTP/RTCP; returns samples available in pcm_in (>0), or 0. */
int peer_el_poll(peer_echolink_t *p, int timeout_ms);
/* Read up to max_samples of inbound PCM (8 kHz s16 LE). Returns samples read. */
int peer_el_read_pcm(peer_echolink_t *p, int16_t *pcm, int max_samples);
/* Queue PCM for TX to peer (may send when 640 samples accumulated). */
int peer_el_write_pcm(peer_echolink_t *p, const int16_t *pcm, int samples);
/* Pad/send any partial outbound RTP packet (call on DMR VTERM). */
void peer_el_flush_pcm(peer_echolink_t *p);
/* Drop inbound decoded PCM (call end — avoid residual silence restarting TX). */
void peer_el_drop_pcm_in(peer_echolink_t *p);
/* Set/clear SDES NAME for remote talker display (NULL/"" clears). Sends SDES. */
void peer_el_set_talker_name(peer_echolink_t *p, const char *name);
/* Set/clear which bridge leg is currently relaying into EchoLink (NULL/""
 * clears); broadcasts the "connected users" roster to inbound stations if
 * this changes the previous value. */
void peer_el_set_relay_label(peer_echolink_t *p, const char *label);
/* Best remote identity for EL→DMR/YSF (talker, else CNAME, else host). */
const char *peer_el_remote_talker(const peer_echolink_t *p);
/* After EL→DMR/YSF hangtime: drop sticky user talker so the next QSO starts clean. */
void peer_el_clear_remote_talker(peer_echolink_t *p);
void peer_el_on_sigint(peer_echolink_t *p);

#endif
