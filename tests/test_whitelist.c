#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "test.h"
#include "../src/whitelist.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// setup
static const char *TEST_WL_FILE = "/tmp/tgbot_test_wl.txt";

static void create_test_file(const char *content)
{
    FILE *f = fopen(TEST_WL_FILE, "w");
    if (f) {
        if (content) {
            fputs(content, f);
        }
        fclose(f);
    }
}

static void cleanup_test_file(void)
{
    unlink(TEST_WL_FILE);
    char tmp[280];
    snprintf(tmp, sizeof(tmp), "%s.tmp", TEST_WL_FILE);
    unlink(tmp);
}



// tests single
TEST(whitelist_load_empty)
{
    create_test_file("");
    Whitelist wl;
    ASSERT_EQ(whitelist_load(&wl, TEST_WL_FILE), 0);
    ASSERT_EQ(wl.count, 0);
    whitelist_cleanup(&wl);
    cleanup_test_file();
}

TEST(whitelist_load_multiple)
{
    create_test_file("100\n200\n300\n");
    Whitelist wl;
    ASSERT_EQ(whitelist_load(&wl, TEST_WL_FILE), 0);
    ASSERT_EQ(wl.count, 3);
    ASSERT(whitelist_contains(&wl, 100));
    ASSERT(whitelist_contains(&wl, 200));
    ASSERT(whitelist_contains(&wl, 300));
    ASSERT(!whitelist_contains(&wl, 999));
    whitelist_cleanup(&wl);
    cleanup_test_file();
}

TEST(whitelist_add_and_contains)
{
    create_test_file("");
    Whitelist wl;
    ASSERT_EQ(whitelist_load(&wl, TEST_WL_FILE), 0);
    ASSERT_EQ(whitelist_add(&wl, 42), 0);
    ASSERT(whitelist_contains(&wl, 42));
    ASSERT_EQ(wl.count, 1);

    // adding again returns 1 (already present)
    ASSERT_EQ(whitelist_add(&wl, 42), 1);
    ASSERT_EQ(wl.count, 1);
    whitelist_cleanup(&wl);
    cleanup_test_file();
}

TEST(whitelist_remove)
{
    create_test_file("10\n20\n30\n");
    Whitelist wl;
    ASSERT_EQ(whitelist_load(&wl, TEST_WL_FILE), 0);
    ASSERT_EQ(wl.count, 3);

    ASSERT_EQ(whitelist_remove(&wl, 20), 0);
    ASSERT_EQ(wl.count, 2);
    ASSERT(!whitelist_contains(&wl, 20));
    ASSERT(whitelist_contains(&wl, 10));
    ASSERT(whitelist_contains(&wl, 30));

    // removing non-existent returns 1
    ASSERT_EQ(whitelist_remove(&wl, 999), 1);
    whitelist_cleanup(&wl);
    cleanup_test_file();
}

TEST(whitelist_persistence)
{
    create_test_file("");
    Whitelist wl;
    ASSERT_EQ(whitelist_load(&wl, TEST_WL_FILE), 0);
    ASSERT_EQ(whitelist_add(&wl, 111), 0);
    ASSERT_EQ(whitelist_add(&wl, 222), 0);

    // reload from disk
    Whitelist wl2;
    ASSERT_EQ(whitelist_load(&wl2, TEST_WL_FILE), 0);
    ASSERT_EQ(wl2.count, 2);
    ASSERT(whitelist_contains(&wl2, 111));
    ASSERT(whitelist_contains(&wl2, 222));
    whitelist_cleanup(&wl);
    whitelist_cleanup(&wl2);
    cleanup_test_file();
}

TEST(whitelist_sorted_after_load)
{
    create_test_file("300\n100\n200\n");
    Whitelist wl;
    ASSERT_EQ(whitelist_load(&wl, TEST_WL_FILE), 0);
    // verify sorted order
    ASSERT(wl.ids[0] <= wl.ids[1]);
    ASSERT(wl.ids[1] <= wl.ids[2]);
    whitelist_cleanup(&wl);
    cleanup_test_file();
}

TEST(whitelist_create_missing_file)
{
    cleanup_test_file(); // ensure it doesn't exist
    Whitelist wl;
    ASSERT_EQ(whitelist_load(&wl, TEST_WL_FILE), 0);
    ASSERT_EQ(wl.count, 0);
    whitelist_cleanup(&wl);
    cleanup_test_file();
}



// tests concurrent
#define WL_NTHREADS 4
#define WL_OPS_PER_THREAD 200
static void *wl_add_remove_thread(void *arg)
{
    Whitelist *wl = (Whitelist *)arg;
    for (int i = 0; i < WL_OPS_PER_THREAD; i++) {
        int64_t id = (int64_t)(i % 50) + 1000;
        whitelist_add(wl, id);
        whitelist_contains(wl, id);
        whitelist_remove(wl, id);
    }
    return NULL;
}

