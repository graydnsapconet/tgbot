#define _POSIX_C_SOURCE 200809L

#include "queue.h"
#include "config.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int64_t chat_id;
    char text[1024];
    double ingress_sec; // CLOCK_MONOTONIC seconds
} Slot;

// per-user ring buffer (FIFO)
typedef struct UserRing {
    int64_t user_id;
    Slot *slots;
    int head; // next read position
    int tail; // next write position
    int count;
    int cap;               // always a power-of-2
    int cap_mask;          // cap - 1, for bitmask modulo
    struct UserRing *next; // hash chain
} UserRing;

// round up to next power-of-2 for bitmask modulo on ring indices
static int round_up_pow2(int v)
{
    if (v < 4)
        v = 4;

    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;

    return v;
}

// global queue state
static struct {
    UserRing *buckets[QUEUE_BUCKETS];
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    int total_pending; // total messages across all users
    int ring_size;
    int shutdown;
    int rr_bucket;     // round-robin cursor for fair pop
} g_queue;

static double monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static unsigned hash_user(int64_t user_id)
{
    uint64_t h = (uint64_t)user_id;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL; // MurmurHash3 fmix64 finalizer
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (unsigned)(h & QUEUE_BUCKET_MASK);
}

// find or create a ring for user_id (caller holds mtx)
static UserRing *ring_get_or_create(int64_t user_id)
{
    unsigned idx = hash_user(user_id);
    UserRing *r = g_queue.buckets[idx];
    while (r) {
        if (r->user_id == user_id) {
            return r;
        }
        r = r->next;
    }

    r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->user_id = user_id;
    r->cap = round_up_pow2(g_queue.ring_size);
    r->cap_mask = r->cap - 1;
    r->slots = calloc((size_t)r->cap, sizeof(Slot));
    if (!r->slots) {
        free(r);
        return NULL;
    }

    r->next = g_queue.buckets[idx];
    g_queue.buckets[idx] = r;
    return r;
}

/* find any non-empty ring and pop its head (caller holds mtx)
 * uses round-robin across buckets so one spammy user cannot starve others
 * writes the popped user_id into *out_user_id
 * returns 1 on success, 0 if everything is empty
 * frees the ring if it drains to zero (prevents unbounded memory growth)
 */
static int ring_pop_any(Slot *out, int64_t *out_user_id)
{
    int start = g_queue.rr_bucket;
    for (int i = 0; i < QUEUE_BUCKETS; i++) {
        int idx = (start + i) & QUEUE_BUCKET_MASK;
        UserRing *prev = NULL;
        UserRing *r = g_queue.buckets[idx];
        while (r) {
            if (r->count > 0) {
                *out = r->slots[r->head];
                *out_user_id = r->user_id;
                r->head = (r->head + 1) & r->cap_mask;
                r->count--;
                g_queue.total_pending--;
                g_queue.rr_bucket = (idx + 1) & QUEUE_BUCKET_MASK;

                // free the ring if it drained to zero
                if (r->count == 0) {
                    if (prev) {
                        prev->next = r->next;
                    } else {
                        g_queue.buckets[idx] = r->next;
                    }
                    free(r->slots);
                    free(r);
                }
                return 1;
            }
            prev = r;
            r = r->next;
        }
    }
    return 0;
}

int queue_init(int ring_size)
{
    memset(&g_queue, 0, sizeof(g_queue));
    g_queue.ring_size = ring_size > 0 ? ring_size : 30;

    if (pthread_mutex_init(&g_queue.mtx, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&g_queue.cond, NULL) != 0) {
        pthread_mutex_destroy(&g_queue.mtx);
        return -1;
    }
    return 0;
}

void queue_destroy(void)
{
    pthread_mutex_lock(&g_queue.mtx);
    for (int i = 0; i < QUEUE_BUCKETS; i++) {
        UserRing *r = g_queue.buckets[i];
        while (r) {
            UserRing *next = r->next;
            free(r->slots);
            free(r);
            r = next;
        }
        g_queue.buckets[i] = NULL;
    }
    g_queue.total_pending = 0;
    pthread_mutex_unlock(&g_queue.mtx);

    pthread_cond_destroy(&g_queue.cond);
    pthread_mutex_destroy(&g_queue.mtx);
}

int queue_push(int64_t user_id, int64_t chat_id, const char *text)
{
    pthread_mutex_lock(&g_queue.mtx);

    UserRing *r = ring_get_or_create(user_id);
    if (!r) {
        pthread_mutex_unlock(&g_queue.mtx);
        return -1;
    }

    // drop newest policy: if ring is full, reject
    if (r->count >= r->cap) {
        pthread_mutex_unlock(&g_queue.mtx);
        return -1;
    }

    Slot *s = &r->slots[r->tail];
    s->chat_id = chat_id;
    s->ingress_sec = monotonic_sec();
    size_t tlen = strlen(text);
    if (tlen >= sizeof(s->text)) {
        tlen = sizeof(s->text) - 1;
    }
    memcpy(s->text, text, tlen);
    s->text[tlen] = '\0';

    r->tail = (r->tail + 1) & r->cap_mask;
    r->count++;
    g_queue.total_pending++;

    pthread_cond_signal(&g_queue.cond);
    pthread_mutex_unlock(&g_queue.mtx);
    return 0;
}

int queue_pop(QueueMsg *out)
{
    pthread_mutex_lock(&g_queue.mtx);

    while (g_queue.total_pending == 0 && !g_queue.shutdown) {
        pthread_cond_wait(&g_queue.cond, &g_queue.mtx);
    }

    if (g_queue.shutdown && g_queue.total_pending == 0) {
        pthread_mutex_unlock(&g_queue.mtx);
        return -1;
    }

    Slot slot;
    int64_t user_id = 0;
    if (!ring_pop_any(&slot, &user_id)) {
        // spurious wakeup or drained during shutdown
        pthread_mutex_unlock(&g_queue.mtx);
        return -1;
    }

    out->user_id = user_id;
    out->chat_id = slot.chat_id;
    out->ingress_sec = slot.ingress_sec;
    size_t slen = strlen(slot.text);
    if (slen >= sizeof(out->text)) {
        slen = sizeof(out->text) - 1;
    }
    memcpy(out->text, slot.text, slen);
    out->text[slen] = '\0';

    pthread_mutex_unlock(&g_queue.mtx);
    return 0;
}

void queue_shutdown(void)
{
    pthread_mutex_lock(&g_queue.mtx);
    g_queue.shutdown = 1;
    pthread_cond_broadcast(&g_queue.cond);
    pthread_mutex_unlock(&g_queue.mtx);
}

int queue_depth(void)
{
    pthread_mutex_lock(&g_queue.mtx);
    int d = g_queue.total_pending;
    pthread_mutex_unlock(&g_queue.mtx);
    return d;
}

int queue_ring_count(void)
{
    pthread_mutex_lock(&g_queue.mtx);
    int n = 0;
    for (int i = 0; i < QUEUE_BUCKETS; i++) {
        UserRing *r = g_queue.buckets[i];
        while (r) {
            n++;
            r = r->next;
        }
    }
    pthread_mutex_unlock(&g_queue.mtx);
    return n;
}
