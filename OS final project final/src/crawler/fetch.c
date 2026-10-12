#include "fetch.h"           /* our own header — FetchResult struct and declaration */
#include "../common/log.h"   /* for LOG_WARN — to log errors without crashing */
#include <stdio.h>           /* for fopen, fseek, ftell, fread, fclose */
#include <stdlib.h>          /* for malloc, free */
#include <string.h>          /* for strerror */
#include <errno.h>           /* for errno — why did the system call fail? */

/*
 * fetch_file — reads a whole file into a heap buffer.
 *
 * Steps:
 *   1. Open the file for reading ("rb" = read bytes, no newline translation)
 *   2. Seek to the end to learn the file size, then rewind to the start
 *   3. Allocate size+1 bytes (the +1 is for the null terminator)
 *   4. Read all bytes with fread() and null-terminate the buffer
 *
 * Any failure is logged and returns -1 — the crawler keeps running.
 */
int fetch_file(const char *path, FetchResult *result) {
    result->data = NULL;   /* NULL so the caller can tell if we failed */
    result->size = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_WARN("cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    /* find the file size: jump to the end, read the position, jump back */
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long len = ftell(fp);
    if (len < 0) { fclose(fp); return -1; }
    rewind(fp);

    char *buf = malloc((size_t)len + 1);   /* +1 for the null terminator */
    if (!buf) {
        LOG_ERROR("out of memory reading %s", path);
        fclose(fp);
        return -1;
    }

    size_t got = fread(buf, 1, (size_t)len, fp);   /* read the whole file */
    fclose(fp);
    if (got != (size_t)len) {
        LOG_WARN("short read on %s (%zu of %ld bytes)", path, got, len);
        free(buf);
        return -1;
    }

    buf[got]     = '\0';   /* null-terminate so it is a valid C string */
    result->data = buf;
    result->size = got;
    return 0;
}
