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

typedef struct {
    int rtp_sock;
    int rtcp_sock;
    struct sockaddr_in bind_rtp;
    struct sockaddr_in bind_rtcp;
    struct sockaddr_in peer_rtp;
    struct sockaddr_in peer_rtcp;
    char callsign[16];
    char password[64];
    char bind_addr[64];
    char host[128]; /* node/conference name to connect (not an IP) */
    char qth[32];
    char email[64];
    char directory_servers[YSF2DMR_EL_DIR_MAX][128];
    int directory_server_count;
    int status; /* PEER_EL_* */
    int linked; /* RTCP SDES seen from peer */
    int peer_resolved; /* peer_rtp filled from directory lookup */
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
} peer_echolink_t;

int peer_el_open(peer_echolink_t *p, const ysf2dmr_echolink_cfg_t *cfg);
void peer_el_close(peer_echolink_t *p);
void peer_el_tick(peer_echolink_t *p);
int peer_el_linked(const peer_echolink_t *p);
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
void peer_el_on_sigint(peer_echolink_t *p);

#endif
