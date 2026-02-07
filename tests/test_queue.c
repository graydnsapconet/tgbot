#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "test.h"
#include "../src/config.h"
#include "../src/queue.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// basic push/pop: message content is preserved
TEST(queue_message_idempotency)
{
    ASSERT_EQ(queue_init(8), 0);

    const char *msg = "Hello, world! 🤖";
    ASSERT_EQ(queue_push(100, 200, msg), 0);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT_EQ(out.user_id, 100);
    ASSERT_EQ(out.chat_id, 200);
    ASSERT_STR_EQ(out.text, msg);

    queue_shutdown();
    queue_destroy();
}

// queue overflow: pushing beyond ring capacity returns -1, no crash
TEST(queue_overflow_no_crash)
{
    int ring_sz = 4; // will round up to 4 (power of 2)
    ASSERT_EQ(queue_init(ring_sz), 0);

    // fill the ring for user 42
    for (int i = 0; i < 4; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "msg %d", i);
        ASSERT_EQ(queue_push(42, 100, buf), 0);
    }

    // next push should fail (ring full) - drop-newest policy
    ASSERT_EQ(queue_push(42, 100, "overflow"), -1);

    // drain and verify first message is intact
    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT_STR_EQ(out.text, "msg 0");

    // now we can push again
    ASSERT_EQ(queue_push(42, 100, "after drain"), 0);

    queue_shutdown();
    queue_destroy();
}

// multiple users: round-robin fairness
TEST(queue_round_robin_fairness)
{
    ASSERT_EQ(queue_init(16), 0);

    // push 3 messages for each of 3 users
    for (int u = 1; u <= 3; u++) {
        for (int m = 0; m < 3; m++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "u%d-m%d", u, m);
            ASSERT_EQ(queue_push((int64_t)u, (int64_t)(u * 100), buf), 0);
        }
    }

    // pop all 9 messages; verify we see all users (not just one)
    int user_seen[4] = {0}; // indices 1-3
    for (int i = 0; i < 9; i++) {
        QueueMsg out;
        ASSERT_EQ(queue_pop(&out), 0);
        ASSERT(out.user_id >= 1 && out.user_id <= 3);
        user_seen[out.user_id]++;
    }

    // each user must have exactly 3 messages popped
    for (int u = 1; u <= 3; u++) {
        ASSERT_EQ(user_seen[u], 3);
    }

    queue_shutdown();
    queue_destroy();
}

// helper for shutdown test
static int g_pop_result = 99;

static void *shutdown_pop_thread(void *arg)
{
    (void)arg;
    QueueMsg msg;
    g_pop_result = queue_pop(&msg);
    return NULL;
}

// shutdown unblocks pop
TEST(queue_shutdown_unblocks_pop)
{
    ASSERT_EQ(queue_init(8), 0);

    g_pop_result = 99;
    pthread_t t;
    pthread_create(&t, NULL, shutdown_pop_thread, NULL);

    usleep(50000); // let thread block on pop
    queue_shutdown();
    pthread_join(t, NULL);

    ASSERT_EQ(g_pop_result, -1); // pop returns -1 on shutdown

    queue_destroy();
}

// ingress timestamp is set (for rate-limiter)
TEST(queue_ingress_timestamp)
{
    ASSERT_EQ(queue_init(8), 0);

    double before = monotonic_sec();
    ASSERT_EQ(queue_push(1, 1, "timing"), 0);
    double after = monotonic_sec();

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT(out.ingress_sec >= before);
    ASSERT(out.ingress_sec <= after);

    queue_shutdown();
    queue_destroy();
}

