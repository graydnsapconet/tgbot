#define _POSIX_C_SOURCE 200809L

#include "whitelist.h"
#include "logger.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int cmp_i64(const void *a, const void *b)
{
    int64_t va = *(const int64_t *)a;
    int64_t vb = *(const int64_t *)b;
    return (va > vb) - (va < vb);
}

static int bsearch_idx(const Whitelist *wl, int64_t user_id, bool *found)
{
    int lo = 0, hi = wl->count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (wl->ids[mid] < user_id) {
            lo = mid + 1;
        } else if (wl->ids[mid] > user_id) {
            hi = mid;
        } else {
            *found = true;
            return mid;
        }
    }
    *found = false;
    return lo;
}

static int read_ids(Whitelist *wl)
{
    FILE *fp = fopen(wl->path, "r");
    if (!fp) {
        if (errno == ENOENT) {
            // first run - create an empty file with restrictive permissions
            fp = fopen(wl->path, "w");
            if (fp) {
                fchmod(fileno(fp), 0600);
                fclose(fp);
            }
            wl->count = 0;
            return 0;
        }
        perror("whitelist: open");
        return -1;
    }

    wl->count = 0;
    char line[64];
    while (fgets(line, sizeof(line), fp)) {
        if (wl->count >= WHITELIST_MAX) {
            log_warn("whitelist: max capacity (%d) reached", WHITELIST_MAX);
            break;
        }
        int64_t id = 0;
        if (sscanf(line, "%" SCNd64, &id) == 1) {
            wl->ids[wl->count++] = id;
        }
    }

    fclose(fp);
    // sort for binary search in whitelist_contains
    if (wl->count > 1) {
        qsort(wl->ids, (size_t)wl->count, sizeof(wl->ids[0]), cmp_i64);
    }
    return 0;
}

int whitelist_load(Whitelist *wl, const char *path)
{
    if (!wl || !path) {
        return -1;
    }
    memset(wl, 0, sizeof(*wl));
    if (pthread_rwlock_init(&wl->lock, NULL) != 0) {
        return -1;
    }
    snprintf(wl->path, sizeof(wl->path), "%s", path);
    return read_ids(wl);
}

int whitelist_save(const Whitelist *wl)
{
    // atomic save: write a temp file, then rename() over the real path
    char tmp_path[sizeof(wl->path) + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", wl->path);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        log_error("whitelist: save (tmp): %s", strerror(errno));
        return -1;
    }
    // restrict permissions before writing sensitive user IDs
    fchmod(fileno(fp), 0600);
    for (int i = 0; i < wl->count; i++) {
        if (fprintf(fp, "%" PRId64 "\n", wl->ids[i]) < 0) {
            log_error("whitelist: write: %s", strerror(errno));
            fclose(fp);
            remove(tmp_path);
            return -1;
        }
    }
    if (fclose(fp) != 0) {
        log_error("whitelist: close (tmp): %s", strerror(errno));
        remove(tmp_path);
        return -1;
    }
    if (rename(tmp_path, wl->path) != 0) {
        log_error("whitelist: rename: %s", strerror(errno));
        remove(tmp_path);
        return -1;
    }
    return 0;
}

bool whitelist_contains(const Whitelist *wl, int64_t user_id)
{
    pthread_rwlock_rdlock((pthread_rwlock_t *)&wl->lock);
    bool found = false;
    bsearch_idx(wl, user_id, &found);
    pthread_rwlock_unlock((pthread_rwlock_t *)&wl->lock);
    return found;
}

int whitelist_add(Whitelist *wl, int64_t user_id)
{
    pthread_rwlock_wrlock(&wl->lock);
    bool found = false;
    int pos = bsearch_idx(wl, user_id, &found);
    if (found) {
        pthread_rwlock_unlock(&wl->lock);
        return 1; // already present
    }
    if (wl->count >= WHITELIST_MAX) {
        pthread_rwlock_unlock(&wl->lock);
        log_warn("whitelist: full");
        return -1;
    }
    // shift elements right to make room at sorted position
    if (pos < wl->count) {
        memmove(&wl->ids[pos + 1], &wl->ids[pos], (size_t)(wl->count - pos) * sizeof(wl->ids[0]));
    }
    wl->ids[pos] = user_id;
    wl->count++;
    int rc = whitelist_save(wl);
    pthread_rwlock_unlock(&wl->lock);
    return rc;
}

int whitelist_remove(Whitelist *wl, int64_t user_id)
{
    pthread_rwlock_wrlock(&wl->lock);
    bool found = false;
    int idx = bsearch_idx(wl, user_id, &found);
    if (!found) {
        pthread_rwlock_unlock(&wl->lock);
        return 1; // not found
    }

    // shift remaining entries left with memmove
    int remaining = wl->count - idx - 1;
    if (remaining > 0) {
        memmove(&wl->ids[idx], &wl->ids[idx + 1], (size_t)remaining * sizeof(wl->ids[0]));
    }
    wl->count--;
    int rc = whitelist_save(wl);
    pthread_rwlock_unlock(&wl->lock);
    return rc;
}

int whitelist_count(const Whitelist *wl)
{
    pthread_rwlock_rdlock((pthread_rwlock_t *)&wl->lock);
    int n = wl->count;
    pthread_rwlock_unlock((pthread_rwlock_t *)&wl->lock);
    return n;
}

void whitelist_cleanup(Whitelist *wl)
{
    if (wl) {
        pthread_rwlock_destroy(&wl->lock);
    }
}
