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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Stale download lock: another instance may take over after this many seconds. */
#define ALIAS_DOWNLOAD_LOCK_STALE_SEC 300
/* Wait for a peer download (curl --max-time 120 + margin). */
#define ALIAS_DOWNLOAD_WAIT_SEC 150
#define ALIAS_DOWNLOAD_POLL_MS 250
/* Open-addressing load factor: grow when count >= cap * LOAD_NUM / LOAD_DEN. */
#define ALIAS_LOAD_NUM 7U
#define ALIAS_LOAD_DEN 10U
#define ALIAS_MIN_CAP 1024U

/* Contiguous open-addressing tables:
 *   id_tab  — every DMR ID → callsign (DMR→YSF; multi-ID callsigns all present)
 *   cs_tab  — callsign → primary ID (YSF→DMR; first file row, local overlay wins) */
typedef struct {
    int id; /* 0 = empty slot */
    char callsign[11];
} alias_id_slot_t;

typedef struct {
    char callsign[11]; /* "" = empty slot */
    int id;
} alias_cs_slot_t;

struct ysf2dmr_aliases {
    alias_id_slot_t *id_tab;
    alias_cs_slot_t *cs_tab;
    size_t cap;   /* power of two; shared by both tables */
    size_t count; /* occupied id_tab slots (= distinct DMR IDs) */
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
    cfg->try_download = 1;
    cfg->stale_minutes = 24 * 60;
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

static int try_download(const char *dir, const char *file, const char *url, int stale_minutes)
{
    char path[512];
    char lockpath[576];
    char tmp[576];
    char cmd[1024];
    int rc;

    if (!url || !url[0] || !file || !file[0])
        return 0;

    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (!file_stale(path, stale_minutes)) {
        LOG_INFO("aliases: '%s' is current, not downloaded\n", file);
        return 0;
    }

    download_lock_path(lockpath, sizeof(lockpath), dir, file);
    for (;;) {
        int got = try_acquire_download_lock(lockpath);

        if (got == 1)
            break;
        if (got < 0)
            return -1;
        if (wait_for_peer_download(lockpath, path, file) == 0)
            return 0;
        /* Peer failed, file missing, or stale lock — retry acquire/download. */
    }

    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    snprintf(cmd, sizeof(cmd),
             "mkdir -p '%s' && curl -fsSL --max-time 120 -o '%s' '%s'",
             dir, tmp, url);
    LOG_INFO("aliases: downloading %s\n", file);
    rc = system(cmd);
    if (rc != 0) {
        LOG_WARNING("aliases: download failed for %s (curl exit %d)\n", file, rc);
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
    LOG_INFO("aliases: downloaded %s\n", file);
    release_download_lock(lockpath);
    return 0;
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

static size_t next_pow2(size_t n)
{
    size_t c = ALIAS_MIN_CAP;

    if (n < ALIAS_MIN_CAP)
        return ALIAS_MIN_CAP;
    while (c < n)
        c <<= 1;
    return c;
}

/* Rehash into new power-of-two capacity. Returns 0 on success. */
static int alias_rehash(ysf2dmr_aliases_t *a, size_t new_cap)
{
    alias_id_slot_t *nid;
    alias_cs_slot_t *ncs;
    size_t i, mask;

    if (new_cap < ALIAS_MIN_CAP || (new_cap & (new_cap - 1)) != 0)
        return -1;

    nid = (alias_id_slot_t *)calloc(new_cap, sizeof(*nid));
    ncs = (alias_cs_slot_t *)calloc(new_cap, sizeof(*ncs));
    if (!nid || !ncs) {
        free(nid);
        free(ncs);
        return -1;
    }

    mask = new_cap - 1;
    if (a->id_tab) {
        for (i = 0; i < a->cap; i++) {
            int id = a->id_tab[i].id;
            unsigned h;

            if (id == 0)
                continue;
            h = hash_id(id) & (unsigned)mask;
            while (nid[h].id != 0)
                h = (h + 1) & (unsigned)mask;
            nid[h] = a->id_tab[i];
        }
    }
    if (a->cs_tab) {
        for (i = 0; i < a->cap; i++) {
            unsigned h;

            if (!a->cs_tab[i].callsign[0])
                continue;
            h = hash_str(a->cs_tab[i].callsign) & (unsigned)mask;
            while (ncs[h].callsign[0])
                h = (h + 1) & (unsigned)mask;
            ncs[h] = a->cs_tab[i];
        }
    }

    free(a->id_tab);
    free(a->cs_tab);
    a->id_tab = nid;
    a->cs_tab = ncs;
    a->cap = new_cap;
    return 0;
}

static int alias_ensure_cap(ysf2dmr_aliases_t *a)
{
    size_t need;

    if (a->cap == 0)
        return alias_rehash(a, ALIAS_MIN_CAP);

    need = a->count + 1;
    if (need * ALIAS_LOAD_DEN < a->cap * ALIAS_LOAD_NUM)
        return 0;
    return alias_rehash(a, next_pow2(a->cap + 1));
}

static int alias_put_id(ysf2dmr_aliases_t *a, int id, const char *key)
{
    unsigned h, mask;

    if (alias_ensure_cap(a) != 0)
        return 0;
    mask = (unsigned)(a->cap - 1);
    h = hash_id(id) & mask;
    for (;;) {
        if (a->id_tab[h].id == 0) {
            a->id_tab[h].id = id;
            strncpy(a->id_tab[h].callsign, key, 10);
            a->id_tab[h].callsign[10] = '\0';
            a->count++;
            return 1;
        }
        if (a->id_tab[h].id == id) {
            strncpy(a->id_tab[h].callsign, key, 10);
            a->id_tab[h].callsign[10] = '\0';
            return 0;
        }
        h = (h + 1) & mask;
    }
}

static void alias_put_cs(ysf2dmr_aliases_t *a, int id, const char *key, int prefer)
{
    unsigned h, mask;

    if (!a->cs_tab || a->cap == 0)
        return;
    mask = (unsigned)(a->cap - 1);
    h = hash_str(key) & mask;
    for (;;) {
        if (!a->cs_tab[h].callsign[0]) {
            strncpy(a->cs_tab[h].callsign, key, 10);
            a->cs_tab[h].callsign[10] = '\0';
            a->cs_tab[h].id = id;
            return;
        }
        if (strcmp(a->cs_tab[h].callsign, key) == 0) {
            if (prefer)
                a->cs_tab[h].id = id;
            return;
        }
        h = (h + 1) & mask;
    }
}

static void insert_alias(ysf2dmr_aliases_t *a, int id, const char *callsign)
{
    char key[16];

    if (id <= 0 || !callsign || !callsign[0])
        return;
    normalize_callsign_key(callsign, key, sizeof(key));
    if (!key[0])
        return;

    alias_put_id(a, id, key);
    alias_put_cs(a, id, key, 0);
}

static void upsert_alias(ysf2dmr_aliases_t *a, int id, const char *callsign)
{
    char key[16];

    if (id <= 0 || !callsign || !callsign[0])
        return;
    normalize_callsign_key(callsign, key, sizeof(key));
    if (!key[0])
        return;

    alias_put_id(a, id, key);
    alias_put_cs(a, id, key, 1);
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
        LOG_WARNING("aliases: cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }

    doc = yyjson_read_opts(data, len, 0, NULL, &err);
    if (!doc) {
        LOG_WARNING("aliases: JSON parse error in %s pos %zu: %s\n",
                    path, (size_t)err.pos, err.msg);
        free(data);
        return -1;
    }

    root = yyjson_doc_get_root(doc);
    results = yyjson_obj_get(root, "results");
    if (!results || !yyjson_is_arr(results)) {
        LOG_WARNING("aliases: no results array in %s\n", path);
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

static int load_file(ysf2dmr_aliases_t *a, const char *dir, const char *file, int local_override)
{
    char path[512];

    if (!file || !file[0])
        return 0;
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (access(path, R_OK) != 0)
        return 0;
    return parse_subscriber_json(a, path, local_override);
}

int ysf2dmr_aliases_load(const ysf2dmr_aliases_cfg_t *cfg, ysf2dmr_aliases_t **out)
{
    ysf2dmr_aliases_t *a;

    if (!cfg || !out)
        return -1;

    a = (ysf2dmr_aliases_t *)calloc(1, sizeof(*a));
    if (!a)
        return -1;

    if (cfg->try_download) {
        if (cfg->checksum_file[0] && cfg->checksum_url[0])
            try_download(cfg->data_dir, cfg->checksum_file, cfg->checksum_url, cfg->stale_minutes);
        if (cfg->subscriber_file[0] && cfg->subscriber_url[0])
            try_download(cfg->data_dir, cfg->subscriber_file, cfg->subscriber_url, cfg->stale_minutes);
    }

    load_file(a, cfg->data_dir, cfg->subscriber_file, 0);
    load_file(a, cfg->data_dir, cfg->local_subscriber_file, 1);

    if (a->count == 0) {
        LOG_WARNING("aliases: no subscriber records loaded (YSF talker lookup disabled)\n");
    } else {
        LOG_INFO("aliases: in-memory table ready (%zu subscriber IDs, open-addr cap=%zu, ~%.1f MiB)\n",
                 a->count, a->cap,
                 (a->cap * (sizeof(alias_id_slot_t) + sizeof(alias_cs_slot_t)))
                     / (1024.0 * 1024.0));
    }

    *out = a;
    return 0;
}

void ysf2dmr_aliases_free(ysf2dmr_aliases_t *a)
{
    if (!a)
        return;
    free(a->id_tab);
    free(a->cs_tab);
    free(a);
}

int ysf2dmr_alias_lookup_id(const ysf2dmr_aliases_t *aliases, const char *callsign)
{
    char key[16];
    unsigned h, mask;
    size_t probes;

    if (!aliases || !aliases->cs_tab || aliases->cap == 0 || !callsign || !callsign[0])
        return 0;
    normalize_callsign_key(callsign, key, sizeof(key));
    if (!key[0])
        return 0;
    mask = (unsigned)(aliases->cap - 1);
    h = hash_str(key) & mask;
    for (probes = 0; probes < aliases->cap; probes++) {
        if (!aliases->cs_tab[h].callsign[0])
            return 0;
        if (strcmp(aliases->cs_tab[h].callsign, key) == 0)
            return aliases->cs_tab[h].id;
        h = (h + 1) & mask;
    }
    return 0;
}

int ysf2dmr_alias_lookup_callsign(const ysf2dmr_aliases_t *aliases, int dmrid, char out[10])
{
    unsigned h, mask;
    size_t probes;
    int i;

    if (!aliases || !aliases->id_tab || aliases->cap == 0 || dmrid <= 0 || !out)
        return 0;
    memset(out, ' ', 10);
    mask = (unsigned)(aliases->cap - 1);
    h = hash_id(dmrid) & mask;
    for (probes = 0; probes < aliases->cap; probes++) {
        if (aliases->id_tab[h].id == 0)
            return 0;
        if (aliases->id_tab[h].id == dmrid) {
            for (i = 0; i < 10 && aliases->id_tab[h].callsign[i]; i++)
                out[i] = (char)toupper((unsigned char)aliases->id_tab[h].callsign[i]);
            return 1;
        }
        h = (h + 1) & mask;
    }
    return 0;
}
