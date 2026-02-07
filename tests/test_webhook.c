#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "test.h"
#include "../src/cfg.h"
#include "../src/config.h"
#include "../src/queue.h"
#include "../src/webhook.h"
#include "../lib/cJSON.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// setup
#define TEST_PORT 19876
#define TEST_SECRET "test-secret-token-abc123"

static int g_update_count = 0;
static pthread_mutex_t g_update_mtx = PTHREAD_MUTEX_INITIALIZER;

static void test_update_cb(void *ctx, cJSON *update)
{
    (void)ctx;
    pthread_mutex_lock(&g_update_mtx);
    g_update_count++;
    pthread_mutex_unlock(&g_update_mtx);
    cJSON_Delete(update);
}

static int get_update_count(void)
{
    pthread_mutex_lock(&g_update_mtx);
    int c = g_update_count;
    pthread_mutex_unlock(&g_update_mtx);
    return c;
}

static void reset_update_count(void)
{
    pthread_mutex_lock(&g_update_mtx);
    g_update_count = 0;
    pthread_mutex_unlock(&g_update_mtx);
}

// raw HTTP request sender - returns HTTP status code or -1 on error
static int raw_http_request(const char *host, int port, const char *request, size_t req_len,
                            char *resp_buf, size_t resp_buf_sz)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    // set a timeout so tests don't hang
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    if (send(fd, request, req_len, 0) != (ssize_t)req_len) {
        close(fd);
        return -1;
    }

    ssize_t n = recv(fd, resp_buf, resp_buf_sz - 1, 0);
    close(fd);

    if (n <= 0) {
        return -1;
    }
    resp_buf[n] = '\0';

    // parse status code from "HTTP/1.x NNN ..."
    const char *sp = strchr(resp_buf, ' ');
    if (!sp) {
        return -1;
    }
    return atoi(sp + 1);
}

// forward-declare so send_webhook_post can call send_webhook_post_ct
static int send_webhook_post_ct(const char *body, const char *secret_header,
                                const char *content_type, int *out_status);

static int send_webhook_post(const char *body, const char *secret_header, int *out_status)
{
    return send_webhook_post_ct(body, secret_header, "application/json", out_status);
}

// send a POST with a custom Content-Type (or NULL to omit it)
static int send_webhook_post_ct(const char *body, const char *secret_header,
                                const char *content_type, int *out_status)
{
    size_t body_len = strlen(body);
    size_t secret_len = secret_header ? strlen(secret_header) : 0;
    size_t ct_len = content_type ? strlen(content_type) : 0;
    size_t buf_sz = 1024 + body_len + secret_len + ct_len;
    char *request = malloc(buf_sz);
    if (!request) {
        return -1;
    }

    // build headers manually for full control
    int off = snprintf(request, buf_sz,
                       "POST /webhook HTTP/1.1\r\n"
                       "Host: 127.0.0.1:%d\r\n",
                       TEST_PORT);
    if (content_type) {
        off += snprintf(request + off, buf_sz - (size_t)off,
                        "Content-Type: %s\r\n", content_type);
    }
    off += snprintf(request + off, buf_sz - (size_t)off,
                    "Content-Length: %d\r\n", (int)body_len);
    if (secret_header) {
        off += snprintf(request + off, buf_sz - (size_t)off,
                        "%s: %s\r\n", WEBHOOK_SECRET_HEADER, secret_header);
    }
    off += snprintf(request + off, buf_sz - (size_t)off, "\r\n%s", body);

    char resp[2048];
    int status = raw_http_request("127.0.0.1", TEST_PORT, request, (size_t)off, resp, sizeof(resp));
    free(request);
    if (out_status) {
        *out_status = status;
    }
    return status;
}

// lifecycle 
static Config g_test_cfg;

