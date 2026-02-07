#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "test.h"
#include "../src/commands.h"
#include "../src/config.h"
#include "../src/queue.h"
#include "../src/whitelist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// helpers
static const char *TEST_WL_FILE = "/tmp/tgbot_test_cmd_wl.txt";
static void create_test_file(const char *content)
{
    FILE *f = fopen(TEST_WL_FILE, "w");
    if (f) {
        if (content) {
            fputs(content, f);
        }
        fclose(f);
    }
}
static void cleanup_test_file(void)
{
    unlink(TEST_WL_FILE);
    char tmp[280];
    snprintf(tmp, sizeof(tmp), "%s.tmp", TEST_WL_FILE);
    unlink(tmp);
}
static double monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static Config g_test_cfg;
static Whitelist g_test_wl;
static void setup_cmd(void)
{
    memset(&g_test_cfg, 0, sizeof(g_test_cfg));
    g_test_cfg.admin_user_id = 1000;
    g_test_cfg.worker_count = 2;
    g_test_cfg.user_ring_size = 16;
    snprintf(g_test_cfg.whitelist_path, sizeof(g_test_cfg.whitelist_path),
             "%s", TEST_WL_FILE);

    create_test_file("");
    whitelist_load(&g_test_wl, TEST_WL_FILE);
    queue_init(g_test_cfg.user_ring_size);
}
static void teardown_cmd(void)
{
    queue_shutdown();
    queue_destroy();
    whitelist_cleanup(&g_test_wl);
    cleanup_test_file();
}
static CmdCtx make_ctx(int64_t sender_id, int64_t chat_id, const char *bot_username)
{
    CmdCtx ctx = {
        .cfg = &g_test_cfg,
        .wl = &g_test_wl,
        .sender_id = sender_id,
        .chat_id = chat_id,
        .bot_username = bot_username,
        .boot_time = monotonic_sec() - 3723.0, // ~1h 2m 3s ago
        .worker_count = g_test_cfg.worker_count,
    };
    return ctx;
}

// tests

// /start queues a greeting
TEST(cmd_dispatch_start)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(42, 42, "testbot");
    int rc = cmd_dispatch(&ctx, "/start");
    ASSERT_EQ(rc, 1);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(strstr(out.text, "Hello") != NULL);

    teardown_cmd();
}

// /help queues help text
TEST(cmd_dispatch_help)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(42, 42, "testbot");
    int rc = cmd_dispatch(&ctx, "/help");
    ASSERT_EQ(rc, 1);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(strstr(out.text, "/start") != NULL);
    ASSERT(strstr(out.text, "/help") != NULL);
    ASSERT(strstr(out.text, "/allow") != NULL);
    ASSERT(strstr(out.text, "/status") != NULL);

    teardown_cmd();
}

// unknown command returns 0
TEST(cmd_dispatch_unknown)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(42, 42, "testbot");
    int rc = cmd_dispatch(&ctx, "/foobar");
    ASSERT_EQ(rc, 0);

    teardown_cmd();
}

// non-command text returns 0
TEST(cmd_dispatch_non_command)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(42, 42, "testbot");
    int rc = cmd_dispatch(&ctx, "hello world");
    ASSERT_EQ(rc, 0);

    teardown_cmd();
}

// /allow from non-admin -> permission denied
TEST(cmd_dispatch_allow_non_admin)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(9999, 9999, "testbot"); // not admin (1000)
    int rc = cmd_dispatch(&ctx, "/allow 123");
    ASSERT_EQ(rc, 1);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(strstr(out.text, "permission denied") != NULL);

    teardown_cmd();
}

// /allow from admin -> adds user to whitelist
TEST(cmd_dispatch_allow_admin)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(1000, 1000, "testbot"); // admin
    int rc = cmd_dispatch(&ctx, "/allow 555");
    ASSERT_EQ(rc, 1);

    // verify whitelist was updated
    ASSERT(whitelist_contains(&g_test_wl, 555));

    // pop admin confirmation message
    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(strstr(out.text, "555") != NULL);
    ASSERT(strstr(out.text, "added") != NULL);

    teardown_cmd();
}