// rate limiter: worker should delay based on ingress timestamp
// simulate worker delay logic from main.c
TEST(queue_rate_limiter_delay)
{
    ASSERT_EQ(queue_init(8), 0);

    ASSERT_EQ(queue_push(1, 1, "rate-test"), 0);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);

    // simulate worker reply_delay = 1 second
    int reply_delay = 1;
    double now = monotonic_sec();
    double delta = now - out.ingress_sec;
    double wait = (double)reply_delay - delta;

    // since we popped almost immediately, wait should be positive (~1s)
    ASSERT(wait > 0.0);
    ASSERT(wait <= 1.1); // should not exceed reply_delay + epsilon

    queue_shutdown();
    queue_destroy();
}

// high-throughput: push/pop 10000 messages without crash or corruption
TEST(queue_high_throughput)
{
    ASSERT_EQ(queue_init(256), 0);

    int n = 10000;
    int pushed = 0;
    int popped = 0;

    // use a single user with large ring, push+pop in batches
    for (int batch = 0; batch < n / 256 + 1; batch++) {
        int to_push = 256;
        if (pushed + to_push > n) {
            to_push = n - pushed;
        }

        for (int i = 0; i < to_push; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "ht-%d", pushed + i);
            int rc = queue_push(1, 1, buf);
            if (rc == 0) {
                pushed++;
            }
        }

        // drain what we pushed
        for (int i = 0; i < to_push; i++) {
            QueueMsg out;
            if (queue_pop(&out) == 0) {
                popped++;
            } else {
                break;
            }
        }
    }

    ASSERT_EQ(pushed, popped);
    ASSERT_EQ(pushed, n);

    queue_shutdown();
    queue_destroy();
}

// text truncation: message longer than 1023 bytes is clipped, not overflow
TEST(queue_text_truncation)
{
    ASSERT_EQ(queue_init(8), 0);

    // build a 2000-byte message
    char big[2001];
    memset(big, 'X', 2000);
    big[2000] = '\0';

    ASSERT_EQ(queue_push(1, 1, big), 0);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    // text should be truncated to sizeof(out.text)-1 = 1023
    ASSERT_EQ((int)strlen(out.text), 1023);

    queue_shutdown();
    queue_destroy();
}

// helper for concurrent producer test
#define NTHREADS 4
#define MSGS_PER_THREAD 500

static void *producer_thread(void *arg)
{
    int tid = *(int *)arg;
    for (int i = 0; i < MSGS_PER_THREAD; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "t%d-m%d", tid, i);
        queue_push((int64_t)(tid + 1), (int64_t)(tid + 1), buf);
    }
    return NULL;
}

// concurrent producers: 4 threads push simultaneously, no crash
TEST(queue_concurrent_producers)
{
    ASSERT_EQ(queue_init(64), 0);

    pthread_t threads[NTHREADS];
    int tids[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, producer_thread, &tids[i]);
    }

    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // drain all messages - should not crash
    int count = 0;
    QueueMsg out;
    queue_shutdown();
    while (queue_pop(&out) == 0) {
        count++;
    }

    // we can't know exactly how many were dropped due to ring overflow
    // but count should be positive and no crash occurred
    ASSERT(count > 0);

    queue_destroy();
}

// idle ring freed: push from user, drain to zero, push again succeeds
TEST(queue_idle_ring_freed)
{
    ASSERT_EQ(queue_init(8), 0);

    // push and drain - ring should be freed on drain
    ASSERT_EQ(queue_push(999, 999, "first"), 0);
    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT_STR_EQ(out.text, "first");
    ASSERT_EQ(out.user_id, 999);

    // user 999's ring was freed; push again triggers re-creation
    ASSERT_EQ(queue_push(999, 999, "second"), 0);
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT_STR_EQ(out.text, "second");

    queue_shutdown();
    queue_destroy();
}

// many transient users: rings freed, no unbounded memory growth
TEST(queue_transient_users_no_leak)
{
    ASSERT_EQ(queue_init(4), 0);

    // simulate 200 transient users each sending one message
    for (int i = 0; i < 200; i++) {
        ASSERT_EQ(queue_push((int64_t)(10000 + i), (int64_t)(10000 + i), "hi"), 0);
        QueueMsg out;
        ASSERT_EQ(queue_pop(&out), 0);
        ASSERT_EQ(out.user_id, (int64_t)(10000 + i));
    }

    // all rings should have been freed after draining
    // push/pop cycle for a new user should still work fine
    ASSERT_EQ(queue_push(1, 1, "still works"), 0);
    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT_STR_EQ(out.text, "still works");

    queue_shutdown();
    queue_destroy();
}

