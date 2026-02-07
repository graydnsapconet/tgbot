#pragma once

#include <stdbool.h>
#include <stdint.h>

// runtime configuration loaded from INI file + env-var overrides
typedef struct {
    // [bot]
    char token[256];
    int reply_delay;
    int poll_timeout;
    int poll_limit;
    char whitelist_path[256];

    // [webhook]
    bool webhook_enabled;
    int webhook_port;
    char webhook_secret[256];
    int webhook_threads;
    int webhook_pool_size;

    // [group]
    int64_t home_group_id; // 0 = not set (accept all groups)

    // [admin]
    int64_t admin_user_id; // 0 = no admin commands

    // [workers]
    int worker_count;
    int user_ring_size;

    // [log]
    char log_path[256];
    int log_max_size_mb;
} Config;

// load config from INI file, then overlay environment variables
// returns 0 on success, non-zero on error
int config_load(Config *cfg, const char *ini_path);

// dump the loaded config to stdout (secrets redacted)
void config_dump(const Config *cfg);
