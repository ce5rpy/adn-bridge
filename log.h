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

#ifndef YSF2DMR_LOG_H
#define YSF2DMR_LOG_H

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARNING = 2,
    LOG_LEVEL_ERROR = 3,
} log_level_t;

void log_set_level(log_level_t level);
log_level_t log_get_level(void);
const char *log_level_name(log_level_t level);
/* Parses DEBUG, INFO, WARNING, ERROR (case-insensitive). Returns INFO if unknown. */
log_level_t log_level_from_string(const char *s);
int log_level_enabled(log_level_t level);
void log_msg(log_level_t level, const char *fmt, ...);

#define LOG_DEBUG(...)   do { if (log_level_enabled(LOG_LEVEL_DEBUG))   log_msg(LOG_LEVEL_DEBUG,   __VA_ARGS__); } while (0)
#define LOG_INFO(...)    do { if (log_level_enabled(LOG_LEVEL_INFO))    log_msg(LOG_LEVEL_INFO,    __VA_ARGS__); } while (0)
#define LOG_WARNING(...) do { if (log_level_enabled(LOG_LEVEL_WARNING)) log_msg(LOG_LEVEL_WARNING, __VA_ARGS__); } while (0)
#define LOG_ERROR(...)   do { if (log_level_enabled(LOG_LEVEL_ERROR))   log_msg(LOG_LEVEL_ERROR,   __VA_ARGS__); } while (0)

#endif
