#ifndef LOG_H
#define LOG_H

/*
 * log.h — declares the thread-safe logging system.
 * Used by ALL three programs: crawler, indexer, and query.
 * Produces timestamped messages like: [10:42:04][INFO] fetched depth=0 ...
 */

/*
 * LogLevel — the three types of log messages.
 *   LOG_INFO  — normal progress message (everything is fine)
 *   LOG_WARN  — something failed but program keeps running (e.g. HTTP 404)
 *   LOG_ERROR — serious failure (e.g. can't open a file)
 */
typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;

/*
 * log_init — initializes the logger. Call once at program startup.
 * filepath — path to a log file, OR NULL to print to the screen (stderr).
 * Must be called before any threads are created.
 */
void log_init(const char *filepath);

/*
 * log_msg — writes a timestamped log message.
 * Thread-safe — can be called from any thread at any time.
 * Uses a mutex internally so messages from different threads don't mix.
 * level — LOG_INFO, LOG_WARN, or LOG_ERROR
 * fmt   — printf-style format string (e.g. "fetched page %d")
 */
void log_msg(LogLevel level, const char *fmt, ...);

/*
 * log_close — flushes and closes the log file.
 * Call once at program shutdown after all threads have finished.
 */
void log_close(void);

/* Shortcut macros so you don't have to type log_msg(LOG_INFO, ...) every time */
/* Usage: LOG_INFO("fetched page %d", docid);  */
#define LOG_INFO(...)  log_msg(LOG_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  log_msg(LOG_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) log_msg(LOG_ERROR, __VA_ARGS__)

#endif /* LOG_H */
