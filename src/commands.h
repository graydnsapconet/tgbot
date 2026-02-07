#pragma once

#include "cfg.h"
#include "queue.h"
#include "whitelist.h"

#include <stdint.h>

// command handler context - passed to every slash-command handler
typedef struct {
    const Config *cfg;
    Whitelist *wl;
    int64_t sender_id;
    int64_t chat_id;
    const char *bot_username; // e.g. "mybot" (without @)
    double boot_time;         // CLOCK_MONOTONIC seconds at startup
    int worker_count;
} CmdCtx;

// try to dispatch a slash command
// returns 1 if the text was a known command (handled), 0 if not a command
int cmd_dispatch(const CmdCtx *ctx, const char *text);
