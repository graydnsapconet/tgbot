#pragma once

#include "cfg.h"

#include <stdbool.h>

// forward-declare opaque cJSON so callers don't need the header
typedef struct cJSON cJSON;

/* start the webhook HTTP server. blocks the calling thread's setup; the
 * actual request handling runs on internal libmicrohttpd threads
 * returns 0 on success
 */
int webhook_start(const Config *cfg, void *update_ctx);

// stop the webhook server and free resources
void webhook_stop(void);

// returns true if the webhook server is currently running
bool webhook_running(void);

/* callback type for processing a parsed update JSON object
 * update_ctx is the opaque pointer passed to webhook_start()
 */
typedef void (*webhook_update_cb)(void *ctx, cJSON *update);

// set the callback that the webhook handler calls for each update
void webhook_set_update_cb(webhook_update_cb cb);