// /revoke from admin -> removes user
TEST(cmd_dispatch_revoke_admin)
{
    setup_cmd();

    // add first, then revoke
    whitelist_add(&g_test_wl, 777);
    ASSERT(whitelist_contains(&g_test_wl, 777));

    CmdCtx ctx = make_ctx(1000, 1000, "testbot");
    int rc = cmd_dispatch(&ctx, "/revoke 777");
    ASSERT_EQ(rc, 1);

    ASSERT(!whitelist_contains(&g_test_wl, 777));

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(strstr(out.text, "777") != NULL);
    ASSERT(strstr(out.text, "removed") != NULL);

    teardown_cmd();
}
// /status from admin -> operational info
TEST(cmd_dispatch_status_admin)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(1000, 1000, "testbot");
    int rc = cmd_dispatch(&ctx, "/status");
    ASSERT_EQ(rc, 1);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(strstr(out.text, "uptime") != NULL);
    ASSERT(strstr(out.text, "queue") != NULL);
    ASSERT(strstr(out.text, "whitelist") != NULL);
    ASSERT(strstr(out.text, "workers") != NULL);

    teardown_cmd();
}

// /status from non-admin -> permission denied
TEST(cmd_dispatch_status_non_admin)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(9999, 9999, "testbot");
    int rc = cmd_dispatch(&ctx, "/status");
    ASSERT_EQ(rc, 1);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(strstr(out.text, "permission denied") != NULL);

    teardown_cmd();
}

// /help@testbot suffix is handled correctly
TEST(cmd_dispatch_at_suffix)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(42, 42, "testbot");
    int rc = cmd_dispatch(&ctx, "/help@testbot");
    ASSERT_EQ(rc, 1);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(strstr(out.text, "/help") != NULL);

    teardown_cmd();
}

// /allow with no argument -> usage message
TEST(cmd_dispatch_allow_no_arg)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(1000, 1000, "testbot");
    int rc = cmd_dispatch(&ctx, "/allow");
    ASSERT_EQ(rc, 1);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(strstr(out.text, "Usage") != NULL);

    teardown_cmd();
}

// /allow with non-numeric argument -> Invalid user ID
TEST(cmd_dispatch_allow_non_numeric)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(1000, 1000, "testbot");
    int rc = cmd_dispatch(&ctx, "/allow notanumber");
    ASSERT_EQ(rc, 1);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(strstr(out.text, "Invalid user ID") != NULL);

    teardown_cmd();
}

// /allow 0 -> Invalid user ID
TEST(cmd_dispatch_allow_zero)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(1000, 1000, "testbot");
    int rc = cmd_dispatch(&ctx, "/allow 0");
    ASSERT_EQ(rc, 1);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(strstr(out.text, "Invalid user ID") != NULL);

    teardown_cmd();
}

// /revoke with non-numeric argument -> Invalid user ID
TEST(cmd_dispatch_revoke_non_numeric)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(1000, 1000, "testbot");
    int rc = cmd_dispatch(&ctx, "/revoke abc");
    ASSERT_EQ(rc, 1);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(strstr(out.text, "Invalid user ID") != NULL);

    teardown_cmd();
}

// /allow enqueues TWO messages: admin confirmation + welcome to target
TEST(cmd_dispatch_allow_enqueues_both)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(1000, 1000, "testbot");
    int rc = cmd_dispatch(&ctx, "/allow 888");
    ASSERT_EQ(rc, 1);

    // first pop: admin confirmation (to sender 1000)
    QueueMsg out1;
    ASSERT_EQ(queue_pop(&out1), 0);
    ASSERT_EQ(out1.user_id, 1000);
    ASSERT(strstr(out1.text, "888") != NULL);
    ASSERT(strstr(out1.text, "added") != NULL);

    // second pop: welcome to target user 888
    QueueMsg out2;
    ASSERT_EQ(queue_pop(&out2), 0);
    ASSERT_EQ(out2.user_id, 888);
    ASSERT(strstr(out2.text, "granted") != NULL || strstr(out2.text, "access") != NULL);

    teardown_cmd();
}

// /help@otherbot -> NOT dispatched (returns 0)
TEST(cmd_dispatch_at_otherbot_ignored)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(42, 42, "testbot");
    int rc = cmd_dispatch(&ctx, "/help@otherbot");
    ASSERT_EQ(rc, 0); // should be ignored, not dispatched

    teardown_cmd();
}

// /start@testbot -> dispatched correctly (our bot name)
TEST(cmd_dispatch_at_ourbot_works)
{
    setup_cmd();

    CmdCtx ctx = make_ctx(42, 42, "testbot");
    int rc = cmd_dispatch(&ctx, "/start@testbot");
    ASSERT_EQ(rc, 1);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(strstr(out.text, "Hello") != NULL);

    teardown_cmd();
}

int main(void)
{
    printf("=== test_commands ===\n");
    return test_summarise();
}
