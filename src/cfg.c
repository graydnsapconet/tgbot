#include "cfg.h"
#include "config.h"
#include "ini.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// safe integer parser: returns 0 on success, -1 on error; clamps to [lo, hi]
static int parse_int(const char *value, int lo, int hi, int *out)
{
    char *endp = NULL;
    errno = 0;
    long v = strtol(value, &endp, 10);
    if (endp == value || *endp != '\0' || errno == ERANGE) {
        return -1;
    }
    if (v < (long)lo) {
        v = (long)lo;
    }
    if (v > (long)hi) {
        v = (long)hi;
    }
    *out = (int)v;
    return 0;
}

static void config_set_defaults(Config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->reply_delay = CFG_DEFAULT_REPLY_DELAY;
    cfg->poll_timeout = CFG_DEFAULT_POLL_TIMEOUT;
    cfg->poll_limit = CFG_DEFAULT_POLL_LIMIT;
    snprintf(cfg->whitelist_path, sizeof(cfg->whitelist_path), "%s",
             CFG_DEFAULT_WHITELIST_PATH);

    cfg->webhook_enabled = false;
    cfg->webhook_port = CFG_DEFAULT_WEBHOOK_PORT;
    cfg->webhook_threads = CFG_DEFAULT_WEBHOOK_THREADS;
    cfg->webhook_pool_size = CFG_DEFAULT_WEBHOOK_POOL_SIZE;

    cfg->home_group_id = 0;
    cfg->admin_user_id = 0;

    cfg->worker_count = CFG_DEFAULT_WORKER_COUNT;
    cfg->user_ring_size = CFG_DEFAULT_USER_RING_SIZE;

    snprintf(cfg->log_path, sizeof(cfg->log_path), "%s", LOG_DEFAULT_PATH);
    cfg->log_max_size_mb = LOG_DEFAULT_MAX_MB;

    snprintf(cfg->llm_endpoint, sizeof(cfg->llm_endpoint), "%s",
             CFG_DEFAULT_LLM_ENDPOINT);
    snprintf(cfg->llm_model, sizeof(cfg->llm_model), "%s",
             CFG_DEFAULT_LLM_MODEL);
    cfg->llm_max_tokens = CFG_DEFAULT_LLM_MAX_TOKENS;
    snprintf(cfg->llm_system_prompt, sizeof(cfg->llm_system_prompt), "%s",
             CFG_DEFAULT_LLM_SYSTEM_PROMPT);
}

