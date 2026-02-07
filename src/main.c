#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "bot.h"
#include "cfg.h"
#include "cli.h"
#include "commands.h"
#include "config.h"
#include "llm.h"
#include "logger.h"
#include "queue.h"
#include "webhook.h"
#include "whitelist.h"

#include <curl/curl.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// globals
static volatile sig_atomic_t g_running = 1;
static Config g_cfg;
static char g_bot_username[128];
static double g_boot_time;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

// worker thread
typedef struct {
    int id;
    const char *token;
    int reply_delay;
    volatile sig_atomic_t *running;
    const char *llm_endpoint;
    const char *llm_model;
    int llm_max_tokens;
    const char *llm_system_prompt;
} WorkerArg;

static double monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *worker_main(void *arg)
{
    WorkerArg *wa = (WorkerArg *)arg;
    BotHandle *bot = bot_init_send_only(wa->token);
    if (!bot) {
        log_error("worker %d: failed to init bot handle", wa->id);
        free(wa);
        return NULL;
    }
    bot_set_abort_flag(bot, wa->running);

    log_info("worker %d: ready", wa->id);

    // each worker gets its own LLM handle (curl is not thread-safe)
    LlmHandle *llm = llm_init(wa->llm_endpoint, wa->llm_model);
    if (!llm) {
        log_warn("worker %d: LLM init failed - will echo instead", wa->id);
    } else {
        llm_set_abort_flag(llm, wa->running);
    }

    QueueMsg msg;
    while (queue_pop(&msg) == 0) {
        // enforce <=1 msg/s per user: sleep delta
        double now = monotonic_sec();
        double delta = now - msg.ingress_sec;
        double wait = (double)wa->reply_delay - delta;
        if (wait > 0.0) {
            struct timespec ts_sleep;
            ts_sleep.tv_sec = (time_t)wait;
            ts_sleep.tv_nsec = (long)((wait - (double)ts_sleep.tv_sec) * 1e9);
            while (nanosleep(&ts_sleep, &ts_sleep) < 0 && errno == EINTR) {
                if (!*wa->running) {
                    break;
                }
            }
            if (!*wa->running) {
                llm_cleanup(llm);
                bot_cleanup(bot);
                log_info("worker %d: exiting (signal)", wa->id);
                free(wa);
                return NULL;
            }
        }

        // send acknowledgment while LLM is thinking
        if (llm) {
            bot_send_message(bot, msg.chat_id, "\xE2\x9C\x8D Thinking...");
        }

        // generate reply via LLM or fall back to echo
        char reply[4096];
        if (llm && llm_chat(llm, wa->llm_system_prompt, msg.text,
                            reply, sizeof(reply), wa->llm_max_tokens) == 0) {
            bot_send_message(bot, msg.chat_id, reply);
        } else {
            char echo[1100];
            snprintf(echo, sizeof(echo), "Hello! You said: %s", msg.text);
            bot_send_message(bot, msg.chat_id, echo);
        }
    }

    llm_cleanup(llm);
    bot_cleanup(bot);
    log_info("worker %d: exiting", wa->id);
    free(wa);
    return NULL;
}

static void print_banner(const cJSON *me_root)
{
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(me_root, "result");
    const cJSON *username = cJSON_GetObjectItemCaseSensitive(result, "username");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(result, "id");

    const char *uname = cJSON_IsString(username) ? username->valuestring : "???";

    snprintf(g_bot_username, sizeof(g_bot_username), "%s", uname);

    log_info("tgbot: online as @%s (id %" PRId64 ")", uname,
           cJSON_IsNumber(id) ? (int64_t)id->valuedouble : (int64_t)0);
}

