/*
 * Local ALSA sound-card peer -- PTT via software VOX (RMS) or a hardware
 * COR/PTT switch on a GPIO pin (libgpiod, WITH_GPIOD build).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "peer_alsa.h"

#include "log.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#ifdef WITH_ALSA
#include <alsa/asoundlib.h>
#endif

#ifdef WITH_GPIOD
#include <gpiod.h>
#endif

#if defined(WITH_ALSA) || defined(WITH_GPIOD)
static uint32_t alsa_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}
#endif

/* Same RMS/gain math as media/core_echolink.c's pcm_rms16/pcm_apply_gain --
 * kept as its own tiny copy rather than a shared helper (adding-new-modes.md
 * ~1: don't couple a new peer's leaf-level utility to another mode's module). */
static int pcm_rms16(const int16_t *pcm, int n)
{
    long long acc = 0;
    int i;

    if (n <= 0)
        return 0;
    for (i = 0; i < n; i++)
        acc += (long)pcm[i] * (long)pcm[i];
    return (int)sqrt((double)acc / (double)n);
}

#ifdef WITH_ALSA
static void pcm_apply_gain(int16_t *pcm, int n, float gain)
{
    int i;

    if (!pcm || n <= 0 || gain == 1.0f)
        return;
    for (i = 0; i < n; i++) {
        float v = (float)pcm[i] * gain;

        if (v > 32767.0f)
            v = 32767.0f;
        else if (v < -32768.0f)
            v = -32768.0f;
        pcm[i] = (int16_t)v;
    }
}
#endif

static const char *peer_alsa_vox_state_name(peer_alsa_vox_state_t s)
{
    switch (s) {
    case ALSA_VOX_IDLE:     return "IDLE";
    case ALSA_VOX_ARMED:    return "ARMED";
    case ALSA_VOX_TX:       return "TX";
    case ALSA_VOX_COOLDOWN: return "COOLDOWN";
    default:                return "?";
    }
}

int peer_alsa_vox_update(peer_alsa_vox_t *vox, const int16_t *pcm, int samples,
                          uint32_t now_ms, const adn_bridge_peer_alsa_t *cfg)
{
    int rms;
    int above;
    peer_alsa_vox_state_t prev;
    int ret;

    if (!vox || !cfg)
        return 0;
    rms = pcm_rms16(pcm, samples);
    vox->last_rms = rms;
    above = rms >= cfg->vox_threshold;
    prev = vox->state;

    /* Heartbeat every ~1s so [peer.*] log_level=DEBUG shows live rms vs
     * threshold even while nothing is arming -- the only way to tell "mic
     * captures silence" from "threshold too high" from "no capture at all"
     * without real hardware in front of you. */
    if ((int32_t)(now_ms - vox->last_heartbeat_ms) >= 1000 || vox->last_heartbeat_ms == 0) {
        vox->last_heartbeat_ms = now_ms;
        LOG_ALSA_DEBUG("alsa: vox rms=%d threshold=%d state=%s\n",
                       rms, cfg->vox_threshold, peer_alsa_vox_state_name(vox->state));
    }

    switch (vox->state) {
    case ALSA_VOX_IDLE:
        if (above) {
            vox->armed_since_ms = now_ms;
            vox->state = ALSA_VOX_ARMED;
        }
        ret = 0;
        break;
    case ALSA_VOX_ARMED:
        if (!above) {
            vox->state = ALSA_VOX_IDLE;
            ret = 0;
            break;
        }
        if ((uint32_t)(now_ms - vox->armed_since_ms) < (uint32_t)cfg->vox_attack_ms) {
            ret = 0;
            break;
        }
        vox->state = ALSA_VOX_TX;
        vox->last_active_ms = now_ms;
        ret = 1;
        break;
    case ALSA_VOX_TX:
        if (above)
            vox->last_active_ms = now_ms;
        if ((uint32_t)(now_ms - vox->last_active_ms) < (uint32_t)cfg->vox_hang_ms) {
            ret = 1;
            break;
        }
        vox->state = ALSA_VOX_COOLDOWN;
        vox->cooldown_until_ms = now_ms + (uint32_t)cfg->tx_cooldown_ms;
        ret = 0;
        break;
    case ALSA_VOX_COOLDOWN:
        if ((int32_t)(now_ms - vox->cooldown_until_ms) >= 0)
            vox->state = ALSA_VOX_IDLE;
        ret = 0;
        break;
    default:
        vox->state = ALSA_VOX_IDLE;
        ret = 0;
        break;
    }

    if (vox->state != prev)
        LOG_ALSA_INFO("alsa: vox %s -> %s (rms=%d, threshold=%d)\n",
                      peer_alsa_vox_state_name(prev), peer_alsa_vox_state_name(vox->state),
                      rms, cfg->vox_threshold);
    return ret;
}

