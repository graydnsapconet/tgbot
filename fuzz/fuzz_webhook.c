#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "../src/commands.h"
#include "../src/config.h"
#include "../src/queue.h"
#include "../src/whitelist.h"
#include "../lib/cJSON.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// stub out bot_send_message — not needed for fuzzing
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

    // treat entire input as a JSON body to parse
    if (size == 0 || size > 256 * 1024) {
        return 0;
    }

    // make a NUL-terminated copy
    char *body = malloc(size + 1);
    if (!body) {
        return 0;
    }
    memcpy(body, data, size);
    body[size] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root) {
        // try to extract message text and dispatch as command
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (msg) {
            const cJSON *text_json = cJSON_GetObjectItemCaseSensitive(msg, "text");
            if (cJSON_IsString(text_json)) {
                const char *text = text_json->valuestring;
                if (text[0] == '/') {
                    CmdCtx ctx = {
                        .cfg = &g_cfg,
                        .wl = &g_wl,
                        .sender_id = 42,
                        .chat_id = 42,
                        .bot_username = "fuzzbot",
                        .boot_time = 0.0,
                        .worker_count = 1,
                    };
                    cmd_dispatch(&ctx, text);
                }
            }
        }
        cJSON_Delete(root);
    }

    // drain queue to prevent unbounded growth
    QueueMsg out;
    while (queue_depth() > 0) {
        queue_pop(&out);
    }

    free(body);
    return 0;
}
