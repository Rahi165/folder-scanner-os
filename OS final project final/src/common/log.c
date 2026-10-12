#include "log.h"      /* our own header — LogLevel and function declarations */
#include <stdio.h>    /* for FILE, fprintf, fopen, fclose, fflush */
#include <stdarg.h>   /* for va_list, va_start, va_end — needed for ... arguments */
#include <time.h>     /* for time(), strftime(), localtime() — to get current time */
#include <pthread.h>  /* for pthread_mutex_t — to make logging thread-safe */

/* The file where logs are written — NULL means use stderr (screen) */
static FILE *log_fp = NULL;

/*
 * log_lock — mutex that protects the logger.
 * Only one thread can write a log message at a time.
 * Without this, 8 threads writing simultaneously would produce garbled output.
 * PTHREAD_MUTEX_INITIALIZER sets up the mutex at program start automatically.
 */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * log_init — sets up where log messages will be written.
 * filepath — if provided, logs go to that file. If NULL, logs go to screen.
 * Locks the mutex before changing log_fp to be thread-safe.
 */
void log_init(const char *filepath) {
    pthread_mutex_lock(&log_lock);   /* lock before changing shared variable */
    log_fp = filepath ? fopen(filepath, "a") : stderr;
    /* fopen with "a" = append mode — adds to existing file instead of overwriting */
    if (!log_fp) log_fp = stderr;    /* if file open failed, fall back to screen */
    pthread_mutex_unlock(&log_lock); /* unlock so other threads can log */
}

/*
 * log_msg — writes one timestamped log message.
 * This is the core logging function — called by LOG_INFO, LOG_WARN, LOG_ERROR.
 *
 * Steps:
 *   1. Get the current time and format it as HH:MM:SS
 *   2. Lock the mutex
 *   3. Print [time][level] message
 *   4. Flush so it appears immediately
 *   5. Unlock the mutex
 *
 * The ... (variadic arguments) allow printf-style formatting:
 *   LOG_INFO("fetched page %d at %s", docid, url);
 */
void log_msg(LogLevel level, const char *fmt, ...) {
    /* array of label strings — index matches LogLevel enum values */
    static const char *labels[] = { "INFO", "WARN", "ERROR" };

    /* get current time as seconds since 1970 */
    time_t now = time(NULL);

    /* buffer to hold the formatted time string "HH:MM:SS" */
    char tbuf[32];

    /* format time as "10:42:04"
     * localtime_r (not localtime) because localtime() uses one shared static
     * buffer — two threads calling it at the same time would be a data race. */
    struct tm tm_buf;
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", localtime_r(&now, &tm_buf));

    /* va_list handles the variable number of arguments (...) */
    va_list ap;
    va_start(ap, fmt); /* initialize ap to point to arguments after fmt */

    pthread_mutex_lock(&log_lock); /* lock — only this thread writes now */

    /* use log_fp if set, otherwise fall back to stderr */
    FILE *fp = log_fp ? log_fp : stderr;

    /* print the timestamp and level: "[10:42:04][INFO] " */
    fprintf(fp, "[%s][%s] ", tbuf, labels[level]);

    /* print the actual message with its arguments (like printf but uses va_list) */
    vfprintf(fp, fmt, ap);

    /* print a newline at the end */
    fprintf(fp, "\n");

    /* flush immediately so message appears right away (not buffered) */
    fflush(fp);

    pthread_mutex_unlock(&log_lock); /* unlock — other threads can log now */

    va_end(ap); /* clean up the va_list */
}

/*
 * log_close — flushes and closes the log file.
 * Only closes if we opened a file (not stderr — we don't own stderr).
 * Call after all threads have finished to ensure all messages are written.
 */
void log_close(void) {
    pthread_mutex_lock(&log_lock);                    /* lock before touching log_fp */
    if (log_fp && log_fp != stderr) fclose(log_fp);  /* close file if we opened one */
    log_fp = NULL;                                    /* reset to NULL */
    pthread_mutex_unlock(&log_lock);                  /* unlock */
}
