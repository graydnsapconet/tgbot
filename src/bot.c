#define _DEFAULT_SOURCE

#include "bot.h"
#include "cJSON.h"
#include "config.h"
#include "logger.h"

#include <curl/curl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct BotHandle {
    CURL *curl;
    char token[256];
    struct curl_slist *json_hdrs;
    char url_prefix[API_URL_MAX];
    size_t url_prefix_len;
    volatile sig_atomic_t *abort_flag;
    int allow_http;
};

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

typedef struct {
    const char *url;
    const char *post_body;
    struct curl_slist *headers;
    long timeout;
    int parse_retry_after;
    int retry_once_on_429;
    const char *curl_error_msg;
    const char *curl_retry_error_msg;
    const char *json_error_msg;
    const char *api_error_msg;
} ApiRequestSpec;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    Buffer *buf = (Buffer *)userdata;

    // guard against size_t overflow on 32bit targets
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        return 0;
    }
    size_t bytes = size * nmemb;

    if (buf->len + bytes + 1 > RESPONSE_BUF_MAX) {
        log_error("bot: response too large (>%d bytes)", RESPONSE_BUF_MAX);
        return 0; // signal error to curl
    }

    if (buf->len + bytes + 1 > buf->cap) {
        size_t new_cap = (buf->cap == 0) ? 4096 : buf->cap * 2;
        while (new_cap < buf->len + bytes + 1) {
            if (new_cap > SIZE_MAX / 2) {
                return 0; // capacity would overflow
            }
            new_cap *= 2;
        }
        char *tmp = realloc(buf->data, new_cap);
        if (!tmp) {
            return 0;
        }
        buf->data = tmp;
        buf->cap = new_cap;
    }

    memcpy(buf->data + buf->len, ptr, bytes);
    buf->len += bytes;
    buf->data[buf->len] = '\0';
    return bytes;
}

// build URL by appending method to pre-built prefix
static int build_url(const BotHandle *bot, const char *method, char *out, size_t out_sz)
{
    size_t mlen = strlen(method);
    size_t need = bot->url_prefix_len + mlen + 1;
    if (need > out_sz) {
        log_error("bot: URL truncated (need %zu, have %zu)", need, out_sz);
        return -1;
    }
    memcpy(out, bot->url_prefix, bot->url_prefix_len);
    memcpy(out + bot->url_prefix_len, method, mlen + 1); // +1 for NULL
    return 0;
}

// apply mandatory TLS settings to a reset CURL handle
static void curl_set_tls(CURL *c)
{
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
}

