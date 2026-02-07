#pragma once

#include <stddef.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3,
} LogLevel;

/**
 * Initialise the logger.  Opens (or creates) the file at @path and sets the
 * circular-file cap to @max_bytes.  If the file already contains the overwrite
 * marker from a previous run the write position is recovered automatically.
 *
 * Call once from the main thread before any other log_* function.
 * Returns 0 on success, -1 on error.
 */
int log_init(const char *path, size_t max_bytes);

/**
 * Flush and close the log file, destroy the internal mutex.
 * Safe to call even if log_init was never called (no-op).
 */
void log_close(void);

/**
 * Set the minimum log level.  Messages below this level are silently dropped.
 * Default level after log_init is LOG_INFO.
 */
void log_set_level(LogLevel level);

/**
 * Write a formatted log line.  Thread-safe.
 * The line is written both to the circular log file AND to stderr so that
 * systemd journal / terminal output works out of the box.
 *
 * If log_init has not been called yet, the line goes to stderr only.
 */
__attribute__((format(printf, 2, 3)))
void log_write(LogLevel level, const char *fmt, ...);

/**
 * Read the log file at @path (respecting circular layout) and print the last
 * @n lines to stdout.  Returns 0 on success, -1 on error.
 */
int log_read_last_n(const char *path, int n);

/**
 * Tail-follow the log file using inotify.  Blocks until SIGINT / SIGTERM.
 * Returns 0 on clean exit, -1 on error.
 */
int log_follow(const char *path);

// convenience macros
#define log_debug(...) log_write(LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  log_write(LOG_INFO, __VA_ARGS__)
#define log_warn(...)  log_write(LOG_WARN, __VA_ARGS__)
#define log_error(...) log_write(LOG_ERROR, __VA_ARGS__)