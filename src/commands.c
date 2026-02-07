#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "commands.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* strip optional @botusername suffix from a /command
 * writes the bare command name (without leading /) into cmd_buf
 * returns a pointer to the argument string after the command (may be "")
 */
static const char *parse_command(const char *text, char *cmd_buf, size_t cmd_buf_sz,
                                 const char *bot_username)
{
    if (!text || text[0] != '/') {
        return NULL;
    }

    const char *p = text + 1; // skip '/'
    const char *end = p;
    while (*end && *end != ' ' && *end != '@') {
        end++;
    }

    size_t cmd_len = (size_t)(end - p);
    if (cmd_len == 0 || cmd_len >= cmd_buf_sz) {
        return NULL;
    }
    memcpy(cmd_buf, p, cmd_len);
    cmd_buf[cmd_len] = '\0';

    // handle @botusername suffix: only accept our own bot, reject others
    if (*end == '@') {
        const char *at_start = end + 1;
        const char *at_end = at_start;
        while (*at_end && *at_end != ' ') {
            at_end++;
        }
        if (bot_username) {
            size_t ulen = strlen(bot_username);
            if ((size_t)(at_end - at_start) == ulen &&
                strncasecmp(at_start, bot_username, ulen) == 0) {
                end = at_end; // our bot - skip suffix
            } else {
                return NULL; // addressed to a different bot - ignore
            }
        } else {
            end = at_end; // no username known - skip any suffix
        }
    }

    // skip whitespace before arguments
    while (*end == ' ') {
        end++;
    }
    return end;
}

static void cmd_start(const CmdCtx *ctx)
{
    queue_push(ctx->sender_id, ctx->chat_id,
               "Hello! I'm tgbot. Use /help to see available commands.");
}

static void cmd_help(const CmdCtx *ctx)
{
    const char *msg = "/start  - show greeting\n"
                      "/help   - this message\n"
                      "/status - (admin) operational status\n"
                      "/allow <user_id>  - (admin) add user to whitelist\n"
                      "/revoke <user_id> - (admin) remove user from whitelist";
    queue_push(ctx->sender_id, ctx->chat_id, msg);
}

static void cmd_allow(const CmdCtx *ctx, const char *args)
{
    if (ctx->cfg->admin_user_id == 0 || ctx->sender_id != ctx->cfg->admin_user_id) {
        queue_push(ctx->sender_id, ctx->chat_id, "permission denied: admin only.");
        return;
    }

    if (!args || args[0] == '\0') {
        queue_push(ctx->sender_id, ctx->chat_id, "Usage: /allow <user_id>");
        return;
    }

    errno = 0;
    char *endptr = NULL;
    int64_t target = strtoll(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || errno == ERANGE || target == 0) {
        queue_push(ctx->sender_id, ctx->chat_id, "Invalid user ID.");
        return;
    }

    int rc = whitelist_add(ctx->wl, target);
    if (rc == 1) {
        queue_push(ctx->sender_id, ctx->chat_id, "User already whitelisted.");
    } else if (rc == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "User %" PRId64 " added to whitelist.", target);
        queue_push(ctx->sender_id, ctx->chat_id, buf);

        // notify the newly allowed user
        char welcome[128];
        snprintf(welcome, sizeof(welcome), "You have been granted access to this bot.");
        queue_push(target, target, welcome);
    } else {
        queue_push(ctx->sender_id, ctx->chat_id, "Failed to add user (whitelist full?).");
    }
}

static void cmd_revoke(const CmdCtx *ctx, const char *args)
{
    if (ctx->cfg->admin_user_id == 0 || ctx->sender_id != ctx->cfg->admin_user_id) {
        queue_push(ctx->sender_id, ctx->chat_id, "permission denied: admin only.");
        return;
    }

    if (!args || args[0] == '\0') {
        queue_push(ctx->sender_id, ctx->chat_id, "Usage: /revoke <user_id>");
        return;
    }

    errno = 0;
    char *endptr = NULL;
    int64_t target = strtoll(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || errno == ERANGE || target == 0) {
        queue_push(ctx->sender_id, ctx->chat_id, "Invalid user ID.");
        return;
    }

    int rc = whitelist_remove(ctx->wl, target);
    if (rc == 1) {
        queue_push(ctx->sender_id, ctx->chat_id, "User was not whitelisted.");
    } else if (rc == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "User %" PRId64 " removed from whitelist.", target);
        queue_push(ctx->sender_id, ctx->chat_id, buf);
    } else {
        queue_push(ctx->sender_id, ctx->chat_id, "Failed to remove user.");
    }
}

static double monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void cmd_status(const CmdCtx *ctx)
{
    if (ctx->cfg->admin_user_id == 0 || ctx->sender_id != ctx->cfg->admin_user_id) {
        queue_push(ctx->sender_id, ctx->chat_id, "permission denied: admin only.");
        return;
    }

    double uptime = monotonic_sec() - ctx->boot_time;
    int secs = (int)uptime;
    int hours = secs / 3600;
    int mins = (secs % 3600) / 60;
    secs = secs % 60;

    int depth = queue_depth();
    int wl_count = whitelist_count(ctx->wl);
    int workers = ctx->worker_count;

    char buf[256];
    snprintf(buf, sizeof(buf),
             "uptime: %dh %dm %ds\n"
             "queue: %d pending\n"
             "whitelist: %d user(s)\n"
             "workers: %d",
             hours, mins, secs, depth, wl_count, workers);
    queue_push(ctx->sender_id, ctx->chat_id, buf);
}

typedef enum { CMD_NOARG, CMD_HASARG } CmdArgType;

typedef struct {
    const char *name;
    CmdArgType arg_type;
    void (*fn_noarg)(const CmdCtx *);
    void (*fn_arg)(const CmdCtx *, const char *);
} CmdEntry;

static const CmdEntry cmd_table[] = {
    // sorted alphabetically by name
    {"allow", CMD_HASARG, NULL, cmd_allow},   {"help", CMD_NOARG, cmd_help, NULL},
    {"revoke", CMD_HASARG, NULL, cmd_revoke}, {"start", CMD_NOARG, cmd_start, NULL},
    {"status", CMD_NOARG, cmd_status, NULL},
};

static const int cmd_table_len = (int)(sizeof(cmd_table) / sizeof(cmd_table[0]));

// binary search on sorted command table
static const CmdEntry *cmd_lookup(const char *name)
{
    int lo = 0, hi = cmd_table_len - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(name, cmd_table[mid].name);
        if (cmp < 0) {
            hi = mid - 1;
        } else if (cmp > 0) {
            lo = mid + 1;
        } else {
            return &cmd_table[mid];
        }
    }
    return NULL;
}

int cmd_dispatch(const CmdCtx *ctx, const char *text)
{
    if (!text || text[0] != '/') {
        return 0;
    }

    char cmd[64];
    const char *args = parse_command(text, cmd, sizeof(cmd), ctx->bot_username);
    if (!args) {
        return 0;
    }

    const CmdEntry *entry = cmd_lookup(cmd);
    if (!entry) {
        return 0;
    }

    if (entry->arg_type == CMD_HASARG) {
        entry->fn_arg(ctx, args);
    } else {
        entry->fn_noarg(ctx);
    }

    return 1;
}
