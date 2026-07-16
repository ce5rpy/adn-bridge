/*
 * Subscriber alias load and lookup (ADN subscriber_ids.json).
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

#include "aliases.h"
#include "log.h"
#include "yyjson.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Manifest key in file_checksums.json (same as new-adn-server / legacy). */
#define ALIAS_CHECKSUM_KEY_SUBSCRIBER "subscriber_ids"
#define BLAKE2B_HEX_LEN 128

#define CS_BUCKETS 65536U
#define ID_BUCKETS 65536U
/* Stale download lock: another instance may take over after this many seconds. */
#define ALIAS_DOWNLOAD_LOCK_STALE_SEC 300
/* Wait for a peer download (curl --max-time 120 + margin). */
#define ALIAS_DOWNLOAD_WAIT_SEC 150
#define ALIAS_DOWNLOAD_POLL_MS 250

typedef struct cs_entry {
    char *callsign;
    int id; /* primary ID for YSF->DMR (first file row; local overlay may replace) */
    struct cs_entry *next;
} cs_entry_t;

typedef struct id_entry {
    int id;
    char callsign[11];
    struct id_entry *next;
} id_entry_t;

struct ysf2dmr_aliases {
    cs_entry_t *cs_buckets[CS_BUCKETS];
    id_entry_t *id_buckets[ID_BUCKETS];
    size_t count; /* unique callsigns in cs_buckets */
    time_t subscriber_mtime; /* mtime of subscriber_file when last built */
    time_t local_mtime;      /* mtime of local_subscriber_file when last built */
    time_t last_mtime_check; /* last disk mtime poll (reload_minutes) */
    /* Wall-clock of last successful subscriber download this process.
     * Resets the stale_minutes countdown (0 = none this session). */
    time_t last_fetch_time;
};

static unsigned hash_str(const char *s)
{
    unsigned h = 5381U;
    while (*s)
        h = ((h << 5) + h) + (unsigned char)*s++;
    return h;
}

static unsigned hash_id(int id)
{
    return (unsigned)id * 2654435761U;
}

void ysf2dmr_aliases_cfg_init(ysf2dmr_aliases_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->stale_minutes = 24 * 60;
    cfg->reload_minutes = 15;
    strncpy(cfg->data_dir, "./data", sizeof(cfg->data_dir) - 1);
    strncpy(cfg->subscriber_file, "subscriber_ids.json", sizeof(cfg->subscriber_file) - 1);
    strncpy(cfg->subscriber_url, "https://servers.adn.systems/subscriber_ids.json",
            sizeof(cfg->subscriber_url) - 1);
    strncpy(cfg->local_subscriber_file, "local_subcriber_ids.json",
            sizeof(cfg->local_subscriber_file) - 1);
    strncpy(cfg->checksum_file, "file_checksums.json", sizeof(cfg->checksum_file) - 1);
    strncpy(cfg->checksum_url, "https://servers.adn.systems/file_checksums.json",
            sizeof(cfg->checksum_url) - 1);
}

static int file_stale(const char *path, int stale_minutes)
{
    struct stat st;

    if (stale_minutes <= 0)
        return 1;
    if (stat(path, &st) != 0)
        return 1;
    return (time(NULL) - st.st_mtime) >= (time_t)stale_minutes * 60;
}

static time_t file_mtime(const char *path)
{
    struct stat st;

    if (!path || !path[0] || stat(path, &st) != 0)
        return 0;
    return st.st_mtime;
}

static void aliases_record_mtimes(ysf2dmr_aliases_t *a, const ysf2dmr_aliases_cfg_t *cfg)
{
    char path[512];

    a->subscriber_mtime = 0;
    a->local_mtime = 0;
    if (cfg->subscriber_file[0]) {
        snprintf(path, sizeof(path), "%s/%s", cfg->data_dir, cfg->subscriber_file);
        a->subscriber_mtime = file_mtime(path);
    }
    if (cfg->local_subscriber_file[0]) {
        snprintf(path, sizeof(path), "%s/%s", cfg->data_dir, cfg->local_subscriber_file);
        a->local_mtime = file_mtime(path);
    }
    a->last_mtime_check = time(NULL);
}

