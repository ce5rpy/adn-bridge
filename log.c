/*
 * Logging helpers for ysf2dmrcon.
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

#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

static log_level_t g_log_level[LOG_CH_COUNT] = {
    LOG_LEVEL_INFO, LOG_LEVEL_INFO, LOG_LEVEL_INFO, LOG_LEVEL_INFO, LOG_LEVEL_INFO,
};

void log_set_level(log_level_t level)
{
    int i;

    if (level < LOG_LEVEL_DEBUG)
        level = LOG_LEVEL_DEBUG;
    if (level > LOG_LEVEL_ERROR)
        level = LOG_LEVEL_ERROR;
    for (i = 0; i < LOG_CH_COUNT; i++)
        g_log_level[i] = level;
}

void log_set_channel_level(log_channel_t ch, log_level_t level)
{
    if ((int)ch < 0 || ch >= LOG_CH_COUNT)
        return;
    if (level < LOG_LEVEL_DEBUG)
        level = LOG_LEVEL_DEBUG;
    if (level > LOG_LEVEL_ERROR)
        level = LOG_LEVEL_ERROR;
    g_log_level[ch] = level;
}

log_level_t log_get_level(void)
{
    return g_log_level[LOG_CH_APP];
}

log_level_t log_get_channel_level(log_channel_t ch)
{
    if ((int)ch < 0 || ch >= LOG_CH_COUNT)
        return LOG_LEVEL_INFO;
    return g_log_level[ch];
}

const char *log_level_name(log_level_t level)
{
    switch (level) {
    case LOG_LEVEL_DEBUG:   return "DEBUG";
    case LOG_LEVEL_INFO:    return "INFO";
    case LOG_LEVEL_WARNING: return "WARNING";
    case LOG_LEVEL_ERROR:   return "ERROR";
    default:                return "?";
    }
}

const char *log_channel_name(log_channel_t ch)
{
    switch (ch) {
    case LOG_CH_APP:      return "app";
    case LOG_CH_ECHOLINK: return "el";
    case LOG_CH_DMR:      return "dmr";
    case LOG_CH_YSF:      return "ysf";
    case LOG_CH_VOCODER:  return "voc";
    default:              return "?";
    }
}

log_level_t log_level_from_string(const char *s)
{
    char buf[16];
    size_t i, n;

    if (!s || !s[0])
        return LOG_LEVEL_INFO;

    n = strlen(s);
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    for (i = 0; i < n; i++)
        buf[i] = (char)toupper((unsigned char)s[i]);
    buf[n] = '\0';

    if (strcmp(buf, "DEBUG") == 0)
        return LOG_LEVEL_DEBUG;
    if (strcmp(buf, "INFO") == 0)
        return LOG_LEVEL_INFO;
    if (strcmp(buf, "WARNING") == 0 || strcmp(buf, "WARN") == 0)
        return LOG_LEVEL_WARNING;
    if (strcmp(buf, "ERROR") == 0)
        return LOG_LEVEL_ERROR;
    return LOG_LEVEL_INFO;
}

int log_level_enabled(log_level_t level)
{
    return log_channel_enabled(LOG_CH_APP, level);
}

int log_channel_enabled(log_channel_t ch, log_level_t level)
{
    if ((int)ch < 0 || ch >= LOG_CH_COUNT)
        return 0;
    return level >= g_log_level[ch];
}

void log_msg(log_level_t level, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    /* Re-use channel path via vfprintf after header — call log_msg_ch style. */
    if (log_channel_enabled(LOG_CH_APP, level)) {
        struct timespec ts;
        struct tm tm;
        char tbuf[32];

        clock_gettime(CLOCK_REALTIME, &ts);
        localtime_r(&ts.tv_sec, &tm);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
        fprintf(stderr, "%s,%03ld %s/app: ", tbuf, ts.tv_nsec / 1000000L,
                log_level_name(level));
        vfprintf(stderr, fmt, ap);
    }
    va_end(ap);
}

void log_msg_ch(log_channel_t ch, log_level_t level, const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;
    struct tm tm;
    char tbuf[32];

    if (!log_channel_enabled(ch, level))
        return;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "%s,%03ld %s/%s: ", tbuf, ts.tv_nsec / 1000000L,
            log_level_name(level), log_channel_name(ch));
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
