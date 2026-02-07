#pragma once
#include "cJSON.h"

#include <signal.h>
#include <stdint.h>

// opaque handle that holds the bot token and a reusable CURL handle
typedef struct BotHandle BotHandle;

// initialise a bot handle; returns NULL on failure
BotHandle *bot_init(const char *token);

// initialise a lightweight send-only handle (for worker threads)
// each worker needs its own CURL handle since libcurl is not thread-safe
BotHandle *bot_init_send_only(const char *token);

// release all resources associated with the handle
void bot_cleanup(BotHandle *bot);

// set a pointer to a volatile sig_atomic_t flag; when the pointed-to value
// becomes 0 all in-flight curl requests are aborted promptly.
void bot_set_abort_flag(BotHandle *bot, volatile sig_atomic_t *flag);

// call getMe and return parsed JSON root, caller frees with cJSON_Delete
cJSON *bot_get_me(BotHandle *bot);

// call getUpdates and return parsed JSON root, caller frees with cJSON_Delete
cJSON *bot_get_updates(BotHandle *bot, int64_t offset, int timeout, int limit);

// call sendMessage with plain text, returns 0 on success
int bot_send_message(BotHandle *bot, int64_t chat_id, const char *text);

// register a webhook URL with Telegram (with secret_token)
int bot_set_webhook(BotHandle *bot, const char *url, const char *secret);

// delete the currently registered webhook
int bot_delete_webhook(BotHandle *bot);

// override the API base URL (default: https://api.telegram.org/bot)
// useful for testing against a local mock server
void bot_set_api_base(BotHandle *bot, const char *base_url);

#ifdef TESTING
// allow plain HTTP connections (for mock server testing only)
void bot_set_allow_http(BotHandle *bot, int allow);
#endif
