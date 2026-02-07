#define _POSIX_C_SOURCE 200809L

#include "webhook.h"
#include "cJSON.h"
#include "config.h"
#include "logger.h"

#include <inttypes.h>
#include <microhttpd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PB_POOL_MAX 64

// constant-time string comparison to prevent timing side-channel on secret
static int ct_strcmp(const char *a, const char *b)
{
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    size_t maxlen = alen > blen ? alen : blen;
    volatile unsigned char result = (unsigned char)(alen ^ blen);
    for (size_t i = 0; i <= maxlen; i++) {
        unsigned char ca = i < alen + 1 ? (unsigned char)a[i] : 0;
        unsigned char cb = i < blen + 1 ? (unsigned char)b[i] : 0;
        result |= ca ^ cb;
    }
    return result != 0;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} PostBody;

static struct {
    PostBody pool[PB_POOL_MAX];
    int free_stack[PB_POOL_MAX]; // indices of free slots
    int free_top;                // stack pointer
    int pool_size;               // configured pool size (<= PB_POOL_MAX)
    pthread_mutex_t mtx;
} g_pb_pool;

static void pb_pool_init(int pool_size)
{
    if (pool_size < 1) {
        pool_size = 8;
    }
    if (pool_size > PB_POOL_MAX) {
        pool_size = PB_POOL_MAX;
    }
    pthread_mutex_init(&g_pb_pool.mtx, NULL);
    g_pb_pool.pool_size = pool_size;
    g_pb_pool.free_top = pool_size;
    for (int i = 0; i < pool_size; i++) {
        g_pb_pool.free_stack[i] = i;
        g_pb_pool.pool[i].data = NULL;
        g_pb_pool.pool[i].len = 0;
        g_pb_pool.pool[i].cap = 0;
    }
}

static PostBody *pb_acquire(void)
{
    PostBody *pb = NULL;
    pthread_mutex_lock(&g_pb_pool.mtx);
    if (g_pb_pool.free_top > 0) {
        int idx = g_pb_pool.free_stack[--g_pb_pool.free_top];
        pb = &g_pb_pool.pool[idx];
        pb->len = 0; // reset for reuse (keep existing cap/data buffer)
    }
    pthread_mutex_unlock(&g_pb_pool.mtx);
    if (!pb) {
        pb = calloc(1, sizeof(*pb)); // fallback: heap alloc
    }
    return pb;
}

static void pb_release(PostBody *pb)
{
    if (!pb)
        return;
    if (pb >= g_pb_pool.pool && pb < g_pb_pool.pool + g_pb_pool.pool_size) {
        pb->len = 0;
        pthread_mutex_lock(&g_pb_pool.mtx);
        int idx = (int)(pb - g_pb_pool.pool);
        g_pb_pool.free_stack[g_pb_pool.free_top++] = idx;
        pthread_mutex_unlock(&g_pb_pool.mtx);
    } else {
        free(pb->data);
        free(pb);
    }
}

static void pb_pool_destroy(void)
{
    for (int i = 0; i < g_pb_pool.pool_size; i++) {
        free(g_pb_pool.pool[i].data);
        g_pb_pool.pool[i].data = NULL;
        g_pb_pool.pool[i].cap = 0;
    }
    pthread_mutex_destroy(&g_pb_pool.mtx);
}

static struct {
    struct MHD_Daemon *daemon;
    char secret[256];
    webhook_update_cb update_cb;
    void *update_ctx;
} g_webhook;

