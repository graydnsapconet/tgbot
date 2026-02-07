#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "../src/logger.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MARKER "---^-OVERWRITE-^---\n"
#define MARKER_LEN 20

static char g_tmp_path[256];
static int g_tmp_counter = 0;

static const char *tmp_log_path(void)
{
    snprintf(g_tmp_path, sizeof(g_tmp_path), "/tmp/tgbot_test_log_%d_%d.log", (int)getpid(),
             g_tmp_counter++);
    return g_tmp_path;
}

static void cleanup_file(const char *path)
{
    unlink(path);
}

// read entire file into malloc'd buffer - caller frees
static char *read_file_contents(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(fp);
        *out_len = 0;
        return calloc(1, 1);
    }
    char *buf = malloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, fp);
    buf[got] = '\0';
    fclose(fp);
    *out_len = got;
    return buf;
}

// needle in haystack
static int count_substr(const char *haystack, const char *needle)
{
    int count = 0;
    size_t nlen = strlen(needle);
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += nlen;
    }
    return count;
}

// tests
TEST(basic_write_and_read)
{
    const char *path = tmp_log_path();
    ASSERT_EQ(log_init(path, 4096), 0);
    log_set_level(LOG_DEBUG);

    log_info("hello world");
    log_debug("debug msg");
    log_error("error msg");
    log_close();

    // has content?
    size_t len;
    char *buf = read_file_contents(path, &len);
    ASSERT_NOT_NULL(buf);
    ASSERT(len > 0);
    ASSERT(strstr(buf, "hello world") != NULL);
    ASSERT(strstr(buf, "debug msg") != NULL);
    ASSERT(strstr(buf, "error msg") != NULL);
    free(buf);
    cleanup_file(path);
}

TEST(level_filtering)
{
    const char *path = tmp_log_path();
    ASSERT_EQ(log_init(path, 4096), 0);
    log_set_level(LOG_WARN);

    log_debug("should not appear");
    log_info("should not appear either");
    log_warn("warning visible");
    log_error("error visible");
    log_close();

    size_t len;
    char *buf = read_file_contents(path, &len);
    ASSERT_NOT_NULL(buf);
    ASSERT(strstr(buf, "should not appear") == NULL);
    ASSERT(strstr(buf, "warning visible") != NULL);
    ASSERT(strstr(buf, "error visible") != NULL);
    free(buf);
    cleanup_file(path);
}

TEST(circular_wrap_file_stays_under_cap)
{
    const char *path = tmp_log_path();
    size_t cap = 2048;
    ASSERT_EQ(log_init(path, cap), 0);
    log_set_level(LOG_DEBUG);

    // exceed cap several times
    for (int i = 0; i < 200; i++) {
        log_info("line %04d padding padding padding padding padding", i);
    }
    log_close();

    size_t len;
    char *buf = read_file_contents(path, &len);
    ASSERT_NOT_NULL(buf);
    ASSERT(len <= cap);
    free(buf);
    cleanup_file(path);
}

TEST(overwrite_marker_present_after_wrap)
{
    const char *path = tmp_log_path();
    size_t cap = 2048;
    ASSERT_EQ(log_init(path, cap), 0);
    log_set_level(LOG_DEBUG);

    // write enough to wrap
    for (int i = 0; i < 200; i++) {
        log_info("line %04d padding padding padding padding padding", i);
    }
    log_close();

    size_t len;
    char *buf = read_file_contents(path, &len);
    ASSERT_NOT_NULL(buf);
    ASSERT(count_substr(buf, MARKER) == 1);
    free(buf);
    cleanup_file(path);
}

TEST(recovery_after_reopen)
{
    const char *path = tmp_log_path();
    size_t cap = 2048;
    ASSERT_EQ(log_init(path, cap), 0);
    log_set_level(LOG_DEBUG);

    // write enough to wrap
    for (int i = 0; i < 200; i++) {
        log_info("line %04d padding padding padding padding padding", i);
    }
    log_close();

    // reopen: should recover write_pos from marker
    ASSERT_EQ(log_init(path, cap), 0);
    log_set_level(LOG_DEBUG);

    log_info("after reopen A");
    log_info("after reopen B");
    log_close();

    size_t len;
    char *buf = read_file_contents(path, &len);
    ASSERT_NOT_NULL(buf);
    ASSERT(len <= cap);
    ASSERT(strstr(buf, "after reopen A") != NULL);
    ASSERT(strstr(buf, "after reopen B") != NULL);
    // still exactly one marker
    ASSERT(count_substr(buf, MARKER) == 1);
    free(buf);
    cleanup_file(path);
}

