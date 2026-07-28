/*
 * Unit checks for log.c sinks (console/file, timed/plain, auto-detect, reopen).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

static const char *SCRATCH = "/tmp/adn-test-log-stderr.txt";

static char *slurp(const char *path)
{
    static char buf[4096];
    FILE *fp = fopen(path, "r");
    size_t n;

    if (!fp)
        return NULL;
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    return buf;
}

static void redirect_stderr(void)
{
    if (!freopen(SCRATCH, "w", stderr))
        abort();
}

static void test_auto_detect_journal(void)
{
    setenv("JOURNAL_STREAM", "8:12345", 1);
    assert(log_auto_detect_console_timed() == 0);
    unsetenv("JOURNAL_STREAM");
}

static void test_auto_detect_non_tty_non_journal(void)
{
    /* stderr redirected to a regular file: neither journal nor a TTY. */
    redirect_stderr();
    unsetenv("JOURNAL_STREAM");
    assert(log_auto_detect_console_timed() == 0);
}

static void test_console_plain_has_no_timestamp(void)
{
    log_output_cfg_t out;
    char *line;

    memset(&out, 0, sizeof(out));
    out.console = 1;
    out.console_timed = 0;
    log_init(&out);
    log_set_channel_level(LOG_CH_APP, LOG_LEVEL_INFO);

    redirect_stderr();
    LOG_INFO("hello plain\n");
    fflush(stderr);
    line = slurp(SCRATCH);
    assert(line != NULL);
    assert(strncmp(line, "INFO/app: hello plain", strlen("INFO/app: hello plain")) == 0);
}

static void test_console_timed_has_timestamp(void)
{
    log_output_cfg_t out;
    char *line;

    memset(&out, 0, sizeof(out));
    out.console = 1;
    out.console_timed = 1;
    log_init(&out);

    redirect_stderr();
    LOG_INFO("hello timed\n");
    fflush(stderr);
    line = slurp(SCRATCH);
    assert(line != NULL);
    /* "YYYY-MM-DD HH:MM:SS,mmm INFO/app: ..." — first char is a digit, and
     * "INFO/app:" shows up after the timestamp prefix, not at offset 0. */
    assert(isdigit((unsigned char)line[0]));
    assert(strstr(line, "INFO/app: hello timed") != NULL);
    assert(strncmp(line, "INFO/app:", 9) != 0);
}

static void test_file_sink_independent_timestamp(void)
{
    log_output_cfg_t out;
    const char *file_path = "/tmp/adn-test-log-file.txt";
    char *line;

    remove(file_path);
    memset(&out, 0, sizeof(out));
    out.console = 1;
    out.console_timed = 0;
    out.file = 1;
    out.file_timed = 1;
    strncpy(out.file_path, file_path, sizeof(out.file_path) - 1);
    log_init(&out);

    redirect_stderr();
    LOG_INFO("dual sink\n");
    fflush(stderr);

    line = slurp(SCRATCH);
    assert(line != NULL);
    assert(strncmp(line, "INFO/app: dual sink", 19) == 0); /* console: no ts */

    line = slurp(file_path);
    assert(line != NULL);
    assert(isdigit((unsigned char)line[0])); /* file: timed, independent of console */
    assert(strstr(line, "INFO/app: dual sink") != NULL);

    remove(file_path);
}

static void test_reopen_files_rotates(void)
{
    log_output_cfg_t out;
    const char *file_path = "/tmp/adn-test-log-rotate.txt";
    const char *rotated_path = "/tmp/adn-test-log-rotate.txt.1";
    char *line;

    remove(file_path);
    remove(rotated_path);
    memset(&out, 0, sizeof(out));
    out.file = 1;
    out.file_timed = 0;
    strncpy(out.file_path, file_path, sizeof(out.file_path) - 1);
    log_init(&out);

    LOG_INFO("before rotate\n");
    rename(file_path, rotated_path);
    log_reopen_files();
    LOG_INFO("after rotate\n");

    line = slurp(rotated_path);
    assert(line != NULL);
    assert(strstr(line, "before rotate") != NULL);

    line = slurp(file_path);
    assert(line != NULL);
    assert(strstr(line, "after rotate") != NULL);
    assert(strstr(line, "before rotate") == NULL);

    remove(file_path);
    remove(rotated_path);
}

static void test_null_handler_silences_output(void)
{
    log_output_cfg_t out;
    char *line;

    memset(&out, 0, sizeof(out));
    log_init(&out); /* console=0, file=0 — the "null" handlers= case */

    redirect_stderr();
    LOG_INFO("should not appear\n");
    fflush(stderr);
    line = slurp(SCRATCH);
    assert(line != NULL);
    assert(line[0] == '\0');
}

int main(void)
{
    test_auto_detect_journal();
    test_auto_detect_non_tty_non_journal();
    test_console_plain_has_no_timestamp();
    test_console_timed_has_timestamp();
    test_file_sink_independent_timestamp();
    test_reopen_files_rotates();
    test_null_handler_silences_output();
    remove(SCRATCH);
    printf("test_log: ok\n");
    return 0;
}
