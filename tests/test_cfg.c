#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "test.h"
#include "../src/cfg.h"
#include "../src/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// helpers
static const char *TMP_INI = "/tmp/tgbot_test_cfg.ini";
static void write_ini(const char *content)
{
    FILE *f = fopen(TMP_INI, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}
static void cleanup_ini(void)
{
    unlink(TMP_INI);
}
static void clear_env(void)
{
    unsetenv("TELEGRAM_BOT_TOKEN");
    unsetenv("WEBHOOK_SECRET");
    unsetenv("T_TOKEN");
    unsetenv("T_SECRET");
}

// tests

// load from INI with known values
TEST(cfg_load_ini_values)
{
    clear_env();
    write_ini(
        "[bot]\n"
        "token = abc123\n"
        "reply_delay = 5\n"
        "poll_timeout = 45\n"
        "poll_limit = 50\n"
        "whitelist_path = /tmp/wl.txt\n"
        "\n"
        "[webhook]\n"
        "enabled = true\n"
        "port = 9000\n"
        "secret = mysecret\n"
        "threads = 8\n"
        "pool_size = 16\n"
        "\n"
        "[workers]\n"
        "count = 4\n"
        "ring_size = 64\n"
        "\n"
        "[log]\n"
        "path = /tmp/test.log\n"
        "max_size_mb = 50\n");

    Config cfg;
    ASSERT_EQ(config_load(&cfg, TMP_INI), 0);

    ASSERT_STR_EQ(cfg.token, "abc123");
    ASSERT_EQ(cfg.reply_delay, 5);
    ASSERT_EQ(cfg.poll_timeout, 45);
    ASSERT_EQ(cfg.poll_limit, 50);
    ASSERT_STR_EQ(cfg.whitelist_path, "/tmp/wl.txt");
    ASSERT(cfg.webhook_enabled);
    ASSERT_EQ(cfg.webhook_port, 9000);
    ASSERT_STR_EQ(cfg.webhook_secret, "mysecret");
    ASSERT_EQ(cfg.webhook_threads, 8);
    ASSERT_EQ(cfg.webhook_pool_size, 16);
    ASSERT_EQ(cfg.worker_count, 4);
    ASSERT_EQ(cfg.user_ring_size, 64);
    ASSERT_STR_EQ(cfg.log_path, "/tmp/test.log");
    ASSERT_EQ(cfg.log_max_size_mb, 50);

    cleanup_ini();
}

// TELEGRAM_BOT_TOKEN overrides INI token
TEST(cfg_env_overrides_ini_token)
{
    clear_env();
    write_ini("[bot]\ntoken = ini_token\n");
    setenv("TELEGRAM_BOT_TOKEN", "env_token", 1);

    Config cfg;
    ASSERT_EQ(config_load(&cfg, TMP_INI), 0);
    ASSERT_STR_EQ(cfg.token, "env_token");

    clear_env();
    cleanup_ini();
}

// T_TOKEN overrides both
TEST(cfg_t_token_overrides_all)
{
    clear_env();
    write_ini("[bot]\ntoken = ini_token\n");
    setenv("TELEGRAM_BOT_TOKEN", "env_token", 1);
    setenv("T_TOKEN", "t_token", 1);

    Config cfg;
    ASSERT_EQ(config_load(&cfg, TMP_INI), 0);
    ASSERT_STR_EQ(cfg.token, "t_token");

    clear_env();
    cleanup_ini();
}

// T_SECRET overrides WEBHOOK_SECRET
TEST(cfg_t_secret_overrides)
{
    clear_env();
    write_ini("[bot]\ntoken = tok\n[webhook]\nsecret = ini_sec\n");
    setenv("WEBHOOK_SECRET", "env_sec", 1);
    setenv("T_SECRET", "t_sec", 1);

    Config cfg;
    ASSERT_EQ(config_load(&cfg, TMP_INI), 0);
    ASSERT_STR_EQ(cfg.webhook_secret, "t_sec");

    clear_env();
    cleanup_ini();
}

// empty token everywhere -> error
TEST(cfg_empty_token_fails)
{
    clear_env();
    write_ini("[bot]\ntoken =\n");

    Config cfg;
    ASSERT_EQ(config_load(&cfg, TMP_INI), -1);

    cleanup_ini();
}

// missing INI file -> fallback to defaults + env
TEST(cfg_missing_ini_uses_env)
{
    clear_env();
    setenv("TELEGRAM_BOT_TOKEN", "env_only_token", 1);

    Config cfg;
    ASSERT_EQ(config_load(&cfg, "/tmp/nonexistent_tgbot_cfg.ini"), 0);
    ASSERT_STR_EQ(cfg.token, "env_only_token");
    ASSERT_EQ(cfg.reply_delay, CFG_DEFAULT_REPLY_DELAY);
    ASSERT_EQ(cfg.poll_timeout, CFG_DEFAULT_POLL_TIMEOUT);

    clear_env();
}

// integer overflow clamping
TEST(cfg_integer_clamping)
{
    clear_env();
    write_ini(
        "[bot]\n"
        "token = tok\n"
        "reply_delay = 999\n"
        "poll_timeout = 999\n"
        "poll_limit = 999\n"
        "\n"
        "[workers]\n"
        "count = 100\n"
        "ring_size = 9999\n");

    Config cfg;
    ASSERT_EQ(config_load(&cfg, TMP_INI), 0);
    // values should be clamped to their upper bounds
    ASSERT(cfg.reply_delay <= 300);
    ASSERT(cfg.poll_timeout <= 120);
    ASSERT(cfg.poll_limit <= 100);
    ASSERT(cfg.worker_count <= 16);
    ASSERT(cfg.user_ring_size <= 256);

    cleanup_ini();
}

// NULL config pointer -> error
TEST(cfg_null_config_fails)
{
    ASSERT_EQ(config_load(NULL, TMP_INI), -1);
}

// defaults are correct when no INI and only env token
TEST(cfg_defaults)
{
    clear_env();
    setenv("TELEGRAM_BOT_TOKEN", "tok", 1);

    Config cfg;
    ASSERT_EQ(config_load(&cfg, NULL), 0);
    ASSERT_EQ(cfg.worker_count, CFG_DEFAULT_WORKER_COUNT);
    ASSERT_EQ(cfg.user_ring_size, CFG_DEFAULT_USER_RING_SIZE);
    ASSERT_EQ(cfg.webhook_port, CFG_DEFAULT_WEBHOOK_PORT);
    ASSERT(!cfg.webhook_enabled);

    clear_env();
}

int main(void)
{
    printf("=== test_cfg ===\n");
    return test_summarise();
}
