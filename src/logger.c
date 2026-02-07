#define _POSIX_C_SOURCE 200809L

#include "logger.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define MARKER          "---^-OVERWRITE-^---\n"
#define MARKER_LEN      20
#define LINE_BUF_MAX    4096
#define MIN_FILE_CAP    256   // smallest permissible max_bytes
#define TIMESTAMP_LEN   22    // "[2026-02-07 12:34:56] "
#define TIMESTAMP_BUF   32    // extra room to satisfy -Wformat-truncation

static const char *const g_level_tags[] = {
    [LOG_DEBUG] = "DEBUG",
    [LOG_INFO] = "INFO",
    [LOG_WARN] = "WARN",
    [LOG_ERROR] = "ERROR",
};

// state
static FILE *g_fp;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t g_max_bytes;
static size_t g_write_pos;
static int g_overwriting;
static int g_initialised;
static _Atomic int g_min_level = LOG_INFO;

// format a timestamp into @buf (must be >= TIMESTAMP_LEN + 1 bytes)
static void fmt_timestamp(char *buf, size_t cap)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    int year = tm.tm_year + 1900;
    if (year < 0) {
        year = 0;
    }
    if (year > 9999) {
        year = 9999;
    }
    snprintf(buf, cap, "[%04d-%02d-%02d %02d:%02d:%02d] ", year, (tm.tm_mon + 1) % 100,
             tm.tm_mday % 100, tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100);
}

/**
 * search the file for the overwrite marker. Returns the byte offset of the
 * marker if found, or -1.
 */
static long find_marker(FILE *fp, size_t file_size)
{
    if (file_size < MARKER_LEN) {
        return -1;
    }

    // read entire file into a stack buffer, or heap if too large
    char stack_buf[65536];
    char *buf = stack_buf;
    int heap = 0;
    if (file_size > sizeof(stack_buf)) {
        buf = malloc(file_size);
        if (!buf) {
            return -1;
        }
        heap = 1;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        if (heap) {
            free(buf);
        }
        return -1;
    }
    size_t got = fread(buf, 1, file_size, fp);
    if (got < MARKER_LEN) {
        if (heap) {
            free(buf);
        }
        return -1;
    }

    // scan for the marker
    const char *p = buf;
    const char *end = buf + got - MARKER_LEN;
    long result = -1;
    while (p <= end) {
        if (memcmp(p, MARKER, MARKER_LEN) == 0) {
            result = (long)(p - buf);
            break;
        }
        // skip to next newline for efficiency
        const char *nl = memchr(p, '\n', (size_t)(end - p + MARKER_LEN));
        if (!nl) {
            break;
        }
        p = nl + 1;
    }

    if (heap) {
        free(buf);
    }
    return result;
}

/**
 * Raw write to the circular file at g_write_pos.
 * Caller must hold g_lock.
 */
static void raw_write(const char *data, size_t len)
{
    if (!g_fp || len == 0) {
        return;
    }

    if (fseek(g_fp, (long)g_write_pos, SEEK_SET) != 0) {
        return;
    }
    size_t written = fwrite(data, 1, len, g_fp);
    fflush(g_fp);
    g_write_pos += written;
}

/**
 * Write the overwrite marker at the current g_write_pos (only if overwriting).
 * Caller must hold g_lock.
 */
static void write_marker(void)
{
    if (!g_overwriting || !g_fp) {
        return;
    }
    if (fseek(g_fp, (long)g_write_pos, SEEK_SET) != 0) {
        return;
    }
    fwrite(MARKER, 1, MARKER_LEN, g_fp);
    fflush(g_fp);
    // DO NOT advance g_write_pos - next write will overwrite the marker
}