static int ini_handler_cb(void *user, const char *section, const char *name, const char *value)
{
    Config *cfg = (Config *)user;

#define MATCH(s, n) (strcmp(section, (s)) == 0 && strcmp(name, (n)) == 0)

    if (MATCH("bot", "token")) {
        snprintf(cfg->token, sizeof(cfg->token), "%s", value);
    } else if (MATCH("bot", "reply_delay")) {
        parse_int(value, 0, 300, &cfg->reply_delay);
    } else if (MATCH("bot", "poll_timeout")) {
        parse_int(value, 1, 120, &cfg->poll_timeout);
    } else if (MATCH("bot", "poll_limit")) {
        parse_int(value, 1, 100, &cfg->poll_limit);
    } else if (MATCH("bot", "whitelist_path")) {
        snprintf(cfg->whitelist_path, sizeof(cfg->whitelist_path), "%s", value);
    } else if (MATCH("webhook", "enabled")) {
        cfg->webhook_enabled =
            (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
    } else if (MATCH("webhook", "port")) {
        parse_int(value, 1, 65535, &cfg->webhook_port);
    } else if (MATCH("webhook", "secret")) {
        snprintf(cfg->webhook_secret, sizeof(cfg->webhook_secret), "%s", value);
    } else if (MATCH("webhook", "threads")) {
        parse_int(value, 1, 32, &cfg->webhook_threads);
    } else if (MATCH("webhook", "pool_size")) {
        parse_int(value, 1, 64, &cfg->webhook_pool_size);
    } else if (MATCH("group", "home_group_id")) {
        cfg->home_group_id = strtoll(value, NULL, 10);
    } else if (MATCH("admin", "admin_user_id")) {
        cfg->admin_user_id = strtoll(value, NULL, 10);
    } else if (MATCH("workers", "count")) {
        parse_int(value, 1, 16, &cfg->worker_count);
    } else if (MATCH("workers", "ring_size")) {
        parse_int(value, 4, 256, &cfg->user_ring_size);
    } else if (MATCH("log", "path")) {
        snprintf(cfg->log_path, sizeof(cfg->log_path), "%s", value);
    } else if (MATCH("log", "max_size_mb")) {
        parse_int(value, 1, 1024, &cfg->log_max_size_mb);
    } else if (MATCH("llm", "endpoint")) {
        snprintf(cfg->llm_endpoint, sizeof(cfg->llm_endpoint), "%s", value);
    } else if (MATCH("llm", "model")) {
        snprintf(cfg->llm_model, sizeof(cfg->llm_model), "%s", value);
    } else if (MATCH("llm", "max_tokens")) {
        parse_int(value, 32, 4096, &cfg->llm_max_tokens);
    } else if (MATCH("llm", "system_prompt")) {
        snprintf(cfg->llm_system_prompt, sizeof(cfg->llm_system_prompt), "%s", value);
    } else {
        fprintf(stderr, "cfg: unknown key [%s] %s\n", section, name);
        return 0; // unknown key - treat as error
    }

#undef MATCH
    return 1; // success
}

static void config_overlay_env(Config *cfg)
{
    // Token: T_TOKEN > TELEGRAM_BOT_TOKEN > INI
    const char *tok = getenv("TELEGRAM_BOT_TOKEN");
    if (tok && tok[0] != '\0') {
        snprintf(cfg->token, sizeof(cfg->token), "%s", tok);
    }
    const char *t_tok = getenv("T_TOKEN");
    if (t_tok && t_tok[0] != '\0') {
        snprintf(cfg->token, sizeof(cfg->token), "%s", t_tok);
    }

    // Secret: T_SECRET > WEBHOOK_SECRET > INI
    const char *secret = getenv("WEBHOOK_SECRET");
    if (secret && secret[0] != '\0') {
        snprintf(cfg->webhook_secret, sizeof(cfg->webhook_secret), "%s", secret);
    }
    const char *t_sec = getenv("T_SECRET");
    if (t_sec && t_sec[0] != '\0') {
        snprintf(cfg->webhook_secret, sizeof(cfg->webhook_secret), "%s", t_sec);
    }
}

int config_load(Config *cfg, const char *ini_path)
{
    if (!cfg) {
        return -1;
    }

    config_set_defaults(cfg);

    if (ini_path) {
        int rc = ini_parse(ini_path, ini_handler_cb, cfg);
        if (rc < 0) {
            fprintf(stderr, "cfg: cannot open '%s' - using defaults + env\n", ini_path);
            // not fatal: we can still work with env vars
        } else if (rc > 0) {
            fprintf(stderr, "cfg: parse error on line %d of '%s'\n", rc, ini_path);
            return -1;
        }
    }

    config_overlay_env(cfg);

    // token is mandatory
    if (cfg->token[0] == '\0') {
        fprintf(stderr, "cfg: bot token not set (set in INI [bot] token, "
                        "TELEGRAM_BOT_TOKEN, or T_TOKEN env)\n");
        return -1;
    }

    // clamping:
    if (cfg->worker_count < 1) {
        cfg->worker_count = 1;
    }
    if (cfg->worker_count > 16) {
        cfg->worker_count = 16;
    }
    if (cfg->user_ring_size < 4) {
        cfg->user_ring_size = 4;
    }
    if (cfg->user_ring_size > 256) {
        cfg->user_ring_size = 256;
    }
    if (cfg->webhook_threads < 1) {
        cfg->webhook_threads = 1;
    }
    if (cfg->webhook_threads > 32) {
        cfg->webhook_threads = 32;
    }
    if (cfg->webhook_pool_size < 1) {
        cfg->webhook_pool_size = 1;
    }
    if (cfg->webhook_pool_size > 64) {
        cfg->webhook_pool_size = 64;
    }

    return 0;
}

void config_dump(const Config *cfg)
{
    printf("cfg: [bot]     token=******** reply_delay=%d "
           "poll_timeout=%d poll_limit=%d\n",
           cfg->reply_delay, cfg->poll_timeout, cfg->poll_limit);
    printf("cfg: [bot]     whitelist_path=%s\n", cfg->whitelist_path);
    printf("cfg: [webhook] enabled=%s port=%d secret=%s threads=%d pool_size=%d\n",
           cfg->webhook_enabled ? "true" : "false",
           cfg->webhook_port, cfg->webhook_secret[0] ? "********" : "(none)",
           cfg->webhook_threads, cfg->webhook_pool_size);
    printf("cfg: [group]   home_group_id=%s\n",
           cfg->home_group_id != 0 ? "****" : "(none)");
    printf("cfg: [admin]   admin_user_id=%s\n",
           cfg->admin_user_id != 0 ? "****" : "(none)");
    printf("cfg: [workers] count=%d ring_size=%d\n", cfg->worker_count, cfg->user_ring_size);
    printf("cfg: [log]     path=%s max_size_mb=%d\n", cfg->log_path, cfg->log_max_size_mb);
    printf("cfg: [llm]     endpoint=%s model=%s max_tokens=%d\n",
           cfg->llm_endpoint, cfg->llm_model[0] ? cfg->llm_model : "(server default)",
           cfg->llm_max_tokens);
}