static int64_t handle_update(Whitelist *wl, const cJSON *update)
{
    const cJSON *uid_json = cJSON_GetObjectItemCaseSensitive(update, "update_id");
    if (!cJSON_IsNumber(uid_json)) {
        return -1;
    }
    int64_t update_id = (int64_t)uid_json->valuedouble;

    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(update, "message");
    if (!msg) {
        return update_id;
    }

    // extract chat first; cheapest rejection path
    const cJSON *chat = cJSON_GetObjectItemCaseSensitive(msg, "chat");
    if (!chat) {
        return update_id;
    }
    const cJSON *chat_id_json = cJSON_GetObjectItemCaseSensitive(chat, "id");
    if (!cJSON_IsNumber(chat_id_json)) {
        return update_id;
    }
    int64_t chat_id = (int64_t)chat_id_json->valuedouble;

    // home-group gating
    if (g_cfg.home_group_id != 0) {
        const cJSON *chat_type = cJSON_GetObjectItemCaseSensitive(chat, "type");
        const char *type_str = cJSON_IsString(chat_type) ? chat_type->valuestring : "private";

        if ((type_str[0] == 'g' || type_str[0] == 's') && chat_id != g_cfg.home_group_id) {
            // ignore messages from non-home groups
            return update_id;
        }
    }

    const cJSON *from = cJSON_GetObjectItemCaseSensitive(msg, "from");
    const cJSON *from_id_json = cJSON_GetObjectItemCaseSensitive(from, "id");
    if (!cJSON_IsNumber(from_id_json)) {
        return update_id;
    }
    int64_t from_id = (int64_t)from_id_json->valuedouble;

    const cJSON *text_json = cJSON_GetObjectItemCaseSensitive(msg, "text");
    const char *text = cJSON_IsString(text_json) ? text_json->valuestring : "";

    // command dispatch (runs before whitelist gate for admin cmds)
    if (text[0] == '/') {
        CmdCtx ctx = {
            .cfg = &g_cfg,
            .wl = wl,
            .sender_id = from_id,
            .chat_id = chat_id,
            .bot_username = g_bot_username,
            .boot_time = g_boot_time,
            .worker_count = g_cfg.worker_count,
        };
        if (cmd_dispatch(&ctx, text)) {
            return update_id;
        }
        // unknown slash command - don't forward to LLM
        queue_push(from_id, chat_id, "Unknown command. Try /help");
        return update_id;
    }

    // whitelist gate
    if (!whitelist_contains(wl, from_id)) {
        const cJSON *from_name = cJSON_GetObjectItemCaseSensitive(from, "first_name");
        log_info("tgbot: ignored user %" PRId64 " (%s) - not whitelisted", from_id,
               cJSON_IsString(from_name) ? from_name->valuestring : "?");
        return update_id;
    }

    // defer name lookup - only for whitelisted users
    const cJSON *from_name = cJSON_GetObjectItemCaseSensitive(from, "first_name");
    log_info("tgbot: [%" PRId64 "] %s: %s", chat_id,
           cJSON_IsString(from_name) ? from_name->valuestring : "?", text);

    // enqueue user message for worker threads (LLM generates the reply)
    if (queue_push(from_id, chat_id, text) != 0) {
        log_warn("tgbot: queue full for user %" PRId64 " - message dropped", from_id);
    }

    return update_id;
}

static void webhook_on_update(void *ctx, cJSON *update)
{
    handle_update((Whitelist *)ctx, update);
    cJSON_Delete(update);
}