int peer_alsa_cor_update(peer_alsa_cor_t *cor, int raw_active, uint32_t now_ms,
                          const adn_bridge_peer_alsa_t *cfg)
{
    int prev;

    if (!cor || !cfg)
        return 0;
    raw_active = raw_active ? 1 : 0;

    if (raw_active != cor->raw_active) {
        cor->raw_active = raw_active;
        cor->change_ms = now_ms;
    }

    prev = cor->debounced_active;
    if (raw_active != cor->debounced_active
        && (uint32_t)(now_ms - cor->change_ms) >= (uint32_t)cfg->cor_debounce_ms)
        cor->debounced_active = raw_active;

    if (cor->debounced_active != prev)
        LOG_ALSA_INFO("alsa: cor %s -> %s\n", prev ? "active" : "inactive",
                      cor->debounced_active ? "active" : "inactive");
    return cor->debounced_active;
}

/* Request the configured GPIO line as an input for COR/PTT sensing. Returns
 * 0 on success (p->gpio_chip/gpio_request set), -1 on failure. No-op success
 * when ptt_type != "gpio" (nothing to open). */
static int peer_alsa_gpio_open(peer_alsa_t *p, const adn_bridge_peer_alsa_t *cfg)
{
    if (strcmp(cfg->ptt_type, "gpio") != 0)
        return 0;

#ifdef WITH_GPIOD
    {
        struct gpiod_chip *chip;
        struct gpiod_line_settings *settings;
        struct gpiod_line_config *line_cfg;
        struct gpiod_request_config *req_cfg;
        struct gpiod_line_request *request;
        unsigned int offset = (unsigned int)cfg->cor_gpio;

        chip = gpiod_chip_open(cfg->gpio_chip);
        if (!chip) {
            LOG_ALSA_ERROR("alsa: gpiod: cannot open chip '%s'\n", cfg->gpio_chip);
            return -1;
        }

        settings = gpiod_line_settings_new();
        if (!settings) {
            LOG_ALSA_ERROR("alsa: gpiod: line_settings_new failed\n");
            gpiod_chip_close(chip);
            return -1;
        }
        gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
        gpiod_line_settings_set_active_low(settings, cfg->cor_active_low ? true : false);

        line_cfg = gpiod_line_config_new();
        if (!line_cfg) {
            LOG_ALSA_ERROR("alsa: gpiod: line_config_new failed\n");
            gpiod_line_settings_free(settings);
            gpiod_chip_close(chip);
            return -1;
        }
        if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) != 0) {
            LOG_ALSA_ERROR("alsa: gpiod: add_line_settings failed for offset %u\n", offset);
            gpiod_line_config_free(line_cfg);
            gpiod_line_settings_free(settings);
            gpiod_chip_close(chip);
            return -1;
        }

        req_cfg = gpiod_request_config_new();
        if (req_cfg)
            gpiod_request_config_set_consumer(req_cfg, "adn-bridge");

        request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);

        if (req_cfg)
            gpiod_request_config_free(req_cfg);
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);

        if (!request) {
            LOG_ALSA_ERROR("alsa: gpiod: request_lines failed (chip='%s' offset=%u)\n",
                           cfg->gpio_chip, offset);
            gpiod_chip_close(chip);
            return -1;
        }

        p->gpio_chip = chip;
        p->gpio_request = request;
    }
    LOG_ALSA_INFO("alsa: ptt_type=gpio -- watching %s offset %d (active-%s, debounce %dms)\n",
                 cfg->gpio_chip, cfg->cor_gpio, cfg->cor_active_low ? "low" : "high",
                 cfg->cor_debounce_ms);
    return 0;
#else
    (void)p;
    LOG_ALSA_ERROR("alsa: ptt_type=gpio needs a build with WITH_GPIOD=1 (libgpiod)\n");
    return -1;
#endif
}

static void peer_alsa_gpio_close(peer_alsa_t *p)
{
#ifdef WITH_GPIOD
    if (p->gpio_request) {
        gpiod_line_request_release((struct gpiod_line_request *)p->gpio_request);
        p->gpio_request = NULL;
    }
    if (p->gpio_chip) {
        gpiod_chip_close((struct gpiod_chip *)p->gpio_chip);
        p->gpio_chip = NULL;
    }
#else
    (void)p;
#endif
}

