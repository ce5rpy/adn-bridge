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

static log_level_t g_log_level = LOG_LEVEL_INFO;

void log_set_level(log_level_t level)
{
    if (level < LOG_LEVEL_DEBUG)
        level = LOG_LEVEL_DEBUG;
    if (level > LOG_LEVEL_ERROR)
        level = LOG_LEVEL_ERROR;
    g_log_level = level;
}

log_level_t log_get_level(void)
{
    return g_log_level;
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
    return level >= g_log_level;
}

void log_msg(log_level_t level, const char *fmt, ...)
{
    va_list ap;

    if (level < g_log_level)
        return;
    fprintf(stderr, "%s: ", log_level_name(level));
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