static enum MHD_Result on_request(void *cls, struct MHD_Connection *conn, const char *url,
                                  const char *method, const char *version, const char *upload_data,
                                  size_t *upload_data_size, void **req_cls)
{
    (void)cls;
    (void)version;

    if (*req_cls == NULL) {
        PostBody *pb = pb_acquire();
        if (!pb) {
            return MHD_NO;
        }
        *req_cls = pb;
        return MHD_YES; // continue to receive data
    }

    PostBody *pb = (PostBody *)*req_cls;

    if (*upload_data_size > 0) {
        size_t need = pb->len + *upload_data_size + 1;
        if (need > RESPONSE_BUF_MAX) {
            // body exceeds size limit - stop accumulating
            pb->len = RESPONSE_BUF_MAX + 1; // mark as oversized
            *upload_data_size = 0;
            return MHD_YES;
        }
        if (need > pb->cap) {
            size_t new_cap = pb->cap == 0 ? 4096 : pb->cap * 2;
            while (new_cap < need) {
                new_cap *= 2;
            }
            char *tmp = realloc(pb->data, new_cap);
            if (!tmp) {
                *upload_data_size = 0;
                return MHD_YES;
            }
            pb->data = tmp;
            pb->cap = new_cap;
        }
        memcpy(pb->data + pb->len, upload_data, *upload_data_size);
        pb->len += *upload_data_size;
        pb->data[pb->len] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }

    struct MHD_Response *resp = NULL;
    unsigned int status = MHD_HTTP_OK;

    // only accept POST /webhook
    if (strcmp(method, "POST") != 0 || strcmp(url, "/webhook") != 0) {
        status = MHD_HTTP_NOT_FOUND;
        resp = MHD_create_response_from_buffer(9, (void *)"not found", MHD_RESPMEM_PERSISTENT);
        goto send;
    }

    // validate secret token header
    if (g_webhook.secret[0] != '\0') {
        const char *hdr = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, WEBHOOK_SECRET_HEADER);
        if (!hdr || ct_strcmp(hdr, g_webhook.secret) != 0) {
            log_warn("webhook: secret mismatch");
            status = MHD_HTTP_FORBIDDEN;
            resp = MHD_create_response_from_buffer(9, (void *)"forbidden", MHD_RESPMEM_PERSISTENT);
            goto send;
        }
    }

    // validate Content-Type: must be application/json (with optional params)
    {
        const char *ct = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Content-Type");
        if (!ct || strncasecmp(ct, "application/json", 16) != 0) {
            status = 415; // Unsupported Media Type
            resp = MHD_create_response_from_buffer(22, (void *)"unsupported media type",
                                                   MHD_RESPMEM_PERSISTENT);
            goto send;
        }
    }

    // reject oversized bodies
    if (pb->len > RESPONSE_BUF_MAX) {
        status = 413; // Payload Too Large
        resp = MHD_create_response_from_buffer(sizeof("payload too large") - 1,
                                               (void *)"payload too large",
                                               MHD_RESPMEM_PERSISTENT);
        goto send;
    }

    // parse JSON body
    if (pb->data && pb->len > 0) {
        cJSON *root = cJSON_Parse(pb->data);
        if (root && g_webhook.update_cb) {
            g_webhook.update_cb(g_webhook.update_ctx, root);
        } else {
            cJSON_Delete(root);
        }
    }

    resp = MHD_create_response_from_buffer(2, (void *)"ok", MHD_RESPMEM_PERSISTENT);

send:
    if (!resp) {
        resp = MHD_create_response_from_buffer(0, (void *)"", MHD_RESPMEM_PERSISTENT);
    }

    enum MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);

    pb_release(pb);
    *req_cls = NULL;

    return ret;
}

void webhook_set_update_cb(webhook_update_cb cb)
{
    g_webhook.update_cb = cb;
}

int webhook_start(const Config *cfg, void *update_ctx)
{
    if (g_webhook.daemon) {
        log_warn("webhook: already running");
        return -1;
    }

    pb_pool_init(cfg->webhook_pool_size);

    snprintf(g_webhook.secret, sizeof(g_webhook.secret), "%s", cfg->webhook_secret);
    g_webhook.update_ctx = update_ctx;

    g_webhook.daemon = MHD_start_daemon(MHD_USE_EPOLL_INTERNALLY | MHD_USE_ERROR_LOG,
                                        (uint16_t)cfg->webhook_port, NULL, NULL, // accept policy
                                        on_request, NULL,                              // request handler
                                        MHD_OPTION_THREAD_POOL_SIZE,
                                        (unsigned int)cfg->webhook_threads,
                                        MHD_OPTION_CONNECTION_MEMORY_LIMIT,
                                        (size_t)RESPONSE_BUF_MAX, MHD_OPTION_END);

    if (!g_webhook.daemon) {
        log_error("webhook: failed to start MHD_Daemon on port %d", cfg->webhook_port);
        return -1;
    }

    log_info("webhook: listening on 0.0.0.0:%d", cfg->webhook_port);
    return 0;
}

void webhook_stop(void)
{
    if (g_webhook.daemon) {
        MHD_stop_daemon(g_webhook.daemon);
        g_webhook.daemon = NULL;
        pb_pool_destroy();
        log_info("webhook: stopped");
    }
}

bool webhook_running(void)
{
    return g_webhook.daemon != NULL;
}