// API
int log_init(const char *path, size_t max_bytes)
{
    if (!path || max_bytes < MIN_FILE_CAP) {
        return -1;
    }

    pthread_mutex_lock(&g_lock);

    if (g_fp) {
        fclose(g_fp);
        g_fp = NULL;
    }

    // open in r+b if file exists, otherwise w+b to create
    g_fp = fopen(path, "r+b");
    if (!g_fp) {
        if (errno == ENOENT) {
            g_fp = fopen(path, "w+b");
        }
        if (!g_fp) {
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
    }

    g_max_bytes = max_bytes;
    g_overwriting = 0;
    g_write_pos = 0;

    // determine file size
    fseek(g_fp, 0, SEEK_END);
    long fsize = ftell(g_fp);
    size_t file_size = (fsize > 0) ? (size_t)fsize : 0;

    if (file_size > 0) {
        // try to recover write position from marker
        long marker_off = find_marker(g_fp, file_size);
        if (marker_off >= 0) {
            g_write_pos = (size_t)marker_off;
            g_overwriting = 1;
        } else if (file_size >= max_bytes) {
            // file is full but no marker - wrap to top
            g_write_pos = 0;
            g_overwriting = 1;
        } else {
            // file not full, append
            g_write_pos = file_size;
        }
    }

    g_initialised = 1;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void log_close(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_fp) {
        fflush(g_fp);
        fclose(g_fp);
        g_fp = NULL;
    }
    g_initialised = 0;
    pthread_mutex_unlock(&g_lock);
}

void log_set_level(LogLevel level)
{
    atomic_store_explicit(&g_min_level, (int)level, memory_order_relaxed);
}

void log_write(LogLevel level, const char *fmt, ...)
{
    if ((int)level < atomic_load_explicit(&g_min_level, memory_order_relaxed)) {
        return;
    }

    // format the message on the stack
    char ts[TIMESTAMP_BUF];
    fmt_timestamp(ts, sizeof(ts));

    const char *tag = g_level_tags[level];
    char line[LINE_BUF_MAX];
    int prefix_len = snprintf(line, sizeof(line), "%s[%-5s] ", ts, tag);
    if (prefix_len < 0) {
        prefix_len = 0;
    }

    va_list ap;
    va_start(ap, fmt);
    int msg_len = vsnprintf(line + prefix_len, sizeof(line) - (size_t)prefix_len, fmt, ap);
    va_end(ap);
    if (msg_len < 0) {
        msg_len = 0;
    }

    size_t total = (size_t)prefix_len + (size_t)msg_len;
    if (total >= sizeof(line) - 1) {
        total = sizeof(line) - 2;
    }
    // ensure newline-terminated
    if (total == 0 || line[total - 1] != '\n') {
        line[total] = '\n';
        total++;
    }
    line[total] = '\0';

    // always write to stderr for journal/terminal
    fputs(line, stderr);

    // write to circular file if initialised
    pthread_mutex_lock(&g_lock);
    if (!g_fp) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    size_t line_len = total;

    // ensure a single line can never exceed (max_bytes - MARKER_LEN)
    size_t usable = g_max_bytes - MARKER_LEN;
    if (line_len > usable) {
        line_len = usable;
    }

    // check if this line would push us past the cap
    size_t space_needed = line_len + (g_overwriting ? MARKER_LEN : 0);
    if (g_write_pos + space_needed > g_max_bytes) {
        // erase the stale marker before wrapping so only one exists
        if (g_overwriting && g_write_pos + MARKER_LEN <= g_max_bytes) {
            char blanks[MARKER_LEN];
            memset(blanks, ' ', MARKER_LEN - 1);
            blanks[MARKER_LEN - 1] = '\n';
            if (fseek(g_fp, (long)g_write_pos, SEEK_SET) == 0) {
                fwrite(blanks, 1, MARKER_LEN, g_fp);
            }
        }
        // wrap to the top
        g_write_pos = 0;
        g_overwriting = 1;
    }

    raw_write(line, line_len);
    write_marker();

    pthread_mutex_unlock(&g_lock);
}

// reading

/**
 * Read the entire log file, reconstruct logical order if wrapped, and return
 * the last n lines via stdout.
 */
int log_read_last_n(const char *path, int n)
{
    if (!path || n <= 0) {
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "tgbot: cannot open log file: %s: %s\n", path, strerror(errno));
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0) {
        fclose(fp);
        fprintf(stderr, "tgbot: log file is empty\n");
        return -1;
    }
    size_t file_size = (size_t)fsize;

    char *buf = malloc(file_size + 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    fseek(fp, 0, SEEK_SET);
    size_t got = fread(buf, 1, file_size, fp);
    fclose(fp);
    buf[got] = '\0';

    // find marker to determine logical order
    const char *logical_start;
    size_t logical_len;
    const char *marker_pos = NULL;

    // scan for marker
    const char *p = buf;
    const char *end = buf + got;
    while (p <= end - MARKER_LEN) {
        if (memcmp(p, MARKER, MARKER_LEN) == 0) {
            marker_pos = p;
            break;
        }
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) {
            break;
        }
        p = nl + 1;
    }

    /*
     * if marker found the logical order is: [marker+MARKER_LEN .. EOF] then [0 .. marker)
     * (skip the marker itself)
     */
    char *ordered = NULL;
    if (marker_pos) {
        size_t after_marker = got - (size_t)(marker_pos - buf) - MARKER_LEN;
        size_t before_marker = (size_t)(marker_pos - buf);
        logical_len = after_marker + before_marker;
        ordered = malloc(logical_len + 1);
        if (!ordered) {
            free(buf);
            return -1;
        }
        memcpy(ordered, marker_pos + MARKER_LEN, after_marker);
        memcpy(ordered + after_marker, buf, before_marker);
        ordered[logical_len] = '\0';
        logical_start = ordered;
    } else {
        logical_start = buf;
        logical_len = got;
    }

    // collect line offsets (pointers into logical_start)
    int capacity = 256;
    const char **lines = malloc((size_t)capacity * sizeof(const char *));
    if (!lines) {
        free(ordered);
        free(buf);
        return -1;
    }
    int line_count = 0;

    const char *lp = logical_start;
    const char *lend = logical_start + logical_len;
    while (lp < lend) {
        // skip empty/partial lines at the very beginning (from overwrite)
        if (*lp == '\0') {
            lp++;
            continue;
        }
        if (line_count >= capacity) {
            capacity *= 2;
            const char **tmp = realloc(lines, (size_t)capacity * sizeof(const char *));
            if (!tmp) {
                break;
            }
            lines = tmp;
        }
        lines[line_count++] = lp;
        const char *nl = memchr(lp, '\n', (size_t)(lend - lp));
        if (nl) {
            lp = nl + 1;
        } else {
            break;
        }
    }

    // print last n lines
    int start = line_count - n;
    if (start < 0) {
        start = 0;
    }
    for (int i = start; i < line_count; i++) {
        const char *line = lines[i];
        const char *nl = memchr(line, '\n', (size_t)(lend - line));
        size_t len = nl ? (size_t)(nl - line + 1) : strlen(line);
        fwrite(line, 1, len, stdout);
    }

    free(lines);
    free(ordered);
    free(buf);
    return 0;
}

