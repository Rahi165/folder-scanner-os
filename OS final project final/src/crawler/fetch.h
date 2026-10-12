#ifndef FETCH_H
#define FETCH_H

#include <stddef.h>  /* needed for size_t */

/*
 * FetchResult — holds the content of a file that was read from disk.
 * After a successful fetch, data points to the full text of the file.
 * The caller is responsible for freeing result->data when done.
 */
typedef struct {
    char  *data;   /* heap-allocated file content (null-terminated) */
    size_t size;   /* number of bytes in data (not counting the null terminator) */
} FetchResult;

/*
 * fetch_file — reads an entire file from disk into memory.
 * path   — the file to read (e.g. "testdata/os/threads.txt")
 * result — struct where the file content will be stored
 *
 * Returns  0 on success — result->data holds the content, caller must free it.
 * Returns -1 on any error (cannot open, read error, out of memory) — logs the failure.
 */
int fetch_file(const char *path, FetchResult *result);

#endif /* FETCH_H */