// queue_depth() returns correct pending count
TEST(queue_depth_tracking)
{
    ASSERT_EQ(queue_init(16), 0);

    ASSERT_EQ(queue_depth(), 0);

    ASSERT_EQ(queue_push(1, 1, "a"), 0);
    ASSERT_EQ(queue_push(2, 2, "b"), 0);
    ASSERT_EQ(queue_push(3, 3, "c"), 0);
    ASSERT_EQ(queue_depth(), 3);

    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT_EQ(queue_depth(), 2);

    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT_EQ(queue_depth(), 0);

    queue_shutdown();
    queue_destroy();
}

// queue_ring_count() returns correct number of allocated user rings
TEST(queue_ring_count_tracking)
{
    ASSERT_EQ(queue_init(8), 0);

    ASSERT_EQ(queue_ring_count(), 0);

    // push for 3 distinct users
    ASSERT_EQ(queue_push(10, 10, "x"), 0);
    ASSERT_EQ(queue_push(20, 20, "y"), 0);
    ASSERT_EQ(queue_push(30, 30, "z"), 0);
    ASSERT_EQ(queue_ring_count(), 3);

    // drain user 10 - pop round-robins, so drain one from each
    QueueMsg out;
    ASSERT_EQ(queue_pop(&out), 0); // pops from one user
    // ring freed on drain -> count should drop
    ASSERT_EQ(queue_ring_count(), 2);

    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT_EQ(queue_ring_count(), 1);

    ASSERT_EQ(queue_pop(&out), 0);
    ASSERT_EQ(queue_ring_count(), 0);

    queue_shutdown();
    queue_destroy();
}

// concurrent producers: verify message integrity
TEST(queue_concurrent_integrity)
{
    ASSERT_EQ(queue_init(256), 0);

    // each thread writes messages with unique prefix "tN-mM"
    pthread_t threads[NTHREADS];
    int tids[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, producer_thread, &tids[i]);
    }

    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    queue_shutdown();

    // drain and verify each message matches expected format "tN-mM"
    int count = 0;
    int valid = 0;
    QueueMsg out;
    while (queue_pop(&out) == 0) {
        count++;
        int tid, mid;
        if (sscanf(out.text, "t%d-m%d", &tid, &mid) == 2) {
            if (tid >= 0 && tid < NTHREADS && mid >= 0 && mid < MSGS_PER_THREAD) {
                valid++;
            }
        }
    }

    ASSERT(count > 0);
    ASSERT_EQ(valid, count);

    queue_destroy();
}

int main(void)
{
    printf("=== test_queue ===\n");
    return test_summarise();
}

// Ring buffer property tests

// no overwrite invariant: fill ring, push one more, verify rejection
// and all existing slots are intact
TEST(queue_property_no_overwrite)
{
    int ring_sz = 8;
    ASSERT_EQ(queue_init(ring_sz), 0);

    // fill the ring for user 1 with known content
    char expected[8][64];
    for (int i = 0; i < 8; i++) {
        snprintf(expected[i], sizeof(expected[i]), "slot-%d", i);
        ASSERT_EQ(queue_push(1, 1, expected[i]), 0);
    }

    // next push should be rejected
    ASSERT_EQ(queue_push(1, 1, "overflow"), -1);

    // pop all and verify content matches original (no corruption)
    for (int i = 0; i < 8; i++) {
        QueueMsg out;
        ASSERT_EQ(queue_pop(&out), 0);
        ASSERT_STR_EQ(out.text, expected[i]);
    }

    queue_shutdown();
    queue_destroy();
}