int log_follow(const char *path)
{
    if (!path) {
        return -1;
    }

    int ifd = inotify_init1(IN_CLOEXEC);
    if (ifd < 0) {
        fprintf(stderr, "tgbot: inotify_init failed: %s\n", strerror(errno));
        return -1;
    }

    int wd = inotify_add_watch(ifd, path, IN_MODIFY);
    if (wd < 0) {
        fprintf(stderr, "tgbot: inotify_add_watch failed: %s\n", strerror(errno));
        close(ifd);
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "tgbot: cannot open log file: %s: %s\n", path, strerror(errno));
        close(ifd);
        return -1;
    }

    // seek to end - we only print new content
    fseek(fp, 0, SEEK_END);
    long last_pos = ftell(fp);

    // block SIGINT/SIGTERM in a way that lets us break cleanly
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    char event_buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    for (;;) {
        // pselect to atomically unmask signals while waiting
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ifd, &rfds);

        sigprocmask(SIG_BLOCK, &mask, &oldmask);
        int ret = pselect(ifd + 1, &rfds, NULL, NULL, NULL, &oldmask);
        sigprocmask(SIG_UNBLOCK, &mask, NULL);

        if (ret < 0) {
            if (errno == EINTR) {
                break; // signal received - clean exit
            }
            break;
        }

        // drain inotify events.
        ssize_t len = read(ifd, event_buf, sizeof(event_buf));
        (void)len; // we only care that a modify happened.

        // read new content.
        fseek(fp, 0, SEEK_END);
        long cur_pos = ftell(fp);

        if (cur_pos < last_pos) {
            // file was truncated / wrapped - re-read from beginning.
            last_pos = 0;
        }

        if (cur_pos > last_pos) {
            fseek(fp, last_pos, SEEK_SET);
            char read_buf[8192];
            size_t to_read = (size_t)(cur_pos - last_pos);
            while (to_read > 0) {
                size_t chunk = to_read > sizeof(read_buf) ? sizeof(read_buf) : to_read;
                size_t got = fread(read_buf, 1, chunk, fp);
                if (got == 0) {
                    break;
                }
                fwrite(read_buf, 1, got, stdout);
                fflush(stdout);
                to_read -= got;
            }
            last_pos = cur_pos;
        }
    }

    fclose(fp);
    close(ifd);
    return 0;
}