static void setup_webhook(void)
{
    memset(&g_test_cfg, 0, sizeof(g_test_cfg));
    g_test_cfg.webhook_enabled = true;
    g_test_cfg.webhook_port = TEST_PORT;
    snprintf(g_test_cfg.webhook_secret, sizeof(g_test_cfg.webhook_secret), "%s", TEST_SECRET);
    snprintf(g_test_cfg.token, sizeof(g_test_cfg.token), "fake-token");
    g_test_cfg.worker_count = 1;
    g_test_cfg.user_ring_size = 8;

    queue_init(g_test_cfg.user_ring_size);
    webhook_set_update_cb(test_update_cb);

    int rc = webhook_start(&g_test_cfg, NULL);
    if (rc != 0) {
        fprintf(stderr, "FATAL: could not start webhook on port %d: %s\n", TEST_PORT,
                strerror(errno));
        exit(1);
    }
    // give MHD threads a moment to bind
    usleep(100000);
    reset_update_count();
}

static void teardown_webhook(void)
{
    webhook_stop();
    queue_shutdown();
    queue_destroy();
}

// tests

// non-telegram headers are "dropped" - requests missing the secret: 403 Forbidden
TEST(webhook_reject_missing_secret_header)
{
    setup_webhook();

    const char *body = "{\"update_id\":1}";
    int status = 0;
    send_webhook_post(body, NULL, &status);
    ASSERT_EQ(status, 403);
    ASSERT_EQ(get_update_count(), 0);

    teardown_webhook();
}

// wrong secret header value -> 403
TEST(webhook_reject_wrong_secret)
{
    setup_webhook();

    const char *body = "{\"update_id\":2}";
    int status = 0;
    send_webhook_post(body, "wrong-secret", &status);
    ASSERT_EQ(status, 403);
    ASSERT_EQ(get_update_count(), 0);

    teardown_webhook();
}

// valid secret + valid JSON -> 200, update callback fires
TEST(webhook_accept_valid_request)
{
    setup_webhook();

    const char *body = "{\"update_id\":3,\"message\":{\"text\":\"hi\"}}";
    int status = 0;
    send_webhook_post(body, TEST_SECRET, &status);
    ASSERT_EQ(status, 200);
    // allow MHD to dispatch
    usleep(50000);
    ASSERT_EQ(get_update_count(), 1);

    teardown_webhook();
}

// valid secret + invalid JSON -> 200 (accepted by MHD) but callback NOT fired
TEST(webhook_reject_invalid_json)
{
    setup_webhook();

    const char *body = "NOT VALID JSON {{{{";
    int status = 0;
    send_webhook_post(body, TEST_SECRET, &status);
    ASSERT_EQ(status, 200); // MHD returns 200; cJSON_Parse fails silently
    usleep(50000);
    ASSERT_EQ(get_update_count(), 0); // callback never invoked

    teardown_webhook();
}

// GET request -> 404
TEST(webhook_reject_get_request)
{
    setup_webhook();

    char request[512];
    int n = snprintf(request, sizeof(request),
                     "GET /webhook HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%d\r\n"
                     "\r\n",
                     TEST_PORT);

    char resp[2048];
    int status = raw_http_request("127.0.0.1", TEST_PORT, request, (size_t)n, resp, sizeof(resp));
    ASSERT_EQ(status, 404);

    teardown_webhook();
}

// POST to wrong path -> 404
TEST(webhook_reject_wrong_path)
{
    setup_webhook();

    const char *body = "{\"update_id\":6}";
    char request[1024];
    int body_len = (int)strlen(body);
    int n = snprintf(request, sizeof(request),
                     "POST /admin HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%d\r\n"
                     "Content-Type: application/json\r\n"
                     "%s: %s\r\n"
                     "Content-Length: %d\r\n"
                     "\r\n"
                     "%s",
                     TEST_PORT, WEBHOOK_SECRET_HEADER, TEST_SECRET, body_len, body);

    char resp[2048];
    int status = raw_http_request("127.0.0.1", TEST_PORT, request, (size_t)n, resp, sizeof(resp));
    ASSERT_EQ(status, 404);

    teardown_webhook();
}

