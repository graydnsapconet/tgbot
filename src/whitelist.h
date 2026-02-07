#pragma once

#include "config.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t ids[WHITELIST_MAX];
    int count;
    char path[256];
    pthread_rwlock_t lock;
} Whitelist;

// load the whitelist from disk; creates file if missing
int whitelist_load(Whitelist *wl, const char *path);

// persist the current in-memory whitelist back to disk
int whitelist_save(const Whitelist *wl);

// check whether a user_id is on the whitelist
bool whitelist_contains(const Whitelist *wl, int64_t user_id);

// add a user_id to the whitelist and save
int whitelist_add(Whitelist *wl, int64_t user_id);

// remove a user_id from the whitelist and save
// returns 0 on success, 1 if not found, -1 on error
int whitelist_remove(Whitelist *wl, int64_t user_id);

// return the current whitelist count (thread-safe)
int whitelist_count(const Whitelist *wl);

// destroy the rwlock; call once during shutdown
void whitelist_cleanup(Whitelist *wl);