/* 1 if on-disk subscriber/local JSON is newer than the last in-memory build.
 * If newer_name is non-NULL, copies the basename of the first newer file. */
static int aliases_disk_newer(const ysf2dmr_aliases_cfg_t *cfg, const ysf2dmr_aliases_t *a,
                              char *newer_name, size_t newer_name_len)
{
    char path[512];
    time_t mt;

    if (newer_name && newer_name_len)
        newer_name[0] = '\0';

    if (cfg->subscriber_file[0]) {
        snprintf(path, sizeof(path), "%s/%s", cfg->data_dir, cfg->subscriber_file);
        mt = file_mtime(path);
        if (mt > a->subscriber_mtime) {
            if (newer_name && newer_name_len)
                snprintf(newer_name, newer_name_len, "%s", cfg->subscriber_file);
            return 1;
        }
    }
    if (cfg->local_subscriber_file[0]) {
        snprintf(path, sizeof(path), "%s/%s", cfg->data_dir, cfg->local_subscriber_file);
        mt = file_mtime(path);
        if (mt > a->local_mtime) {
            if (newer_name && newer_name_len)
                snprintf(newer_name, newer_name_len, "%s", cfg->local_subscriber_file);
            return 1;
        }
    }
    return 0;
}

static void download_lock_path(char *out, size_t outlen, const char *dir, const char *file)
{
    snprintf(out, outlen, "%s/.%s.download.lock", dir, file);
}

static int download_lock_stale(const char *lockpath)
{
    struct stat st;

    if (stat(lockpath, &st) != 0)
        return 1;
    return (time(NULL) - st.st_mtime) >= (time_t)ALIAS_DOWNLOAD_LOCK_STALE_SEC;
}

static int peer_download_active(const char *lockpath)
{
    return access(lockpath, F_OK) == 0 && !download_lock_stale(lockpath);
}

/* Returns 1 on success, 0 if a live peer holds the lock, -1 on error. */
static int try_acquire_download_lock(const char *lockpath)
{
    int fd;
    char buf[64];
    ssize_t n;

    if (access(lockpath, F_OK) == 0) {
        if (!download_lock_stale(lockpath))
            return 0;
        unlink(lockpath);
    }

    fd = open(lockpath, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        if (errno == EEXIST)
            return 0;
        LOG_WARNING("aliases: cannot create download lock %s: %s\n",
                    lockpath, strerror(errno));
        return -1;
    }
    snprintf(buf, sizeof(buf), "%d %ld\n", (int)getpid(), (long)time(NULL));
    n = write(fd, buf, strlen(buf));
    close(fd);
    if (n < 0) {
        unlink(lockpath);
        return -1;
    }
    return 1;
}

static void release_download_lock(const char *lockpath)
{
    unlink(lockpath);
}

/* Block until peer releases lock or wait times out. Returns 0 when path is readable. */
static int wait_for_peer_download(const char *lockpath, const char *path, const char *file)
{
    time_t deadline = time(NULL) + (time_t)ALIAS_DOWNLOAD_WAIT_SEC;

    LOG_INFO("aliases: peer is downloading '%s', waiting...\n", file);
    while (time(NULL) < deadline) {
        if (!peer_download_active(lockpath)) {
            if (access(path, R_OK) == 0) {
                LOG_INFO("aliases: peer finished '%s'\n", file);
                return 0;
            }
            LOG_WARNING("aliases: peer lock gone but '%s' is missing\n", file);
            return -1;
        }
        usleep((useconds_t)ALIAS_DOWNLOAD_POLL_MS * 1000U);
    }

    if (!peer_download_active(lockpath) && access(path, R_OK) == 0) {
        LOG_INFO("aliases: peer finished '%s' (wait edge)\n", file);
        return 0;
    }
    if (download_lock_stale(lockpath)) {
        LOG_WARNING("aliases: peer download lock stale for '%s', taking over\n", file);
        return -1;
    }
    LOG_WARNING("aliases: timed out waiting for peer download of '%s'\n", file);
    return -1;
}

static char *read_file(const char *path, size_t *out_len);

