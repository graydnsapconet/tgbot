#pragma once

#include <stdint.h>

/* per-user message queue with rate-limiting
 * thread-safe: multiple producers (handle_update), multiple consumers (workers)
 *
 * initialise the global queue (call once from main before spawning workers)
 * ring_size: per-user ring buffer depth (e.g. 30)
 * returns 0 on success
 */
int queue_init(int ring_size);

// tear down the global queue and free all memory
void queue_destroy(void);

// enqueue a message for a user; timestamps the message on ingress
// returns 0 on success, -1 if the user's ring is full (drop newest policy)
int queue_push(int64_t user_id, int64_t chat_id, const char *text);

// a popped message ready for the worker to send
typedef struct {
    int64_t user_id;
    int64_t chat_id;
    char text[1024];
    double ingress_sec; // CLOCK_MONOTONIC seconds at enqueue time
} QueueMsg;

// block until a message is available (or shutdown is signalled)
// returns 0 on success and fills *out, -1 on shutdown
int queue_pop(QueueMsg *out);

// signal all blocked workers to wake up and exit
void queue_shutdown(void);

// return the total number of pending messages across all users (thread-safe)
int queue_depth(void);

// return the number of allocated user rings (thread-safe)
int queue_ring_count(void);
