#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "test.h"
#include "../src/config.h"
#include "../src/logger.h"
#include "../src/queue.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// # queue fairness under contention
#define FAIR_NTHREADS 8
#define FAIR_MSGS_PER 100

static void *fairness_producer(void *arg)
{
    int64_t uid = *(int64_t *)arg;
    for (int i = 0; i < FAIR_MSGS_PER; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "u%" PRId64 "-m%d", uid, i);
        queue_push(uid, uid, buf);
        // small jitter to simulate real contention
        struct timespec ts = {0, (long)(rand() % 100000)};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

TEST(stress_queue_fairness)
{
    ASSERT_EQ(queue_init(256), 0);

    pthread_t threads[FAIR_NTHREADS];
    int64_t uids[FAIR_NTHREADS];
    for (int i = 0; i < FAIR_NTHREADS; i++) {
        uids[i] = (int64_t)(1000 + i);
        pthread_create(&threads[i], NULL, fairness_producer, &uids[i]);
    }

    for (int i = 0; i < FAIR_NTHREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    queue_shutdown();

    // pop all and count per-user
    int counts[FAIR_NTHREADS] = {0};
    QueueMsg out;
    while (queue_pop(&out) == 0) {
        for (int i = 0; i < FAIR_NTHREADS; i++) {
            if (out.user_id == 1000 + i) {
                counts[i]++;
                break;
            }
        }
    }

    // verify no single user > 2x fair share of what was popped
    int total = 0;
    for (int i = 0; i < FAIR_NTHREADS; i++) {
        total += counts[i];
    }
    ASSERT(total > 0);

    int fair_share = total / FAIR_NTHREADS;
    for (int i = 0; i < FAIR_NTHREADS; i++) {
        // no user should have more than 2x the average share + some buffer
        ASSERT(counts[i] <= fair_share * 2 + 10);
    }

    queue_destroy();
}

// # queue push after shutdown
TEST(stress_queue_push_after_shutdown)
{
    ASSERT_EQ(queue_init(8), 0);

    queue_shutdown();

    // push after shutdown should still work (shutdown only wakes poppers)
    // but pop should return -1 when queue is empty
    int rc = queue_push(42, 42, "after shutdown");
    // push may succeed (it writes to ring), pop should eventually drain
    (void)rc;

    QueueMsg out;
    // try to pop - with shutdown flag, if queue has items it pops them,
    // once empty returns -1
    int pop_rc = queue_pop(&out);
    if (pop_rc == 0) {
        // ok, got the message
        ASSERT_STR_EQ(out.text, "after shutdown");
    }
    // subsequent pop should fail
    ASSERT_EQ(queue_pop(&out), -1);

    queue_destroy();
}

// # double shutdown
TEST(stress_queue_double_shutdown)
{
    ASSERT_EQ(queue_init(8), 0);

    queue_shutdown();
    queue_shutdown(); // second call should not crash

    ASSERT(1); // survived

    queue_destroy();
}

// # logger wrap under multi-thread load
#define LOG_STRESS_THREADS 8
#define LOG_STRESS_ENTRIES 2000
#define LOG_STRESS_CAP 4096

static const char *g_log_stress_path = "/tmp/tgbot_test_stress_log.log";

static void *log_stress_writer(void *arg)
{
    int id = *(int *)arg;
    for (int i = 0; i < LOG_STRESS_ENTRIES; i++) {
        log_info("stress t%d iter %04d pad pad pad pad", id, i);
    }
    return NULL;
}

TEST(stress_logger_concurrent_wrap)
{
    unlink(g_log_stress_path);
    ASSERT_EQ(log_init(g_log_stress_path, LOG_STRESS_CAP), 0);
    log_set_level(LOG_DEBUG);

    pthread_t threads[LOG_STRESS_THREADS];
    int ids[LOG_STRESS_THREADS];
    for (int i = 0; i < LOG_STRESS_THREADS; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, log_stress_writer, &ids[i]);
    }

    for (int i = 0; i < LOG_STRESS_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    log_close();

    // verify file didn't exceed cap
    FILE *fp = fopen(g_log_stress_path, "rb");
    ASSERT_NOT_NULL(fp);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fclose(fp);
    ASSERT(sz > 0);
    ASSERT((size_t)sz <= LOG_STRESS_CAP);

    unlink(g_log_stress_path);
}

// # multiple concurrent consumers
static int g_consumed_count;
static pthread_mutex_t g_consumed_mtx = PTHREAD_MUTEX_INITIALIZER;

static void *consumer_thread(void *arg)
{
    (void)arg;
    QueueMsg out;
    while (queue_pop(&out) == 0) {
        pthread_mutex_lock(&g_consumed_mtx);
        g_consumed_count++;
        pthread_mutex_unlock(&g_consumed_mtx);
    }
    return NULL;
}

TEST(stress_multiple_consumers)
{
    ASSERT_EQ(queue_init(64), 0);
    g_consumed_count = 0;

    // push 200 messages from 10 users
    for (int u = 0; u < 10; u++) {
        for (int m = 0; m < 20; m++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "u%d-m%d", u, m);
            queue_push((int64_t)(u + 1), (int64_t)(u + 1), buf);
        }
    }

    // start 4 consumer threads
    pthread_t consumers[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&consumers[i], NULL, consumer_thread, NULL);
    }

    // let them consume for a bit, then shutdown
    usleep(100000);
    queue_shutdown();

    for (int i = 0; i < 4; i++) {
        pthread_join(consumers[i], NULL);
    }

    ASSERT_EQ(g_consumed_count, 200);

    queue_destroy();
}

int main(void)
{
    printf("=== test_stress ===\n");
    return test_summarise();
}