/* Blake2b-512 hex digest (128 chars), same as Python hashlib.blake2b(). */
static int blake2b_hex_file(const char *path, char out_hex[BLAKE2B_HEX_LEN + 1])
{
    FILE *fp;
    EVP_MD_CTX *ctx;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    char buf[4096];
    size_t n;
    unsigned i;

    out_hex[0] = '\0';
    fp = fopen(path, "rb");
    if (!fp)
        return -1;
    ctx = EVP_MD_CTX_new();
    if (!ctx) {
        fclose(fp);
        return -1;
    }
    if (EVP_DigestInit_ex(ctx, EVP_blake2b512(), NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        fclose(fp);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (EVP_DigestUpdate(ctx, buf, n) != 1) {
            EVP_MD_CTX_free(ctx);
            fclose(fp);
            return -1;
        }
    }
    if (ferror(fp) || EVP_DigestFinal_ex(ctx, md, &mdlen) != 1 || mdlen != 64) {
        EVP_MD_CTX_free(ctx);
        fclose(fp);
        return -1;
    }
    EVP_MD_CTX_free(ctx);
    fclose(fp);
    for (i = 0; i < mdlen; i++)
        sprintf(out_hex + i * 2, "%02x", md[i]);
    out_hex[BLAKE2B_HEX_LEN] = '\0';
    return 0;
}

static int blake2b_matches(const char *path, const char *expected_hex)
{
    char got[BLAKE2B_HEX_LEN + 1];

    if (!expected_hex || !expected_hex[0])
        return 1;
    if (blake2b_hex_file(path, got) != 0)
        return 0;
    return strcmp(got, expected_hex) == 0;
}

/* Load expected blake2b for subscriber_ids from checksum JSON. Empty if absent. */
static void load_subscriber_checksum(const ysf2dmr_aliases_cfg_t *cfg,
                                     char out_hex[BLAKE2B_HEX_LEN + 1])
{
    char path[512];
    char *data;
    size_t len;
    yyjson_doc *doc;
    yyjson_val *root, *val;

    out_hex[0] = '\0';
    if (!cfg || !cfg->checksum_file[0])
        return;
    snprintf(path, sizeof(path), "%s/%s", cfg->data_dir, cfg->checksum_file);
    if (access(path, R_OK) != 0)
        return;
    data = read_file(path, &len);
    if (!data)
        return;
    doc = yyjson_read_opts(data, len, 0, NULL, NULL);
    free(data);
    if (!doc)
        return;
    root = yyjson_doc_get_root(doc);
    val = yyjson_is_obj(root) ? yyjson_obj_get(root, ALIAS_CHECKSUM_KEY_SUBSCRIBER) : NULL;
    if (val && yyjson_is_str(val)) {
        const char *s = yyjson_get_str(val);
        if (s && strlen(s) == BLAKE2B_HEX_LEN)
            memcpy(out_hex, s, BLAKE2B_HEX_LEN + 1);
        else if (s)
            LOG_WARNING("aliases: checksum key '%s' has unexpected length\n",
                        ALIAS_CHECKSUM_KEY_SUBSCRIBER);
    }
    yyjson_doc_free(doc);
}

/* 0 = any JSON value; 1 = subscriber_ids shape (object with results[]). */
static int json_download_valid(const char *path, int subscriber_shape)
{
    char *data;
    size_t len;
    yyjson_doc *doc;
    yyjson_read_err err;
    yyjson_val *root;

    data = read_file(path, &len);
    if (!data || len == 0) {
        free(data);
        return 0;
    }
    doc = yyjson_read_opts(data, len, 0, NULL, &err);
    free(data);
    if (!doc) {
        LOG_WARNING("aliases: ignoring corrupt JSON %s (parse error at pos %zu: %s)\n",
                    path, (size_t)err.pos, err.msg);
        return 0;
    }
    root = yyjson_doc_get_root(doc);
    if (subscriber_shape) {
        yyjson_val *results = yyjson_is_obj(root) ? yyjson_obj_get(root, "results") : NULL;
        if (!results || !yyjson_is_arr(results)) {
            LOG_WARNING("aliases: ignoring corrupt JSON %s (missing results array)\n", path);
            yyjson_doc_free(doc);
            return 0;
        }
    }
    yyjson_doc_free(doc);
    return 1;
}

/* Returns 1 if a new file was written, 0 if skipped/unchanged, -1 on error.
 * validate: 0 = no JSON check, 1 = any JSON, 2 = subscriber_ids shape.
 * expected_blake2b: optional hex digest; if set, must match before rename.
 * Also re-downloads when the local file fails that checksum (even if not stale). */
static int try_download(const char *dir, const char *file, const char *url,
                        int stale_minutes, int validate, const char *expected_blake2b)
{
    char path[512];
    char lockpath[576];
    char tmp[576];
    char cmd[1024];
    int rc;
    int force = 0;

    if (!url || !url[0] || !file || !file[0])
        return 0;

    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (access(path, R_OK) == 0 && expected_blake2b && expected_blake2b[0]
        && !blake2b_matches(path, expected_blake2b)) {
        LOG_INFO("aliases: '%s' blake2b mismatch vs checksum; will re-download\n",
                 file);
        force = 1;
    }
    if (!force && !file_stale(path, stale_minutes)) {
        LOG_DEBUG("aliases: '%s' is current, not downloaded\n", file);
        return 0;
    }

    download_lock_path(lockpath, sizeof(lockpath), dir, file);
    for (;;) {
        int got = try_acquire_download_lock(lockpath);

        if (got == 1)
            break;
        if (got < 0)
            return -1;
        if (wait_for_peer_download(lockpath, path, file) == 0) {
            if (expected_blake2b && expected_blake2b[0]
                && access(path, R_OK) == 0
                && !blake2b_matches(path, expected_blake2b)) {
                LOG_WARNING("aliases: peer finished '%s' but blake2b still mismatches\n",
                            file);
                return -1;
            }
            return access(path, R_OK) == 0 ? 1 : 0;
        }
        /* Peer failed, file missing, or stale lock — retry acquire/download. */
    }

    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    snprintf(cmd, sizeof(cmd),
             "mkdir -p '%s' && curl -fsSL --max-time 120 -o '%s' '%s'",
             dir, tmp, url);
    LOG_INFO("aliases: downloading '%s'...\n", file);
    rc = system(cmd);
    if (rc != 0) {
        LOG_WARNING("aliases: download failed for '%s' (curl exit %d); keeping previous file\n",
                    file, rc);
        unlink(tmp);
        release_download_lock(lockpath);
        return -1;
    }
    if (validate == 1 && !json_download_valid(tmp, 0)) {
        LOG_WARNING("aliases: download of '%s' ignored (corrupt JSON); keeping previous file\n",
                    file);
        unlink(tmp);
        release_download_lock(lockpath);
        return -1;
    }
    if (validate == 2 && !json_download_valid(tmp, 1)) {
        LOG_WARNING("aliases: download of '%s' ignored (corrupt subscriber JSON); "
                    "keeping previous file\n", file);
        unlink(tmp);
        release_download_lock(lockpath);
        return -1;
    }
    if (expected_blake2b && expected_blake2b[0] && !blake2b_matches(tmp, expected_blake2b)) {
        LOG_WARNING("aliases: download of '%s' ignored (blake2b checksum mismatch); "
                    "keeping previous file\n", file);
        unlink(tmp);
        release_download_lock(lockpath);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        LOG_WARNING("aliases: rename %s -> %s failed: %s\n", tmp, path, strerror(errno));
        unlink(tmp);
        release_download_lock(lockpath);
        return -1;
    }
    LOG_INFO("aliases: download of '%s' completed and saved to disk\n", file);
    release_download_lock(lockpath);
    return 1;
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *fp;
    char *buf;
    long n;

    fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    n = ftell(fp);
    if (n < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    buf = (char *)malloc((size_t)n + 1U);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    buf[n] = '\0';
    fclose(fp);
    if (out_len)
        *out_len = (size_t)n;
    return buf;
}

static void normalize_callsign_key(const char *src, char *dst, size_t dstlen)
{
    size_t i = 0;

    while (*src && isspace((unsigned char)*src))
        src++;
    while (*src && i + 1 < dstlen) {
        if (*src == '-' || *src == '/')
            break;
        dst[i++] = (char)toupper((unsigned char)*src++);
    }
    while (i > 0 && isspace((unsigned char)dst[i - 1]))
        i--;
    dst[i] = '\0';
}

static void alias_index_id(ysf2dmr_aliases_t *a, int id, const char *callsign)
{
    char key[16];
    unsigned h;
    id_entry_t *ie;

    normalize_callsign_key(callsign, key, sizeof(key));
    if (!key[0])
        return;

    h = hash_id(id) % ID_BUCKETS;
    for (ie = a->id_buckets[h]; ie; ie = ie->next) {
        if (ie->id == id) {
            strncpy(ie->callsign, key, 10);
            ie->callsign[10] = '\0';
            return;
        }
    }
    ie = (id_entry_t *)calloc(1, sizeof(*ie));
    if (!ie)
        return;
    ie->id = id;
    strncpy(ie->callsign, key, 10);
    ie->callsign[10] = '\0';
    ie->next = a->id_buckets[h];
    a->id_buckets[h] = ie;
}

static void insert_alias(ysf2dmr_aliases_t *a, int id, const char *callsign)
{
    char key[16];
    unsigned h;
    cs_entry_t *ce;

    if (id <= 0 || !callsign || !callsign[0])
        return;

    normalize_callsign_key(callsign, key, sizeof(key));
    if (!key[0])
        return;

    h = hash_str(key) % CS_BUCKETS;
    for (ce = a->cs_buckets[h]; ce; ce = ce->next) {
        if (strcmp(ce->callsign, key) == 0) {
            /* Keep first primary id; still index this id→callsign (DMR→YSF). */
            alias_index_id(a, id, callsign);
            return;
        }
    }
    ce = (cs_entry_t *)calloc(1, sizeof(*ce));
    if (!ce)
        return;
    ce->callsign = strdup(key);
    if (!ce->callsign) {
        free(ce);
        return;
    }
    ce->id = id;
    ce->next = a->cs_buckets[h];
    a->cs_buckets[h] = ce;
    a->count++;
    alias_index_id(a, id, callsign);
}

static void upsert_alias(ysf2dmr_aliases_t *a, int id, const char *callsign)
{
    char key[16];
    unsigned h;
    cs_entry_t *ce;

    if (id <= 0 || !callsign || !callsign[0])
        return;

    normalize_callsign_key(callsign, key, sizeof(key));
    if (!key[0])
        return;

    h = hash_str(key) % CS_BUCKETS;
    for (ce = a->cs_buckets[h]; ce; ce = ce->next) {
        if (strcmp(ce->callsign, key) == 0) {
            ce->id = id; /* local overlay wins for YSF→DMR */
            break;
        }
    }
    if (!ce) {
        ce = (cs_entry_t *)calloc(1, sizeof(*ce));
        if (!ce)
            return;
        ce->callsign = strdup(key);
        if (!ce->callsign) {
            free(ce);
            return;
        }
        ce->id = id;
        ce->next = a->cs_buckets[h];
        a->cs_buckets[h] = ce;
        a->count++;
    }

    alias_index_id(a, id, callsign);
}

static int parse_subscriber_json(ysf2dmr_aliases_t *a, const char *path, int local_override)
{
    char *data;
    size_t len;
    yyjson_doc *doc;
    yyjson_read_err err;
    yyjson_val *root, *results;
    size_t idx, max;
    yyjson_val *item;
    int loaded = 0;

    data = read_file(path, &len);
    if (!data) {
        LOG_WARNING("aliases: cannot read %s: %s (ignored)\n", path, strerror(errno));
        return -1;
    }

    doc = yyjson_read_opts(data, len, 0, NULL, &err);
    if (!doc) {
        LOG_WARNING("aliases: ignoring corrupt file %s (JSON parse error at pos %zu: %s)\n",
                    path, (size_t)err.pos, err.msg);
        free(data);
        return -1;
    }

    root = yyjson_doc_get_root(doc);
    results = yyjson_obj_get(root, "results");
    if (!results || !yyjson_is_arr(results)) {
        LOG_WARNING("aliases: ignoring corrupt file %s (missing results array)\n", path);
        yyjson_doc_free(doc);
        free(data);
        return -1;
    }

    yyjson_arr_foreach(results, idx, max, item) {
        yyjson_val *idv = yyjson_obj_get(item, "id");
        yyjson_val *csv = yyjson_obj_get(item, "callsign");
        int id;

        if (!idv || !csv || !yyjson_is_str(csv))
            continue;
        if (yyjson_is_uint(idv))
            id = (int)yyjson_get_uint(idv);
        else if (yyjson_is_sint(idv))
            id = (int)yyjson_get_sint(idv);
        else
            continue;

        if (local_override)
            upsert_alias(a, id, yyjson_get_str(csv));
        else
            insert_alias(a, id, yyjson_get_str(csv));
        loaded++;
    }

    yyjson_doc_free(doc);
    free(data);
    LOG_INFO("aliases: loaded %d records from %s\n", loaded, path);
    return loaded > 0 ? 0 : -1;
}

static int load_file(ysf2dmr_aliases_t *a, const char *dir, const char *file,
                     int local_override, const char *expected_blake2b)
{
    char path[512];
    int rc;

    if (!file || !file[0])
        return 0;
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (access(path, R_OK) != 0)
        return 0;
    /* Always verify blake2b when a checksum is present (adn-server parity). */
    if (!local_override && expected_blake2b && expected_blake2b[0]
        && !blake2b_matches(path, expected_blake2b)) {
        LOG_WARNING("aliases: ignoring %s (blake2b checksum mismatch / corrupt or incomplete)\n",
                    path);
        return -1;
    }
    /* parse_subscriber_json validates JSON shape (results[]). */
    rc = parse_subscriber_json(a, path, local_override);
    if (rc < 0 && local_override) {
        LOG_WARNING("aliases: ignoring local overlay %s (corrupt JSON)\n", path);
        return 0;
    }
    return rc;
}

/* Returns 1 if subscriber_ids was freshly downloaded, 0 otherwise.
 * force_subscriber: treat subscriber as stale (e.g. missing file on reload). */
static int aliases_download(const ysf2dmr_aliases_cfg_t *cfg, int force_subscriber)
{
    char csum_path[512];
    char expect[BLAKE2B_HEX_LEN + 1];
    int sub_stale;
    int fetched = 0;

    /* Checksum is optional: download if configured; missing → JSON-only accept. */
    if (cfg->checksum_file[0] && cfg->checksum_url[0]) {
        if (try_download(cfg->data_dir, cfg->checksum_file, cfg->checksum_url,
                         force_subscriber ? 0 : cfg->stale_minutes, 1, NULL) < 0) {
            snprintf(csum_path, sizeof(csum_path), "%s/%s",
                     cfg->data_dir, cfg->checksum_file);
            if (access(csum_path, R_OK) != 0)
                LOG_INFO("aliases: no checksum file; accepting subscriber JSON if valid\n");
        }
    }

    load_subscriber_checksum(cfg, expect);
    if (expect[0])
        LOG_DEBUG("aliases: verifying subscriber_ids with blake2b from checksum file\n");
    else if (cfg->checksum_file[0])
        LOG_INFO("aliases: checksum file has no '%s' entry; accepting valid JSON\n",
                 ALIAS_CHECKSUM_KEY_SUBSCRIBER);

    sub_stale = force_subscriber ? 0 : cfg->stale_minutes;
    if (cfg->subscriber_file[0] && cfg->subscriber_url[0]) {
        if (try_download(cfg->data_dir, cfg->subscriber_file, cfg->subscriber_url,
                         sub_stale, 2, expect[0] ? expect : NULL) > 0)
            fetched = 1;
    }
    return fetched;
}

static int aliases_build(const ysf2dmr_aliases_cfg_t *cfg, ysf2dmr_aliases_t **out)
{
    ysf2dmr_aliases_t *a;
    char expect[BLAKE2B_HEX_LEN + 1];
    char path[512];
    int sub_rc;

    a = (ysf2dmr_aliases_t *)calloc(1, sizeof(*a));
    if (!a)
        return -1;

    load_subscriber_checksum(cfg, expect);
    if (expect[0])
        LOG_DEBUG("aliases: requiring blake2b match for subscriber_ids\n");

    /* Main DB: if the file exists it must pass JSON + checksum (when present). */
    if (cfg->subscriber_file[0]) {
        snprintf(path, sizeof(path), "%s/%s", cfg->data_dir, cfg->subscriber_file);
        if (access(path, R_OK) == 0) {
            sub_rc = load_file(a, cfg->data_dir, cfg->subscriber_file, 0,
                               expect[0] ? expect : NULL);
            if (sub_rc < 0) {
                LOG_WARNING("aliases: subscriber file ignored (corrupt or checksum failed); "
                            "aborting in-memory rebuild\n");
                ysf2dmr_aliases_free(a);
                return -1;
            }
        }
    }

    /* Local overlay is optional; invalid local is skipped, not fatal. */
    load_file(a, cfg->data_dir, cfg->local_subscriber_file, 1, NULL);
    aliases_record_mtimes(a, cfg);

    if (a->count == 0) {
        LOG_WARNING("aliases: no subscriber records loaded (YSF talker lookup disabled)\n");
    } else {
        LOG_INFO("aliases: in-memory table ready (%zu unique callsigns; "
                 "all DMR IDs indexed for DMR→YSF)\n",
                 a->count);
    }

    *out = a;
    return 0;
}

int ysf2dmr_aliases_load(const ysf2dmr_aliases_cfg_t *cfg, ysf2dmr_aliases_t **out)
{
    int fetched;

    if (!cfg || !out)
        return -1;

    fetched = aliases_download(cfg, 0);
    if (aliases_build(cfg, out) != 0)
        return -1;
    if (fetched) {
        (*out)->last_fetch_time = time(NULL);
        if (cfg->stale_minutes > 0)
            LOG_INFO("aliases: stale timer reset after download "
                     "(next re-download in %d min)\n",
                     cfg->stale_minutes);
    }
    return 0;
}

static int aliases_needs_download(const ysf2dmr_aliases_cfg_t *cfg,
                                  const ysf2dmr_aliases_t *a)
{
    char path[512];
    char expect[BLAKE2B_HEX_LEN + 1];
    time_t now;

    if (cfg->stale_minutes <= 0)
        return 0;
    if (!cfg->subscriber_file[0] || !cfg->subscriber_url[0])
        return 0;

    /* A successful download (e.g. forced from reload_minutes) restarts stale. */
    now = time(NULL);
    if (a && a->last_fetch_time > 0
        && (now - a->last_fetch_time) < (time_t)cfg->stale_minutes * 60)
        return 0;

    snprintf(path, sizeof(path), "%s/%s", cfg->data_dir, cfg->subscriber_file);
    if (file_stale(path, cfg->stale_minutes))
        return 1;
    load_subscriber_checksum(cfg, expect);
    if (expect[0] && access(path, R_OK) == 0 && !blake2b_matches(path, expect))
        return 1;
    return 0;
}

int ysf2dmr_aliases_maybe_refresh(const ysf2dmr_aliases_cfg_t *cfg,
                                  ysf2dmr_aliases_t **aliases)
{
    ysf2dmr_aliases_t *a;
    ysf2dmr_aliases_t *neu = NULL;
    ysf2dmr_aliases_t *old;
    size_t prev_count;
    time_t now;
    int did_download = 0;
    int want_reload = 0;

    if (!cfg || !aliases || !*aliases)
        return 0;

    a = *aliases;
    now = time(NULL);

    if (aliases_needs_download(cfg, a)) {
        LOG_INFO("aliases: subscriber data stale or checksum mismatch; starting download\n");
        if (aliases_download(cfg, 0) > 0)
            did_download = 1;
        want_reload = 1;
    }

    if (!want_reload && cfg->reload_minutes > 0) {
        if (now - a->last_mtime_check >= (time_t)cfg->reload_minutes * 60) {
            char newer[64];
            char path[512];

            a->last_mtime_check = now;
            snprintf(path, sizeof(path), "%s/%s", cfg->data_dir, cfg->subscriber_file);
            if (cfg->subscriber_file[0] && access(path, R_OK) != 0) {
                if (cfg->subscriber_url[0]) {
                    LOG_INFO("aliases: subscriber file missing ('%s'); forcing download\n",
                             cfg->subscriber_file);
                    if (aliases_download(cfg, 1) > 0)
                        did_download = 1;
                    want_reload = 1;
                } else {
                    LOG_WARNING("aliases: subscriber file missing ('%s') and "
                                "subscriber_url empty; keeping previous in-memory table\n",
                                cfg->subscriber_file);
                }
            } else if (aliases_disk_newer(cfg, a, newer, sizeof(newer))) {
                LOG_INFO("aliases: newer on-disk file detected ('%s'); "
                         "attempting in-memory reload\n",
                         newer[0] ? newer : "?");
                want_reload = 1;
            }
        }
    }

    if (!want_reload)
        return 0;

    prev_count = a->count;
    LOG_INFO("aliases: rebuilding in-memory table from disk "
             "(previous entries=%zu)%s\n",
             prev_count, did_download ? " after download" : "");
    /* aliases_build always re-checks blake2b (if present) and JSON validity. */
    if (aliases_build(cfg, &neu) != 0) {
        LOG_WARNING("aliases: in-memory table NOT updated "
                    "(file ignored: corrupt JSON or checksum mismatch); "
                    "keeping previous table (%zu entries)\n",
                    prev_count);
        return -1;
    }
    if (neu->count == 0 && prev_count > 0) {
        LOG_WARNING("aliases: in-memory table NOT updated "
                    "(rebuild produced 0 entries); keeping previous table (%zu entries)\n",
                    prev_count);
        ysf2dmr_aliases_free(neu);
        return -1;
    }

    /* Keep or reset stale countdown across the hot-swap. */
    if (did_download) {
        neu->last_fetch_time = time(NULL);
        if (cfg->stale_minutes > 0)
            LOG_INFO("aliases: stale timer reset after download "
                     "(next re-download in %d min)\n",
                     cfg->stale_minutes);
    } else {
        neu->last_fetch_time = a->last_fetch_time;
    }

    old = *aliases;
    *aliases = neu;
    ysf2dmr_aliases_free(old);
    LOG_INFO("aliases: in-memory table UPDATED (%zu -> %zu entries)%s\n",
             prev_count, neu->count,
             did_download ? " after download" : " from newer on-disk file");
    return 1;
}

void ysf2dmr_aliases_free(ysf2dmr_aliases_t *a)
{
    unsigned i;

    if (!a)
        return;
    for (i = 0; i < CS_BUCKETS; i++) {
        cs_entry_t *ce = a->cs_buckets[i];
        while (ce) {
            cs_entry_t *n = ce->next;
            free(ce->callsign);
            free(ce);
            ce = n;
        }
    }
    for (i = 0; i < ID_BUCKETS; i++) {
        id_entry_t *ie = a->id_buckets[i];
        while (ie) {
            id_entry_t *n = ie->next;
            free(ie);
            ie = n;
        }
    }
    free(a);
}

int ysf2dmr_alias_lookup_id(const ysf2dmr_aliases_t *aliases, const char *callsign)
{
    char key[16];
    unsigned h;
    cs_entry_t *ce;

    if (!aliases || !callsign || !callsign[0])
        return 0;
    normalize_callsign_key(callsign, key, sizeof(key));
    if (!key[0])
        return 0;
    h = hash_str(key) % CS_BUCKETS;
    for (ce = aliases->cs_buckets[h]; ce; ce = ce->next) {
        if (strcmp(ce->callsign, key) == 0)
            return ce->id;
    }
    return 0;
}

int ysf2dmr_alias_lookup_callsign(const ysf2dmr_aliases_t *aliases, int dmrid, char out[10])
{
    unsigned h;
    id_entry_t *ie;
    int i;

    if (!aliases || dmrid <= 0 || !out)
        return 0;
    memset(out, ' ', 10);
    h = hash_id(dmrid) % ID_BUCKETS;
    for (ie = aliases->id_buckets[h]; ie; ie = ie->next) {
        if (ie->id == dmrid) {
            for (i = 0; i < 10 && ie->callsign[i]; i++)
                out[i] = (char)toupper((unsigned char)ie->callsign[i]);
            return 1;
        }
    }
    return 0;
}
