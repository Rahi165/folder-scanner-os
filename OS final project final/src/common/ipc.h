#ifndef IPC_H
#define IPC_H

/*
 * ipc.h — defines the message format shared between the crawler and indexer.
 * Both programs include this file so they agree on the exact structure
 * of every message sent over the UNIX domain socket.
 */

#include <stdint.h>  /* needed for uint32_t — a 32-bit unsigned integer */

/* Maximum number of characters allowed in the original file path */
#define IPC_SRC_MAX   2048

/* Maximum number of characters allowed in the saved-copy file path */
#define IPC_PATH_MAX   512

/*
 * CrawlMsg — the message the crawler sends to the indexer for every file scanned.
 *
 * Wire format: the crawler writes sizeof(CrawlMsg) bytes in one send() call.
 * The indexer reads sizeof(CrawlMsg) bytes per message in a loop.
 * Fixed size means no need for length headers or delimiters — simple and fast.
 *
 * Fields:
 *   docid    — unique ID number assigned to this file (0, 1, 2, 3...)
 *   depth    — how many subdirectory levels below the root folder it was found
 *   src_path — where the file was found (e.g. "testdata/os/threads.txt")
 *   filepath — where the crawler saved a copy (e.g. "data/pages/0.txt")
 */
typedef struct {
    uint32_t docid;                  /* unique file number assigned by the crawler */
    uint32_t depth;                  /* folder depth (0 = directly inside the root folder) */
    char     src_path[IPC_SRC_MAX];  /* null-terminated original path (max 2048 chars) */
    char     filepath[IPC_PATH_MAX]; /* null-terminated saved-copy path (max 512 chars) */
} CrawlMsg;

/*
 * IPC_SENTINEL_DOCID — special docid value meaning "scanning is done".
 * The crawler sends one final CrawlMsg with docid = UINT32_MAX (4294967295)
 * to tell the indexer there are no more files coming.
 * The indexer sees this and flushes the index to disk then exits.
 */
#define IPC_SENTINEL_DOCID UINT32_MAX

#endif /* IPC_H */