TEST(whitelist_concurrent_add_remove)
{
    create_test_file("");
    Whitelist wl;
    ASSERT_EQ(whitelist_load(&wl, TEST_WL_FILE), 0);

    pthread_t threads[WL_NTHREADS];
    for (int i = 0; i < WL_NTHREADS; i++) {
        pthread_create(&threads[i], NULL, wl_add_remove_thread, &wl);
    }
    for (int i = 0; i < WL_NTHREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // no crash, no ASan race report - that's the assertion
    ASSERT(wl.count >= 0);
    ASSERT(wl.count <= WHITELIST_MAX);
    whitelist_cleanup(&wl);
    cleanup_test_file();
}

static void *wl_reader_thread(void *arg)
{
    Whitelist *wl = (Whitelist *)arg;
    for (int i = 0; i < WL_OPS_PER_THREAD * 5; i++) {
        whitelist_contains(wl, (int64_t)(i % 100) + 1000);
    }
    return NULL;
}

static void *wl_writer_thread(void *arg)
{
    Whitelist *wl = (Whitelist *)arg;
    for (int i = 0; i < WL_OPS_PER_THREAD; i++) {
        int64_t id = (int64_t)(i % 50) + 2000;
        whitelist_add(wl, id);
        whitelist_remove(wl, id);
    }
    return NULL;
}

TEST(whitelist_concurrent_read_write)
{
    create_test_file("");
    Whitelist wl;
    ASSERT_EQ(whitelist_load(&wl, TEST_WL_FILE), 0);

    pthread_t readers[3];
    pthread_t writer;

    pthread_create(&writer, NULL, wl_writer_thread, &wl);
    for (int i = 0; i < 3; i++) {
        pthread_create(&readers[i], NULL, wl_reader_thread, &wl);
    }

    pthread_join(writer, NULL);
    for (int i = 0; i < 3; i++) {
        pthread_join(readers[i], NULL);
    }

    ASSERT(wl.count >= 0);
    whitelist_cleanup(&wl);
    cleanup_test_file();
}

// boundary tests
// fill whitelist; next add must fail with -1
TEST(whitelist_boundary_max_capacity)
{
    create_test_file("");
    Whitelist wl;
    ASSERT_EQ(whitelist_load(&wl, TEST_WL_FILE), 0);

    for (int i = 0; i < WHITELIST_MAX; i++) {
        int rc = whitelist_add(&wl, (int64_t)(i + 1));
        ASSERT_EQ(rc, 0);
    }
    ASSERT_EQ(wl.count, WHITELIST_MAX);

    // sorted invariant
    for (int i = 1; i < wl.count; i++) {
        ASSERT(wl.ids[i - 1] < wl.ids[i]);
    }

    // one more must be rejected
    ASSERT_EQ(whitelist_add(&wl, (int64_t)(WHITELIST_MAX + 1)), -1);
    ASSERT_EQ(wl.count, WHITELIST_MAX);

    whitelist_cleanup(&wl);
    cleanup_test_file();
}

// Load file with more than WHITELIST_MAX lines; extra lines silently dropped
TEST(whitelist_boundary_overloaded_file)
{
    // write WHITELIST_MAX + 50 IDs to file
    FILE *f = fopen(TEST_WL_FILE, "w");
    ASSERT_NOT_NULL(f);
    for (int i = 0; i < WHITELIST_MAX + 50; i++) {
        fprintf(f, "%d\n", i + 1);
    }
    fclose(f);

    Whitelist wl;
    ASSERT_EQ(whitelist_load(&wl, TEST_WL_FILE), 0);
    ASSERT_EQ(wl.count, WHITELIST_MAX);

    // sorted invariant after truncated load
    for (int i = 1; i < wl.count; i++) {
        ASSERT(wl.ids[i - 1] < wl.ids[i]);
    }

    whitelist_cleanup(&wl);
    cleanup_test_file();
}

// null lines in file are handled gracefully
TEST(whitelist_load_garbage_lines)
{
    create_test_file("100\nhello\n\n200\n100\nxyz\n300\n");
    Whitelist wl;
    ASSERT_EQ(whitelist_load(&wl, TEST_WL_FILE), 0);
    // sscanf skips non-numeric lines; duplicates are loaded (sorted + dedup is not done)
    // At minimum, 100, 200, 300 must be present and count >= 3
    ASSERT(wl.count >= 3);
    ASSERT(whitelist_contains(&wl, 100));
    ASSERT(whitelist_contains(&wl, 200));
    ASSERT(whitelist_contains(&wl, 300));
    whitelist_cleanup(&wl);
    cleanup_test_file();
}

// negative + very large int64 IDs work correctly
TEST(whitelist_negative_and_large_ids)
{
    create_test_file("");
    Whitelist wl;
    ASSERT_EQ(whitelist_load(&wl, TEST_WL_FILE), 0);

    ASSERT_EQ(whitelist_add(&wl, -999), 0);
    ASSERT(whitelist_contains(&wl, -999));

    ASSERT_EQ(whitelist_add(&wl, INT64_MAX), 0);
    ASSERT(whitelist_contains(&wl, INT64_MAX));

    ASSERT_EQ(whitelist_add(&wl, INT64_MIN), 0);
    ASSERT(whitelist_contains(&wl, INT64_MIN));

    // sorted: INT64_MIN < -999 < INT64_MAX
    ASSERT(wl.ids[0] == INT64_MIN);
    ASSERT(wl.ids[1] == -999);
    ASSERT(wl.ids[2] == INT64_MAX);

    // remove and verify
    ASSERT_EQ(whitelist_remove(&wl, -999), 0);
    ASSERT(!whitelist_contains(&wl, -999));
    ASSERT_EQ(wl.count, 2);

    whitelist_cleanup(&wl);
    cleanup_test_file();
}

int main(void)
{
    printf("=== test_whitelist ===\n");
    return test_summarise();
}