// variant that allows http if bot->allow_http is set
static void curl_set_tls_bot(BotHandle *bot)
{
    curl_set_tls(bot->curl);
    if (bot->allow_http) {
        curl_easy_setopt(bot->curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(bot->curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(bot->curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
}

// progress callback: aborts transfer when abort_flag goes to 0
static int xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                       curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    BotHandle *bot = (BotHandle *)clientp;
    if (bot->abort_flag && *bot->abort_flag == 0) {
        return 1; // abort transfer
    }
    return 0;
}

// install the abort-aware progress callback on the curl handle
static void curl_set_abort(BotHandle *bot)
{
    if (bot->abort_flag) {
        curl_easy_setopt(bot->curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(bot->curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
        curl_easy_setopt(bot->curl, CURLOPT_XFERINFODATA, bot);
    }
}

// header callback to capture Retry-After value
static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    long *retry_after = (long *)userdata;
    size_t total = size * nitems;
    if (total > 13 && strncasecmp(buffer, "Retry-After:", 12) == 0) {
        long val = strtol(buffer + 12, NULL, 10);
        if (val > 0 && val <= 60) {
            *retry_after = val;
        }
    }
    return total;
}

static void curl_prepare_request(BotHandle *bot, const ApiRequestSpec *spec, Buffer *buf, long *retry_after)
{
    curl_easy_reset(bot->curl);
    curl_set_tls_bot(bot);
    curl_set_abort(bot);

    curl_easy_setopt(bot->curl, CURLOPT_URL, spec->url);
    curl_easy_setopt(bot->curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(bot->curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(bot->curl, CURLOPT_TIMEOUT, spec->timeout);

    if (spec->headers) {
        curl_easy_setopt(bot->curl, CURLOPT_HTTPHEADER, spec->headers);
    }

    if (spec->post_body) {
        curl_easy_setopt(bot->curl, CURLOPT_POSTFIELDS, spec->post_body);
    }

    if (spec->parse_retry_after && retry_after) {
        curl_easy_setopt(bot->curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(bot->curl, CURLOPT_HEADERDATA, retry_after);
    }
}

static cJSON *api_perform_json(BotHandle *bot, const ApiRequestSpec *spec)
{
    Buffer buf = {0};
    long retry_after = 1;

    curl_prepare_request(bot, spec, &buf, &retry_after);

    CURLcode rc = curl_easy_perform(bot->curl);
    if (rc != CURLE_OK) {
        log_error("bot: %s: %s", spec->curl_error_msg, curl_easy_strerror(rc));
        free(buf.data);
        return NULL;
    }

    long http_code = 0;
    curl_easy_getinfo(bot->curl, CURLINFO_RESPONSE_CODE, &http_code);

    // handle 429 Too Many Requests with retry
    if (spec->retry_once_on_429 && http_code == 429) {
        log_warn("bot: rate-limited (429), retrying after %lds", retry_after);
        free(buf.data);
        sleep((unsigned int)retry_after);

        // retry once
        buf = (Buffer){0};
        retry_after = 1;
        curl_prepare_request(bot, spec, &buf, &retry_after);
        rc = curl_easy_perform(bot->curl);
        if (rc != CURLE_OK) {
            log_error("bot: %s: %s", spec->curl_retry_error_msg, curl_easy_strerror(rc));
            free(buf.data);
            return NULL;
        }
    }

    cJSON *root = cJSON_Parse(buf.data);
    free(buf.data);

    if (!root) {
        log_error("bot: %s", spec->json_error_msg);
        return NULL;
    }

    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        cJSON *desc = cJSON_GetObjectItemCaseSensitive(root, "description");
        log_error("bot: %s: %s", spec->api_error_msg,
                cJSON_IsString(desc) ? desc->valuestring : "(unknown)");
        cJSON_Delete(root);
        return NULL;
    }

    return root;
}

static cJSON *api_get(BotHandle *bot, const char *url)
{
    ApiRequestSpec spec = {
        .url = url,
        .post_body = NULL,
        .headers = NULL,
        .timeout = 60L,
        .parse_retry_after = 1,
        .retry_once_on_429 = 1,
        .curl_error_msg = "curl GET failed",
        .curl_retry_error_msg = "curl GET retry failed",
        .json_error_msg = "JSON parse failed",
        .api_error_msg = "API error",
    };

    return api_perform_json(bot, &spec);
}

static cJSON *api_post_json(BotHandle *bot, const char *url, const char *json_body)
{
    ApiRequestSpec spec = {
        .url = url,
        .post_body = json_body,
        .headers = bot->json_hdrs,
        .timeout = 60L,
        .parse_retry_after = 1,
        .retry_once_on_429 = 1,
        .curl_error_msg = "curl POST failed",
        .curl_retry_error_msg = "curl POST retry failed",
        .json_error_msg = "JSON parse failed",
        .api_error_msg = "API error",
    };

    return api_perform_json(bot, &spec);
}

BotHandle *bot_init(const char *token)
{
    if (!token || token[0] == '\0') {
        log_error("bot: empty token");
        return NULL;
    }

    BotHandle *bot = calloc(1, sizeof(*bot));
    if (!bot) {
        return NULL;
    }

    snprintf(bot->token, sizeof(bot->token), "%s", token);

    // pre-build URL prefix: "https://api.telegram.org/bot<token>/"
    int plen = snprintf(bot->url_prefix, sizeof(bot->url_prefix), "%s%s/", API_BASE, bot->token);
    if (plen < 0 || (size_t)plen >= sizeof(bot->url_prefix)) {
        free(bot);
        return NULL;
    }
    bot->url_prefix_len = (size_t)plen;

    bot->curl = curl_easy_init();
    if (!bot->curl) {
        free(bot);
        return NULL;
    }

    bot->json_hdrs = curl_slist_append(NULL, "Content-Type: application/json");

    return bot;
}

BotHandle *bot_init_send_only(const char *token)
{
    return bot_init(token);
}

void bot_set_abort_flag(BotHandle *bot, volatile sig_atomic_t *flag)
{
    if (bot) {
        bot->abort_flag = flag;
    }
}

void bot_cleanup(BotHandle *bot)
{
    if (!bot) {
        return;
    }
    if (bot->curl) {
        curl_easy_cleanup(bot->curl);
    }
    if (bot->json_hdrs) {
        curl_slist_free_all(bot->json_hdrs);
    }
    // scrub the token from memory before freeing
    explicit_bzero(bot->token, sizeof(bot->token));
    explicit_bzero(bot->url_prefix, sizeof(bot->url_prefix));
    bot->url_prefix_len = 0;
    free(bot);
}

cJSON *bot_get_me(BotHandle *bot)
{
    char url[API_URL_MAX];
    if (build_url(bot, "getMe", url, sizeof(url)) != 0) {
        return NULL;
    }
    return api_get(bot, url);
}

cJSON *bot_get_updates(BotHandle *bot, int64_t offset, int timeout, int limit)
{
    char url[API_URL_MAX];
    int url_len = snprintf(url, sizeof(url),
                           "%sgetUpdates?offset=%" PRId64
                           "&limit=%d&timeout=%d&allowed_updates=[\"message\"]",
                           bot->url_prefix, offset, limit, timeout);
    if (url_len < 0 || (size_t)url_len >= sizeof(url)) {
        log_error("bot: getUpdates URL truncated");
        return NULL;
    }

    ApiRequestSpec spec = {
        .url = url,
        .post_body = NULL,
        .headers = NULL,
        .timeout = (long)(timeout + 10),
        .parse_retry_after = 0,
        .retry_once_on_429 = 0,
        .curl_error_msg = "getUpdates curl error",
        .curl_retry_error_msg = "getUpdates curl retry error",
        .json_error_msg = "getUpdates JSON parse failed",
        .api_error_msg = "getUpdates error",
    };

    return api_perform_json(bot, &spec);
}

int bot_send_message(BotHandle *bot, int64_t chat_id, const char *text)
{
    char url[API_URL_MAX];
    if (build_url(bot, "sendMessage", url, sizeof(url)) != 0) {
        return -1;
    }

    // build JSON with cJSON to get correct string escaping
    cJSON *body = cJSON_CreateObject();
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%" PRId64, chat_id);
    cJSON_AddRawToObject(body, "chat_id", id_str);
    cJSON_AddStringToObject(body, "text", text);

    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) {
        return -1;
    }

    cJSON *resp = api_post_json(bot, url, json_str);
    cJSON_free(json_str);
    if (!resp) {
        return -1;
    }
    cJSON_Delete(resp);
    return 0;
}

int bot_set_webhook(BotHandle *bot, const char *url, const char *secret)
{
    char api_url[API_URL_MAX];
    if (build_url(bot, "setWebhook", api_url, sizeof(api_url)) != 0) {
        return -1;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "url", url);
    if (secret && secret[0] != '\0') {
        cJSON_AddStringToObject(body, "secret_token", secret);
    }
    cJSON *allowed = cJSON_AddArrayToObject(body, "allowed_updates");
    cJSON_AddItemToArray(allowed, cJSON_CreateString("message"));

    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) {
        return -1;
    }

    cJSON *resp = api_post_json(bot, api_url, json_str);
    cJSON_free(json_str);
    if (!resp) {
        return -1;
    }
    cJSON_Delete(resp);
    log_info("bot: webhook set to %s", url);
    return 0;
}

int bot_delete_webhook(BotHandle *bot)
{
    char api_url[API_URL_MAX];
    if (build_url(bot, "deleteWebhook", api_url, sizeof(api_url)) != 0) {
        return -1;
    }

    cJSON *resp = api_post_json(bot, api_url, "{}");
    if (!resp) {
        return -1;
    }
    cJSON_Delete(resp);
    log_info("bot: webhook deleted");
    return 0;
}

void bot_set_api_base(BotHandle *bot, const char *base_url)
{
    if (!bot || !base_url) {
        return;
    }
    int plen = snprintf(bot->url_prefix, sizeof(bot->url_prefix), "%s%s/", base_url, bot->token);
    if (plen < 0 || (size_t)plen >= sizeof(bot->url_prefix)) {
        log_error("bot: API base URL too long");
        return;
    }
    bot->url_prefix_len = (size_t)plen;
}

#ifdef TESTING
void bot_set_allow_http(BotHandle *bot, int allow)
{
    if (bot) {
        bot->allow_http = allow;
    }
}
#endif