/* Raw (pre-debounce) COR pin read: 1 = active, 0 = inactive/error. Polarity
 * (active-high/low) is already handled by gpiod_line_settings_set_active_low
 * at request time, so GPIOD_LINE_VALUE_ACTIVE always means "active" here
 * regardless of cor_active_low. */
static int peer_alsa_gpio_read(peer_alsa_t *p)
{
#ifdef WITH_GPIOD
    if (!p->gpio_request)
        return 0;
    return gpiod_line_request_get_value((struct gpiod_line_request *)p->gpio_request,
                                        (unsigned int)p->cfg.cor_gpio) == GPIOD_LINE_VALUE_ACTIVE;
#else
    (void)p;
    return 0;
#endif
}

int peer_alsa_open(peer_alsa_t *p, const adn_bridge_peer_alsa_t *cfg)
{
    if (!p || !cfg)
        return -1;
    memset(p, 0, sizeof(*p));
    p->cfg = *cfg;
    p->vox.state = ALSA_VOX_IDLE;

#ifdef WITH_ALSA
    {
        snd_pcm_t *capture = NULL, *playback = NULL;
        int err;

        /* Capture opens in BLOCKING mode on purpose -- see peer_alsa_poll's
         * use of snd_pcm_wait(). Some ALSA backends (notably the "pulse"
         * compat plugin used under WSLg/PipeWire setups, where there's no
         * real hardware card at all) are known to behave unreliably with a
         * raw SND_PCM_NONBLOCK capture read: it can "succeed" and return a
         * full period of stale/zeroed samples instead of a proper EAGAIN
         * when no real audio is ready yet. Blocking + snd_pcm_wait is the
         * more portable pattern across hw/dmix/pulse plugins. */
        err = snd_pcm_open(&capture, cfg->capture_device, SND_PCM_STREAM_CAPTURE, 0);
        if (err < 0) {
            LOG_ALSA_ERROR("alsa: capture open '%s' failed: %s\n",
                           cfg->capture_device, snd_strerror(err));
            return -1;
        }
        err = snd_pcm_set_params(capture, SND_PCM_FORMAT_S16_LE,
                                 SND_PCM_ACCESS_RW_INTERLEAVED, 1, ALSA_PCM_RATE, 1, 100000);
        if (err < 0) {
            LOG_ALSA_ERROR("alsa: capture params '%s' failed: %s\n",
                           cfg->capture_device, snd_strerror(err));
            snd_pcm_close(capture);
            return -1;
        }

        err = snd_pcm_open(&playback, cfg->playback_device, SND_PCM_STREAM_PLAYBACK,
                           SND_PCM_NONBLOCK);
        if (err < 0) {
            LOG_ALSA_ERROR("alsa: playback open '%s' failed: %s\n",
                           cfg->playback_device, snd_strerror(err));
            snd_pcm_close(capture);
            return -1;
        }
        err = snd_pcm_set_params(playback, SND_PCM_FORMAT_S16_LE,
                                 SND_PCM_ACCESS_RW_INTERLEAVED, 1, ALSA_PCM_RATE, 1, 100000);
        if (err < 0) {
            LOG_ALSA_ERROR("alsa: playback params '%s' failed: %s\n",
                           cfg->playback_device, snd_strerror(err));
            snd_pcm_close(capture);
            snd_pcm_close(playback);
            return -1;
        }

        p->capture_pcm = capture;
        p->playback_pcm = playback;

        if (peer_alsa_gpio_open(p, cfg) != 0) {
            snd_pcm_close(capture);
            snd_pcm_close(playback);
            p->capture_pcm = NULL;
            p->playback_pcm = NULL;
            return -1;
        }
    }
    p->open = 1;
    LOG_ALSA_INFO("alsa: opened capture='%s' playback='%s'\n",
                 cfg->capture_device, cfg->playback_device);
    return 0;
#else
    LOG_ALSA_ERROR("alsa: built without WITH_ALSA, peer_alsa_open() unavailable\n");
    return -1;
#endif
}

void peer_alsa_close(peer_alsa_t *p)
{
    if (!p || !p->open)
        return;
#ifdef WITH_ALSA
    if (p->capture_pcm)
        snd_pcm_close((snd_pcm_t *)p->capture_pcm);
    if (p->playback_pcm)
        snd_pcm_close((snd_pcm_t *)p->playback_pcm);
#endif
    peer_alsa_gpio_close(p);
    p->capture_pcm = NULL;
    p->playback_pcm = NULL;
    p->open = 0;
}