// per-user FIFO ordering preserved
TEST(queue_property_fifo_order)
{
    ASSERT_EQ(queue_init(32), 0);

    // push 20 messages for same user
    for (int i = 0; i < 20; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "msg-%03d", i);
        ASSERT_EQ(queue_push(42, 42, buf), 0);
    }

    // pop and verify FIFO order
    for (int i = 0; i < 20; i++) {
        QueueMsg out;
        ASSERT_EQ(queue_pop(&out), 0);
        char expected[64];
        snprintf(expected, sizeof(expected), "msg-%03d", i);
        ASSERT_STR_EQ(out.text, expected);
    }

    queue_shutdown();
    queue_destroy();
}

// bounded memory: 1000 unique users, verify ring count and cleanup
TEST(queue_property_bounded_memory)
{
    ASSERT_EQ(queue_init(4), 0);

    // push from 1000 unique users
    for (int i = 0; i < 1000; i++) {
        queue_push((int64_t)(5000 + i), (int64_t)(5000 + i), "x");
    }

    ASSERT(queue_ring_count() <= 1000);

    queue_shutdown();

    // pop all
    QueueMsg out;
    while (queue_pop(&out) == 0) {
        // drain
    }

    // after draining, all rings should be freed
    ASSERT_EQ(queue_ring_count(), 0);
    ASSERT_EQ(queue_depth(), 0);

    queue_destroy();
}

// Capacity is always power-of-2: test this indirectly,
// init with various sizes and try to fill to the next power-of-2.
// all pushes should succeed
TEST(queue_property_capacity_pow2)
{
    int test_sizes[] = {1, 3, 7, 15, 31, 255};
    int expected_caps[] = {4, 4, 8, 16, 32, 256};
    int n = (int)(sizeof(test_sizes) / sizeof(test_sizes[0]));

    for (int t = 0; t < n; t++) {
        ASSERT_EQ(queue_init(test_sizes[t]), 0);

        // push expected_caps[t] messages - all should succeed
        for (int i = 0; i < expected_caps[t]; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "p2-%d", i);
            ASSERT_EQ(queue_push(1, 1, buf), 0);
        }

        // one more should fail (ring full)
        ASSERT_EQ(queue_push(1, 1, "overflow"), -1);

        queue_shutdown();
        QueueMsg out;
        while (queue_pop(&out) == 0) {
        }
        queue_destroy();
    }
}

// hash distribution: 1000 sequential user IDs, no severely unbalanced bucket
TEST(queue_property_hash_distribution)
{
    ASSERT_EQ(queue_init(4), 0);

    // push one message per user for 1000 sequential users
    for (int i = 0; i < 1000; i++) {
        queue_push((int64_t)(10000 + i), (int64_t)(10000 + i), "h");
    }

    // can only verify through ring_count that all 1000 are present
    ASSERT_EQ(queue_ring_count(), 1000);

    /* The queue uses 64 buckets internally (QUEUE_BUCKETS).
     * Average load = 1000/64 ≈ 15.6
     * We can't inspect buckets directly, but if the hash is severely broken,
     * one bucket would have all 1000 and round-robin would break.
     * We verify by checking that round-robin pops from multiple users
     * in the first 20 pops (not just one user repeated).
     */
    queue_shutdown();

    int unique_users[20] = {0};
    int unique_count = 0;
    QueueMsg out;
    for (int i = 0; i < 20 && queue_pop(&out) == 0; i++) {
        int found = 0;
        for (int j = 0; j < unique_count; j++) {
            if (unique_users[j] == (int)out.user_id) {
                found = 1;
                break;
            }
        }
        if (!found && unique_count < 20) {
            unique_users[unique_count++] = (int)out.user_id;
        }
    }

    // should see at least 10 unique users in the first 20 pops
    // (with a good hash and round-robin)
    ASSERT(unique_count >= 10);

    // drain remaining
    while (queue_pop(&out) == 0) {
    }

    queue_destroy();
}