TEST(multiple_wrap_cycles_no_drift)
{
    const char *path = tmp_log_path();
    size_t cap = 2048;

    for (int cycle = 0; cycle < 5; cycle++) {
        ASSERT_EQ(log_init(path, cap), 0);
        log_set_level(LOG_DEBUG);
        for (int i = 0; i < 100; i++) {
            log_info("cycle %d line %04d pad pad pad pad pad pad", cycle, i);
        }
        log_close();

        size_t len;
        char *buf = read_file_contents(path, &len);
        ASSERT_NOT_NULL(buf);
        ASSERT(len <= cap);
        ASSERT(count_substr(buf, MARKER) == 1);
        free(buf);
    }
    cleanup_file(path);
}

TEST(read_last_n_after_wrap)
{
    const char *path = tmp_log_path();
    size_t cap = 2048;
    ASSERT_EQ(log_init(path, cap), 0);
    log_set_level(LOG_DEBUG);

    for (int i = 0; i < 200; i++) {
        log_info("seq %04d pad pad pad pad pad pad pad", i);
    }
    log_close();

    // log_read_last_n prints to stdout - verify it doesn't crash
    ASSERT_EQ(log_read_last_n(path, 5), 0);
    cleanup_file(path);
}

TEST(no_wrap_small_file)
{
    const char *path = tmp_log_path();
    ASSERT_EQ(log_init(path, 65536), 0);
    log_set_level(LOG_DEBUG);

    log_info("one");
    log_info("two");
    log_info("three");
    log_close();

    size_t len;
    char *buf = read_file_contents(path, &len);
    ASSERT_NOT_NULL(buf);
    // no marker when file hasn't wrapped
    ASSERT(count_substr(buf, MARKER) == 0);
    ASSERT(strstr(buf, "one") != NULL);
    ASSERT(strstr(buf, "two") != NULL);
    ASSERT(strstr(buf, "three") != NULL);
    free(buf);
    cleanup_file(path);
}

TEST(close_without_init_is_safe)
{
    log_close(); // shouldn't crash
    ASSERT(1);
}

TEST(write_without_init_goes_to_stderr)
{
    // shouldn't crash - just stderr
    log_error("no init yet");
    ASSERT(1);
}

// thread safety
#define THREAD_COUNT 4
#define WRITES_PER_THREAD 500

static void *writer_thread(void *arg)
{
    int id = *(int *)arg;
    for (int i = 0; i < WRITES_PER_THREAD; i++) {
        log_info("thread %d iter %04d padding padding padding", id, i);
    }
    return NULL;
}

TEST(concurrent_writes)
{
    const char *path = tmp_log_path();
    size_t cap = 8192;
    ASSERT_EQ(log_init(path, cap), 0);
    log_set_level(LOG_DEBUG);

    pthread_t threads[THREAD_COUNT];
    int ids[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, writer_thread, &ids[i]);
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    log_close();

    size_t len;
    char *buf = read_file_contents(path, &len);
    ASSERT_NOT_NULL(buf);
    ASSERT(len <= cap);
    // after all those writes, file should have wrapped with marker present
    ASSERT(count_substr(buf, MARKER) == 1);
    free(buf);
    cleanup_file(path);
}

TEST(line_format_has_timestamp_and_level)
{
    const char *path = tmp_log_path();
    ASSERT_EQ(log_init(path, 4096), 0);
    log_set_level(LOG_DEBUG);

    log_warn("specific_marker_text");
    log_close();

    size_t len;
    char *buf = read_file_contents(path, &len);
    ASSERT_NOT_NULL(buf);
    // expect format: [YYYY-MM-DD HH:MM:SS] [WARN ] specific_marker_text
    ASSERT(strstr(buf, "[WARN ] specific_marker_text") != NULL);
    ASSERT(buf[0] == '['); // starts with timestamp bracket
    free(buf);
    cleanup_file(path);
}

TEST(read_last_n_no_wrap)
{
    const char *path = tmp_log_path();
    ASSERT_EQ(log_init(path, 65536), 0);
    log_set_level(LOG_DEBUG);

    for (int i = 0; i < 10; i++) {
        log_info("line %d", i);
    }
    log_close();

    // should not crash and returns 0
    ASSERT_EQ(log_read_last_n(path, 3), 0);
    cleanup_file(path);
}

TEST(init_rejects_tiny_cap)
{
    const char *path = tmp_log_path();
    // cap smaller than MIN_FILE_CAP (256) should be rejected
    ASSERT_EQ(log_init(path, 100), -1);
    cleanup_file(path);
}

int main(void)
{
    return test_summarise();
}