int main(int argc, char **argv)
{
    // CLI subcommand dispatch (start, stop, restart, status, logs, help)
    int cli_exit = 0;
    if (cli_dispatch(argc, argv, &cli_exit) == 0) {
        return cli_exit;
    }

    // signal handlers - do NOT use SA_RESTART so pause()/sleep() return on signal
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (config_load(&g_cfg, "tgbot.ini") != 0) {
        return 1;
    }
    config_dump(&g_cfg);

    // init logger (stderr-only until this point - safe)
    if (log_init(g_cfg.log_path, (size_t)g_cfg.log_max_size_mb * 1024UL * 1024UL) != 0) {
        fprintf(stderr, "tgbot: failed to open log file '%s' - logging to stderr only\n",
                g_cfg.log_path);
    }

    g_boot_time = monotonic_sec();

    curl_global_init(CURL_GLOBAL_DEFAULT);

    // init bot handle
    BotHandle *bot = bot_init(g_cfg.token);
    if (!bot) {
        log_error("tgbot: failed to initialise bot handle");
        log_close();
        curl_global_cleanup();
        return 1;
    }
    bot_set_abort_flag(bot, &g_running);

    // verify token with getMe
    cJSON *me = bot_get_me(bot);
    if (!me) {
        log_error("tgbot: getMe failed - bad token?");
        bot_cleanup(bot);
        log_close();
        curl_global_cleanup();
        return 1;
    }
    print_banner(me);
    cJSON_Delete(me);

    Whitelist wl;
    if (whitelist_load(&wl, g_cfg.whitelist_path) != 0) {
        log_error("tgbot: failed to load whitelist");
        bot_cleanup(bot);
        log_close();
        curl_global_cleanup();
        return 1;
    }
    log_info("tgbot: whitelist loaded - %d user(s)", wl.count);

    // init message queue
    if (queue_init(g_cfg.user_ring_size) != 0) {
        log_error("tgbot: failed to init message queue");
        bot_cleanup(bot);
        log_close();
        curl_global_cleanup();
        return 1;
    }

    // spawn workers
    int nworkers = g_cfg.worker_count;
    pthread_t *workers = calloc((size_t)nworkers, sizeof(pthread_t));
    if (!workers) {
        log_error("tgbot: alloc workers failed");
        queue_destroy();
        bot_cleanup(bot);
        log_close();
        curl_global_cleanup();
        return 1;
    }

    // block SIGINT/SIGTERM in worker threads - only main thread handles them
    sigset_t block_mask, old_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGINT);
    sigaddset(&block_mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block_mask, &old_mask);

    // each worker gets a send-only bot handle
    for (int i = 0; i < nworkers; i++) {
        WorkerArg *wa = calloc(1, sizeof(*wa));
        if (!wa) {
            log_error("tgbot: alloc worker arg failed");
            break;
        }
        wa->id = i;
        wa->token = g_cfg.token;
        wa->reply_delay = g_cfg.reply_delay;
        wa->running = &g_running;
        wa->llm_endpoint = g_cfg.llm_endpoint;
        wa->llm_model = g_cfg.llm_model;
        wa->llm_max_tokens = g_cfg.llm_max_tokens;
        wa->llm_system_prompt = g_cfg.llm_system_prompt;
        if (pthread_create(&workers[i], NULL, worker_main, wa) != 0) {
            log_error("tgbot: failed to create worker %d", i);
            free(wa);
        }
    }

    // restore signal mask for main thread
    pthread_sigmask(SIG_SETMASK, &old_mask, NULL);

    if (g_cfg.webhook_enabled) {
        // webhook mode
        webhook_set_update_cb(webhook_on_update);
        if (webhook_start(&g_cfg, &wl) != 0) {
            log_error("tgbot: webhook failed to start");
            goto shutdown;
        }

        log_info("tgbot: running in webhook mode");

        while (g_running) {
            pause();
        }

        webhook_stop();
        bot_delete_webhook(bot);
    } else {
        // poll mode - delete any stale webhook first
        bot_delete_webhook(bot);

        int64_t offset = 0;
        log_info("tgbot: entering poll loop (timeout=%ds)...", g_cfg.poll_timeout);

        while (g_running) {
            cJSON *root = bot_get_updates(bot, offset, g_cfg.poll_timeout, g_cfg.poll_limit);
            if (!root) {
                if (!g_running) {
                    break;
                }
                log_warn("tgbot: getUpdates failed, retrying in 5s");
                // TODO: exponential backoff with jitter on consecutive failures
                for (int i = 0; i < 5 && g_running; i++) {
                    sleep(1);
                }
                continue;
            }

            const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");

            const cJSON *upd = NULL;
            cJSON_ArrayForEach(upd, result)
            {
                int64_t uid = handle_update(&wl, upd);
                if (uid >= 0 && uid >= offset) {
                    offset = uid + 1;
                }
            }

            cJSON_Delete(root);
        }
    }

shutdown:
    log_info("tgbot: shutting down.");

    // signal workers to exit and join
    queue_shutdown();
    for (int i = 0; i < nworkers; i++) {
        if (workers[i]) {
            pthread_join(workers[i], NULL);
        }
    }
    free(workers);

    queue_destroy();
    whitelist_cleanup(&wl);
    bot_cleanup(bot);
    explicit_bzero(g_cfg.token, sizeof(g_cfg.token));
    curl_global_cleanup();
    log_info("tgbot: clean shutdown complete");
    log_close();
    return 0;
}
