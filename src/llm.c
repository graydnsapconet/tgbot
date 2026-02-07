#define _POSIX_C_SOURCE 200809L

#include "llm.h"
#include "cJSON.h"
#include "logger.h"

#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// maximum response body from LM Studio (256 KiB)
#define LLM_RESPONSE_MAX (256 * 1024)

struct LlmHandle {
    CURL *curl;
    struct curl_slist *headers;
    char url[512];
    char model[128];
    volatile sig_atomic_t *abort_flag;
};

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} LlmBuffer;

static size_t llm_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    LlmBuffer *buf = (LlmBuffer *)userdata;
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        return 0;
    }
    size_t bytes = size * nmemb;

    if (buf->len + bytes + 1 > LLM_RESPONSE_MAX) {
        return 0;
    }

    if (buf->len + bytes + 1 > buf->cap) {
        size_t new_cap = (buf->cap == 0) ? 4096 : buf->cap * 2;
        while (new_cap < buf->len + bytes + 1) {
            if (new_cap > SIZE_MAX / 2) {
                return 0;
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

static int llm_abort_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    volatile sig_atomic_t *flag = (volatile sig_atomic_t *)clientp;
    if (flag && !*flag) {
        return 1; // abort
    }
    return 0;
}

LlmHandle *llm_init(const char *endpoint, const char *model)
{
    if (!endpoint) {
        return NULL;
    }

    LlmHandle *llm = calloc(1, sizeof(*llm));
    if (!llm) {
        return NULL;
    }

    llm->curl = curl_easy_init();
    if (!llm->curl) {
        free(llm);
        return NULL;
    }

    snprintf(llm->url, sizeof(llm->url), "%s/v1/chat/completions", endpoint);

    if (model && model[0]) {
        snprintf(llm->model, sizeof(llm->model), "%s", model);
    }

    llm->headers = curl_slist_append(NULL, "Content-Type: application/json");
    if (!llm->headers) {
        curl_easy_cleanup(llm->curl);
        free(llm);
        return NULL;
    }

    return llm;
}

void llm_cleanup(LlmHandle *llm)
{
    if (!llm) {
        return;
    }
    if (llm->headers) {
        curl_slist_free_all(llm->headers);
    }
    if (llm->curl) {
        curl_easy_cleanup(llm->curl);
    }
    free(llm);
}

void llm_set_abort_flag(LlmHandle *llm, volatile sig_atomic_t *flag)
{
    if (llm) {
        llm->abort_flag = flag;
    }
}

// strip all <think>...</think> blocks (and self-closing <think/>) from text in-place.
// handles nested occurrences, missing close tags, and leading/trailing whitespace.
size_t llm_strip_think_tags(char *text)
{
    if (!text) {
        return 0;
    }

    char *dst = text;
    const char *src = text;
    size_t len = strlen(text);
    const char *end = text + len;

    while (src < end) {
        // look for <think at current position (case-insensitive)
        if (*src == '<' && (size_t)(end - src) >= 7 &&
            strncasecmp(src, "<think", 6) == 0) {

            const char *after_tag = src + 6;

            // self-closing: <think/> or <think />
            if (after_tag < end && *after_tag == '/') {
                after_tag++;
                if (after_tag < end && *after_tag == '>') {
                    src = after_tag + 1;
                    continue;
                }
            }
            if (after_tag < end && *after_tag == ' ' && (after_tag + 1) < end &&
                *(after_tag + 1) == '/' && (after_tag + 2) < end &&
                *(after_tag + 2) == '>') {
                src = after_tag + 3;
                continue;
            }

            // opening <think> - find matching </think>
            if (after_tag < end && *after_tag == '>') {
                const char *close = after_tag + 1;
                // search for </think>
                while (close < end) {
                    if (*close == '<' && (size_t)(end - close) >= 8 &&
                        strncasecmp(close, "</think>", 8) == 0) {
                        src = close + 8;
                        break;
                    }
                    close++;
                }
                if (close >= end) {
                    // no closing tag found - strip rest of string
                    src = end;
                }
                continue;
            }
        }

        *dst++ = *src++;
    }
    *dst = '\0';

    // trim leading whitespace
    char *start = text;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
        start++;
    }
    if (start != text) {
        size_t final_len = (size_t)(dst - start);
        memmove(text, start, final_len + 1);
        dst = text + final_len;
    }

    // trim trailing whitespace
    while (dst > text && (*(dst - 1) == ' ' || *(dst - 1) == '\t' ||
                          *(dst - 1) == '\n' || *(dst - 1) == '\r')) {
        dst--;
    }
    *dst = '\0';

    return (size_t)(dst - text);
}

int llm_chat(LlmHandle *llm, const char *system_prompt,
             const char *user_msg, char *out_buf, size_t out_cap,
             int max_tokens)
{
    if (!llm || !user_msg || !out_buf || out_cap == 0) {
        return -1;
    }

    // build the JSON request body
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        snprintf(out_buf, out_cap, "[llm error: alloc failed]");
        return -1;
    }

    if (llm->model[0]) {
        cJSON_AddStringToObject(root, "model", llm->model);
    }

    cJSON_AddNumberToObject(root, "max_tokens", max_tokens);
    cJSON_AddNumberToObject(root, "temperature", 0.7);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    if (!messages) {
        cJSON_Delete(root);
        snprintf(out_buf, out_cap, "[llm error: alloc failed]");
        return -1;
    }

    // optional system prompt
    if (system_prompt && system_prompt[0]) {
        cJSON *sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", system_prompt);
        cJSON_AddItemToArray(messages, sys_msg);
    }

    // user message
    cJSON *usr_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(usr_msg, "role", "user");
    cJSON_AddStringToObject(usr_msg, "content", user_msg);
    cJSON_AddItemToArray(messages, usr_msg);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        snprintf(out_buf, out_cap, "[llm error: JSON print failed]");
        return -1;
    }

    // set up the curl request
    LlmBuffer resp = {0};

    curl_easy_reset(llm->curl);
    curl_easy_setopt(llm->curl, CURLOPT_URL, llm->url);
    curl_easy_setopt(llm->curl, CURLOPT_HTTPHEADER, llm->headers);
    curl_easy_setopt(llm->curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(llm->curl, CURLOPT_WRITEFUNCTION, llm_write_cb);
    curl_easy_setopt(llm->curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(llm->curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(llm->curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(llm->curl, CURLOPT_NOSIGNAL, 1L);

    if (llm->abort_flag) {
        curl_easy_setopt(llm->curl, CURLOPT_XFERINFOFUNCTION, llm_abort_cb);
        curl_easy_setopt(llm->curl, CURLOPT_XFERINFODATA, (void *)llm->abort_flag);
        curl_easy_setopt(llm->curl, CURLOPT_NOPROGRESS, 0L);
    }

    CURLcode res = curl_easy_perform(llm->curl);
    free(body);

    if (res != CURLE_OK) {
        log_error("llm: curl error: %s", curl_easy_strerror(res));
        free(resp.data);
        snprintf(out_buf, out_cap, "[llm error: request failed]");
        return -1;
    }

    if (!resp.data) {
        snprintf(out_buf, out_cap, "[llm error: empty response]");
        return -1;
    }

    // parse the response
    cJSON *json = cJSON_Parse(resp.data);
    free(resp.data);
    if (!json) {
        log_error("llm: failed to parse response JSON");
        snprintf(out_buf, out_cap, "[llm error: bad response]");
        return -1;
    }

    // extract choices[0].message.content
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(json, "choices");
    const cJSON *first = cJSON_GetArrayItem(choices, 0);
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(first, "message");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");

    if (!cJSON_IsString(content) || !content->valuestring[0]) {
        log_error("llm: no content in response");
        cJSON_Delete(json);
        snprintf(out_buf, out_cap, "[llm error: no content]");
        return -1;
    }

    snprintf(out_buf, out_cap, "%s", content->valuestring);
    cJSON_Delete(json);

    // strip <think>...</think> reasoning blocks from the response
    size_t stripped_len = llm_strip_think_tags(out_buf);
    if (stripped_len == 0) {
        snprintf(out_buf, out_cap, "[llm error: empty after stripping think tags]");
        return -1;
    }

    return 0;
}
