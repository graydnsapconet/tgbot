#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "../src/commands.h"
#include "../src/config.h"
#include "../src/queue.h"
#include "../src/whitelist.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// stub out bot_send_message
int bot_send_message(void *bot, int64_t chat_id, const char *text)
{
    (void)bot;
    (void)chat_id;
    (void)text;
    return 0;
}

static int g_init = 0;
static Config g_cfg;
static Whitelist g_wl;

static void init_once(void)
{
    if (g_init) {
        return;
    }
    g_init = 1;

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.admin_user_id = 1000;
    g_cfg.worker_count = 1;
    g_cfg.user_ring_size = 8;

    memset(&g_wl, 0, sizeof(g_wl));
    queue_init(g_cfg.user_ring_size);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    init_once();

    if (size == 0 || size > 64 * 1024) {
        return 0;
    }

    // make NUL-terminated copy
    char *text = malloc(size + 1);
    if (!text) {
        return 0;
    }
    memcpy(text, data, size);
    text[size] = '\0';

    // vary the CmdCtx fields based on first byte
    uint8_t flags = data[0];
    int64_t sender_id = (flags & 1) ? 1000 : 42; // admin or normal user
    int64_t chat_id = (int64_t)(flags & 0x0F) + 1;

    CmdCtx ctx = {
        .cfg = &g_cfg,
        .wl = &g_wl,
        .sender_id = sender_id,
        .chat_id = chat_id,
        .bot_username = (flags & 2) ? "fuzzbot" : NULL,
        .boot_time = 0.0,
        .worker_count = 1,
    };

    cmd_dispatch(&ctx, text);

    // drain queue
    QueueMsg out;
    while (queue_depth() > 0) {
        queue_pop(&out);
    }

    free(text);
    return 0;
}