// server is addressable via LAN
TEST(webhook_addressable_via_lan)
{
    setup_webhook();

    /* connecting to 127.0.0.1 proves it bound to at least localhost;
     * MHD with no bind address binds to INADDR_ANY (all interfaces)
     */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(fd >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    ASSERT_EQ(rc, 0);
    close(fd);

    teardown_webhook();
}

// extremely long header value (shouldn't crash)
TEST(webhook_malicious_long_header)
{
    setup_webhook();

    // build a request with a 4000-byte secret header value
    char long_val[4001];
    memset(long_val, 'A', 4000);
    long_val[4000] = '\0';

    const char *body = "{\"update_id\":8}";
    int status = 0;
    send_webhook_post(body, long_val, &status);
    // server should reject (403 - wrong secret) and not crash
    ASSERT(status == 403 || status == 200);

    // verify server is still alive
    int status2 = 0;
    send_webhook_post(body, TEST_SECRET, &status2);
    ASSERT_EQ(status2, 200);

    teardown_webhook();
}

// POST with empty body
TEST(webhook_malicious_empty_body)
{
    setup_webhook();

    const char *body = "";
    int status = 0;
    send_webhook_post(body, TEST_SECRET, &status);
    ASSERT_EQ(status, 200); // server returns OK
    usleep(50000);
    ASSERT_EQ(get_update_count(), 0);

    teardown_webhook();
}

// null bytes in body
TEST(webhook_malicious_null_bytes)
{
    setup_webhook();

    char request[1024];
    char body[] = "{\"upd\0ate_id\":10}";
    size_t body_len = 5; // truncated at null

    int n = snprintf(request, sizeof(request),
                     "POST /webhook HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%d\r\n"
                     "Content-Type: application/json\r\n"
                     "%s: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n",
                     TEST_PORT, WEBHOOK_SECRET_HEADER, TEST_SECRET, body_len);
    memcpy(request + n, body, body_len);

    char resp[2048];
    int status = raw_http_request("127.0.0.1", TEST_PORT, request, (size_t)n + body_len, resp, sizeof(resp));
    ASSERT(status == 200);
    usleep(50000);
    // invalid JSON -> callback not fired
    ASSERT_EQ(get_update_count(), 0);

    teardown_webhook();
}

// wrong Content-Type -> 415 Unsupported Media Type
TEST(webhook_reject_wrong_content_type)
{
    setup_webhook();

    const char *body = "{\"update_id\":11}";
    int status = 0;
    send_webhook_post_ct(body, TEST_SECRET, "text/plain", &status);
    ASSERT_EQ(status, 415);
    usleep(50000);
    ASSERT_EQ(get_update_count(), 0);

    teardown_webhook();
}

// missing Content-Type header -> 415
TEST(webhook_reject_missing_content_type)
{
    setup_webhook();

    const char *body = "{\"update_id\":12}";
    int status = 0;
    send_webhook_post_ct(body, TEST_SECRET, NULL, &status);
    ASSERT_EQ(status, 415);
    usleep(50000);
    ASSERT_EQ(get_update_count(), 0);

    teardown_webhook();
}

// Content-Type with charset variant -> 200
TEST(webhook_accept_charset_variant)
{
    setup_webhook();

    const char *body = "{\"update_id\":13,\"message\":{\"text\":\"ok\"}}";
    int status = 0;
    send_webhook_post_ct(body, TEST_SECRET, "application/json; charset=utf-8", &status);
    ASSERT_EQ(status, 200);
    usleep(50000);
    ASSERT_EQ(get_update_count(), 1);

    teardown_webhook();
}

// oversized body -> deterministically 413
TEST(webhook_reject_oversized_body)
{
    setup_webhook();
    
    /* BUG?:
     *  Performing an implicit widening conversion to type 'size_t' (aka 'unsigned long') of a multiplication performed in type
     *  'int' (fixes available)clang-tidybugprone-implicit-widening-of-multiplication-result
     */

    // build a body larger than RESPONSE_BUF_MAX (512 KiB)
    size_t big_sz = 600 * 1024;
    char *big_body = malloc(big_sz + 1);
    ASSERT_NOT_NULL(big_body);
    memset(big_body, 'A', big_sz);
    big_body[big_sz] = '\0';

    int status = 0;
    send_webhook_post_ct(big_body, TEST_SECRET, "application/json", &status);
    ASSERT_EQ(status, 413);
    free(big_body);

    // verify server survived
    const char *ok_body = "{\"update_id\":14,\"message\":{\"text\":\"alive\"}}";
    int status2 = 0;
    send_webhook_post(ok_body, TEST_SECRET, &status2);
    ASSERT_EQ(status2, 200);

    teardown_webhook();
}

// empty webhook_secret config -> requests accepted without secret header
TEST(webhook_empty_secret_allows_all)
{
    // custom setup with empty secret
    memset(&g_test_cfg, 0, sizeof(g_test_cfg));
    g_test_cfg.webhook_enabled = true;
    g_test_cfg.webhook_port = TEST_PORT;
    g_test_cfg.webhook_secret[0] = '\0'; // no secret
    snprintf(g_test_cfg.token, sizeof(g_test_cfg.token), "fake-token");
    g_test_cfg.worker_count = 1;
    g_test_cfg.user_ring_size = 8;

    queue_init(g_test_cfg.user_ring_size);
    webhook_set_update_cb(test_update_cb);

    int rc = webhook_start(&g_test_cfg, NULL);
    ASSERT_EQ(rc, 0);
    usleep(100000);
    reset_update_count();

    // send without secret header - should succeed
    const char *body = "{\"update_id\":15,\"message\":{\"text\":\"no-secret\"}}";
    int status = 0;
    send_webhook_post(body, NULL, &status);
    ASSERT_EQ(status, 200);
    usleep(50000);
    ASSERT_EQ(get_update_count(), 1);

    webhook_stop();
    queue_shutdown();
    queue_destroy();
}

// concurrent requests exceeding PB_POOL_SIZE force heap fallback
#define CONCURRENT_REQS 16

static void *flood_thread(void *arg)
{
    (void)arg;
    const char *body = "{\"update_id\":16,\"message\":{\"text\":\"flood\"}}";
    int status = 0;
    send_webhook_post(body, TEST_SECRET, &status);
    // any valid HTTP response is acceptable (server didn't crash)
    return NULL;
}

TEST(webhook_pool_exhaustion_fallback)
{
    setup_webhook();

    // fire many concurrent requests to exhaust the pool and exercise heap fallback
    pthread_t threads[CONCURRENT_REQS];

    for (int i = 0; i < CONCURRENT_REQS; i++) {
        pthread_create(&threads[i], NULL, flood_thread, NULL);
    }
    for (int i = 0; i < CONCURRENT_REQS; i++) {
        pthread_join(threads[i], NULL);
    }

    usleep(100000);

    // verify server still alive after pool exhaustion
    const char *ok_body = "{\"update_id\":17,\"message\":{\"text\":\"alive\"}}";
    int status2 = 0;
    send_webhook_post(ok_body, TEST_SECRET, &status2);
    ASSERT_EQ(status2, 200);

    teardown_webhook();
}

int main(void)
{
    printf("=== test_webhook ===\n");
    return test_summarise();
}

// truncated JSON body
TEST(webhook_adversarial_truncated_json)
{
    setup_webhook();

    const char *body = "{\"update_id\":100,\"message\":{\"text\":\"hi";
    int status = 0;
    send_webhook_post(body, TEST_SECRET, &status);
    ASSERT_EQ(status, 200); // accepted but parse fails
    usleep(50000);
    ASSERT_EQ(get_update_count(), 0);

    teardown_webhook();
}

// deeply nested JSON
TEST(webhook_adversarial_deep_nesting)
{
    setup_webhook();

    // build 200 levels of nesting: {{{...}}}
    char deep[2048];
    int off = 0;
    for (int i = 0; i < 200 && off < 1900; i++) {
        deep[off++] = '{';
        deep[off++] = '"';
        deep[off++] = 'a';
        deep[off++] = '"';
        deep[off++] = ':';
    }
    deep[off++] = '1';
    for (int i = 0; i < 200 && off < (int)sizeof(deep) - 1; i++) {
        deep[off++] = '}';
    }
    deep[off] = '\0';

    int status = 0;
    send_webhook_post(deep, TEST_SECRET, &status);
    ASSERT(status == 200); // server didn't crash
    usleep(50000);

    // verify server still alive
    const char *ok = "{\"update_id\":101,\"message\":{\"text\":\"alive\"}}";
    int s2 = 0;
    send_webhook_post(ok, TEST_SECRET, &s2);
    ASSERT_EQ(s2, 200);

    teardown_webhook();
}

// key-only JSON (no values)
TEST(webhook_adversarial_key_only_json)
{
    setup_webhook();

    const char *body = "{\"update_id\"}";
    int status = 0;
    send_webhook_post(body, TEST_SECRET, &status);
    ASSERT_EQ(status, 200);
    usleep(50000);
    ASSERT_EQ(get_update_count(), 0);

    teardown_webhook();
}

// array-only body (not an object)
TEST(webhook_adversarial_array_body)
{
    setup_webhook();

    const char *body = "[1, 2, 3]";
    int status = 0;
    send_webhook_post(body, TEST_SECRET, &status);
    ASSERT_EQ(status, 200);
    usleep(50000);
    // cJSON_Parse succeeds but update_cb may fire with array root
    // either way, no crash

    teardown_webhook();
}

// BOM prefix before JSON
TEST(webhook_adversarial_bom_prefix)
{
    setup_webhook();

    const char *body = "\xEF\xBB\xBF{\"update_id\":102}";
    int status = 0;
    send_webhook_post(body, TEST_SECRET, &status);
    ASSERT_EQ(status, 200);
    usleep(50000);

    teardown_webhook();
}

// extremely long string value
TEST(webhook_adversarial_long_string_value)
{
    setup_webhook();

    // build JSON with a 100 KB string value (under RESPONSE_BUF_MAX)
    size_t val_len = 100 * 1024;
    size_t buf_sz = val_len + 128;
    char *body = malloc(buf_sz);
    ASSERT_NOT_NULL(body);

    int prefix = snprintf(body, buf_sz, "{\"update_id\":103,\"message\":{\"text\":\"");
    memset(body + prefix, 'X', val_len);
    int suffix_off = prefix + (int)val_len;
    snprintf(body + suffix_off, buf_sz - (size_t)suffix_off, "\"}}");

    int status = 0;
    send_webhook_post(body, TEST_SECRET, &status);
    ASSERT_EQ(status, 200);
    free(body);

    // verify server survived
    usleep(50000);
    const char *ok = "{\"update_id\":104}";
    int s2 = 0;
    send_webhook_post(ok, TEST_SECRET, &s2);
    ASSERT_EQ(s2, 200);

    teardown_webhook();
}

// binary garbage as POST body
TEST(webhook_adversarial_binary_garbage)
{
    setup_webhook();

    char garbage[256];
    for (int i = 0; i < 256; i++) {
        garbage[i] = (char)(i & 0xFF);
    }
    // need NUL-terminated for send_webhook_post, so use raw request
    char request[2048];
    int n = snprintf(request, sizeof(request),
                     "POST /webhook HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%d\r\n"
                     "Content-Type: application/json\r\n"
                     "%s: %s\r\n"
                     "Content-Length: 256\r\n"
                     "\r\n",
                     TEST_PORT, WEBHOOK_SECRET_HEADER, TEST_SECRET);
    memcpy(request + n, garbage, 256);

    char resp[2048];
    int status = raw_http_request("127.0.0.1", TEST_PORT, request, (size_t)n + 256, resp, sizeof(resp));
    ASSERT(status == 200);
    usleep(50000);
    ASSERT_EQ(get_update_count(), 0);

    teardown_webhook();
}

// extremely long header value (>8 KB)
TEST(webhook_adversarial_8k_header)
{
    setup_webhook();

    // 9000-byte secret header value
    size_t hdr_len = 9000;
    char *long_val = malloc(hdr_len + 1);
    ASSERT_NOT_NULL(long_val);
    memset(long_val, 'B', hdr_len);
    long_val[hdr_len] = '\0';

    const char *body = "{\"update_id\":105}";
    int status = 0;
    send_webhook_post(body, long_val, &status);
    // should be rejected (wrong secret) and not crash
    ASSERT(status == 403 || status > 0);
    free(long_val);

    // verify alive
    int s2 = 0;
    send_webhook_post("{\"update_id\":106}", TEST_SECRET, &s2);
    ASSERT_EQ(s2, 200);

    teardown_webhook();
}