void peer_alsa_tick(peer_alsa_t *p)
{
    (void)p;
}

int peer_alsa_poll(peer_alsa_t *p, int timeout_ms)
{
    if (!p || !p->open)
        return 0;

#ifdef WITH_ALSA
    {
        int16_t buf[ALSA_PCM_SAMPLES];
        snd_pcm_sframes_t n;
        uint32_t now_ms;
        int active;
        int ready;

        if (!p->capture_pcm)
            return 0;
        /* snd_pcm_wait polls the PCM's own descriptors internally, which is
         * the portable way to ask "is a period ready?" across ALSA backends
         * (hw, dmix, the "pulse" compat plugin) -- see the comment on the
         * blocking snd_pcm_open() in peer_alsa_open() for why we don't just
         * do a raw non-blocking readi here. */
        ready = snd_pcm_wait((snd_pcm_t *)p->capture_pcm, timeout_ms > 0 ? timeout_ms : 0);
        if (ready <= 0)
            return 0; /* not ready yet, or a (harmless) wait timeout */

        n = snd_pcm_readi((snd_pcm_t *)p->capture_pcm, buf, ALSA_PCM_SAMPLES);
        if (n == -EAGAIN)
            return 0;
        if (n == -EPIPE) {
            snd_pcm_prepare((snd_pcm_t *)p->capture_pcm);
            return 0;
        }
        if (n < 0) {
            LOG_ALSA_WARNING("alsa: capture read error: %s\n", snd_strerror((int)n));
            return 0;
        }
        if (n != ALSA_PCM_SAMPLES)
            return 0; /* partial period; wait for the next tick rather than a short frame */

        now_ms = alsa_now_ms();
        if (strcmp(p->cfg.ptt_type, "gpio") == 0)
            active = peer_alsa_cor_update(&p->cor, peer_alsa_gpio_read(p), now_ms, &p->cfg);
        else
            active = peer_alsa_vox_update(&p->vox, buf, ALSA_PCM_SAMPLES, now_ms, &p->cfg);
        if (!active) {
            p->pcm_in_count = 0;
            return 0;
        }

        pcm_apply_gain(buf, ALSA_PCM_SAMPLES, p->cfg.gain);
        memcpy(p->pcm_in, buf, sizeof(buf));
        p->pcm_in_count = ALSA_PCM_SAMPLES;
        return p->pcm_in_count;
    }
#else
    return 0;
#endif
}

int peer_alsa_read_pcm(peer_alsa_t *p, int16_t *pcm, int max_samples)
{
    int n;

    if (!p || !pcm || max_samples <= 0 || p->pcm_in_count <= 0)
        return 0;
    n = p->pcm_in_count < max_samples ? p->pcm_in_count : max_samples;
    memcpy(pcm, p->pcm_in, (size_t)n * sizeof(int16_t));
    p->pcm_in_count = 0;
    return n;
}

int peer_alsa_write_pcm(peer_alsa_t *p, const int16_t *pcm, int samples)
{
    if (!p || !p->open || !pcm || samples <= 0)
        return 0;

#ifdef WITH_ALSA
    {
        snd_pcm_sframes_t n;

        if (!p->playback_pcm)
            return 0;
        n = snd_pcm_writei((snd_pcm_t *)p->playback_pcm, pcm, (snd_pcm_uframes_t)samples);
        if (n == -EAGAIN)
            return 0;
        if (n == -EPIPE) {
            snd_pcm_prepare((snd_pcm_t *)p->playback_pcm);
            n = snd_pcm_writei((snd_pcm_t *)p->playback_pcm, pcm, (snd_pcm_uframes_t)samples);
        }
        if (n < 0) {
            LOG_ALSA_WARNING("alsa: playback write error: %s\n", snd_strerror((int)n));
            return 0;
        }
        return (int)n;
    }
#else
    return 0;
#endif
}

void peer_alsa_flush_pcm(peer_alsa_t *p)
{
    (void)p;
}

void peer_alsa_drop_pcm_in(peer_alsa_t *p)
{
    if (!p)
        return;
    p->pcm_in_count = 0;
    p->vox.state = ALSA_VOX_IDLE;
}

void peer_alsa_on_sigint(peer_alsa_t *p)
{
    (void)p;
}
