#pragma once

// maximum number of whitelisted users
// TODO: consider dynamic growth (realloc) instead of fixed ceiling
#define WHITELIST_MAX 256

// Telegram Bot API base URL (no trailing slash)
#define API_BASE "https://api.telegram.org/bot"

// maximum URL length for API requests
#define API_URL_MAX 512

// maximum HTTP response body size (512 KiB)
#define RESPONSE_BUF_MAX (512 * 1024)

// webhook secret-token header name
#define WEBHOOK_SECRET_HEADER "X-Telegram-Bot-Api-Secret-Token"

// hash-map bucket count for the per-user message queue (must be power-of-2)
#define QUEUE_BUCKETS 64
#define QUEUE_BUCKET_MASK (QUEUE_BUCKETS - 1)

// default log file path and maximum size
#define LOG_DEFAULT_PATH "/var/log/tgbot/tgbot.log"
#define LOG_DEFAULT_MAX_MB 10
#define LOG_PATH_MAX 256

// default config values (used by cfg.c; override in INI or env)
#define CFG_DEFAULT_REPLY_DELAY       3
#define CFG_DEFAULT_POLL_TIMEOUT      30
#define CFG_DEFAULT_POLL_LIMIT        100
#define CFG_DEFAULT_WHITELIST_PATH    "whitelist.txt"
#define CFG_DEFAULT_WEBHOOK_PORT      8443
#define CFG_DEFAULT_WEBHOOK_THREADS   4
#define CFG_DEFAULT_WEBHOOK_POOL_SIZE 8
#define CFG_DEFAULT_WORKER_COUNT      2
#define CFG_DEFAULT_USER_RING_SIZE    30
