/*
 * Logging helpers for adn-bridge.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef ADN_BRIDGE_LOG_H
#define ADN_BRIDGE_LOG_H

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARNING = 2,
    LOG_LEVEL_ERROR = 3,
} log_level_t;

/* Per-stanza / subsystem channels (INI log_level= or [log] keys). */
typedef enum {
    LOG_CH_APP = 0,      /* main, aliases, bridge shell — [log] level= */
    LOG_CH_ECHOLINK = 1, /* [echolink] log_level= */
    LOG_CH_DMR = 2,      /* [dmr] log_level= */
    LOG_CH_YSF = 3,      /* [ysf] log_level= */
    LOG_CH_VOCODER = 4,  /* [peer.*] vocoder_log_level= */
    LOG_CH_COUNT
} log_channel_t;

/* Output sinks — console (stderr) and/or a log file, each with its own
 * timestamp choice. Prefix is always "LEVEL/channel: " (or "ts LEVEL/channel: "
 * when timed) regardless of sink, so grep habits (`journalctl | grep '/dmr:'`)
 * keep working either way. */
typedef struct {
    int  console;       /* stderr sink enabled */
    int  console_timed; /* stderr line includes the "YYYY-MM-DD HH:MM:SS,mmm " prefix */
    int  file;          /* file sink enabled */
    int  file_timed;    /* file line includes the timestamp prefix */
    char file_path[256];
} log_output_cfg_t;

/* systemd (JOURNAL_STREAM set) -> not timed, journal already stamps each line;
 * TTY -> timed; anything else (piped/redirected, no journal) -> not timed —
 * set `console-timed` explicitly in [log] handlers= if you need a timestamp
 * in that case (e.g. `./adn-bridge >> out.log 2>&1`). */
int log_auto_detect_console_timed(void);

/* Applies the sink configuration; opens file_path if file is enabled. Safe to
 * call again later (e.g. after config reload) — closes any previously open
 * file first. Before this is called, sinks default to console-timed (today's
 * behavior), so any log call before main() calls this — config load errors,
 * mainly — still reaches stderr with a timestamp. */
void log_init(const log_output_cfg_t *out);
/* SIGHUP-style logrotate support: close and reopen the file sink at its
 * configured path (without restarting the process). No-op if the file sink
 * isn't enabled. */
void log_reopen_files(void);

void log_set_level(log_level_t level); /* sets all channels (compat) */
void log_set_channel_level(log_channel_t ch, log_level_t level);
log_level_t log_get_level(void); /* APP channel */
log_level_t log_get_channel_level(log_channel_t ch);
const char *log_level_name(log_level_t level);
const char *log_channel_name(log_channel_t ch);
/* Parses DEBUG, INFO, WARNING, ERROR (case-insensitive). Returns INFO if unknown. */
log_level_t log_level_from_string(const char *s);
int log_level_enabled(log_level_t level); /* APP channel */
int log_channel_enabled(log_channel_t ch, log_level_t level);
void log_msg(log_level_t level, const char *fmt, ...);
void log_msg_ch(log_channel_t ch, log_level_t level, const char *fmt, ...);

#define LOG_CH_DEBUG(ch, ...)   do { if (log_channel_enabled((ch), LOG_LEVEL_DEBUG))   log_msg_ch((ch), LOG_LEVEL_DEBUG,   __VA_ARGS__); } while (0)
#define LOG_CH_INFO(ch, ...)    do { if (log_channel_enabled((ch), LOG_LEVEL_INFO))    log_msg_ch((ch), LOG_LEVEL_INFO,    __VA_ARGS__); } while (0)
#define LOG_CH_WARNING(ch, ...) do { if (log_channel_enabled((ch), LOG_LEVEL_WARNING)) log_msg_ch((ch), LOG_LEVEL_WARNING, __VA_ARGS__); } while (0)
#define LOG_CH_ERROR(ch, ...)   do { if (log_channel_enabled((ch), LOG_LEVEL_ERROR))   log_msg_ch((ch), LOG_LEVEL_ERROR,   __VA_ARGS__); } while (0)

/* Default channel = APP (aliases, main, uncategorized). */
#define LOG_DEBUG(...)   LOG_CH_DEBUG(LOG_CH_APP, __VA_ARGS__)
#define LOG_INFO(...)    LOG_CH_INFO(LOG_CH_APP, __VA_ARGS__)
#define LOG_WARNING(...) LOG_CH_WARNING(LOG_CH_APP, __VA_ARGS__)
#define LOG_ERROR(...)   LOG_CH_ERROR(LOG_CH_APP, __VA_ARGS__)

#define LOG_EL_DEBUG(...)   LOG_CH_DEBUG(LOG_CH_ECHOLINK, __VA_ARGS__)
#define LOG_EL_INFO(...)    LOG_CH_INFO(LOG_CH_ECHOLINK, __VA_ARGS__)
#define LOG_EL_WARNING(...) LOG_CH_WARNING(LOG_CH_ECHOLINK, __VA_ARGS__)
#define LOG_EL_ERROR(...)   LOG_CH_ERROR(LOG_CH_ECHOLINK, __VA_ARGS__)

#define LOG_DMR_DEBUG(...)   LOG_CH_DEBUG(LOG_CH_DMR, __VA_ARGS__)
#define LOG_DMR_INFO(...)    LOG_CH_INFO(LOG_CH_DMR, __VA_ARGS__)
#define LOG_DMR_WARNING(...) LOG_CH_WARNING(LOG_CH_DMR, __VA_ARGS__)
#define LOG_DMR_ERROR(...)   LOG_CH_ERROR(LOG_CH_DMR, __VA_ARGS__)

#define LOG_YSF_DEBUG(...)   LOG_CH_DEBUG(LOG_CH_YSF, __VA_ARGS__)
#define LOG_YSF_INFO(...)    LOG_CH_INFO(LOG_CH_YSF, __VA_ARGS__)
#define LOG_YSF_WARNING(...) LOG_CH_WARNING(LOG_CH_YSF, __VA_ARGS__)
#define LOG_YSF_ERROR(...)   LOG_CH_ERROR(LOG_CH_YSF, __VA_ARGS__)

#define LOG_VOC_DEBUG(...)   LOG_CH_DEBUG(LOG_CH_VOCODER, __VA_ARGS__)
#define LOG_VOC_INFO(...)    LOG_CH_INFO(LOG_CH_VOCODER, __VA_ARGS__)
#define LOG_VOC_WARNING(...) LOG_CH_WARNING(LOG_CH_VOCODER, __VA_ARGS__)
#define LOG_VOC_ERROR(...)   LOG_CH_ERROR(LOG_CH_VOCODER, __VA_ARGS__)

#endif
