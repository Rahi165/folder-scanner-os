#include "threadpool.h"      /* our own header — WorkerArgs, ThreadPool structs */
#include "fetch.h"           /* for fetch_file() — reads a file from disk */
#include "parse.h"           /* for list_dir(), has_extension() — looks inside folders */
#include "../common/log.h"   /* for LOG_INFO, LOG_ERROR — logging */
#include <stdio.h>           /* for fopen, fwrite, fclose, snprintf */
#include <stdlib.h>          /* for malloc, free */
#include <string.h>          /* for memset, strncpy, strerror */
#include <errno.h>           /* for errno */
#include <unistd.h>          /* for POSIX file operations */
#include <sys/stat.h>        /* for lstat, S_ISDIR, S_ISREG */
#include <sys/socket.h>      /* for send() — sending data over socket */

/*
 * LinkCtx — context passed to the entry callback function.
 * When list_dir finds an entry, it calls entry_callback with this struct.
 */
typedef struct {
    WorkerArgs *args;       /* shared worker arguments (queue, visited set, etc.) */
    int         dir_depth;  /* depth of the folder being listed */
} LinkCtx;

/*
 * bump — adds delta to one of the shared counters while holding counter_lock.
 * Every counter is shared by all threads, so it must always be changed under the lock.
 */
static void bump(WorkerArgs *args, int *counter, int delta) {
    pthread_mutex_lock(args->counter_lock);
    *counter += delta;
    pthread_mutex_unlock(args->counter_lock);
}

/* process_path is defined below; entry_callback needs to call it */
static int process_path(WorkerArgs *args, char *path, int depth);

/*
 * entry_callback — called by list_dir() for each file or subfolder it finds.
 * This is where new work gets added to the queue for other threads.
 *
 * Steps:
 *   1. Subfolders deeper than --max-depth are dropped
 *   2. Check if the path was already seen → skip if so
 *   3. Try to add the path to the queue for another worker
 *   4. If the queue is FULL, handle the path right here instead of waiting.
 *      Why? If every worker waited for a free slot, nobody would be left to
 *      take items out of the queue → deadlock. Doing the work ourselves
 *      guarantees that at least one thread always keeps making progress.
 *
 * Depth rule: files directly inside the root folder have depth 0.
 * Each subfolder level adds 1. A file has the same depth as its folder.
 *
 * path      — the full path found (we own this, must free it)
 * is_dir    — 1 for a subfolder, 0 for a regular file
 * user_data — our LinkCtx struct cast to void*
 */
static void entry_callback(char *path, int is_dir, void *user_data) {
    LinkCtx *ctx   = user_data;
    int      depth = is_dir ? ctx->dir_depth + 1 : ctx->dir_depth;

    /* Skip subfolders that are deeper than --max-depth allows */
    if (is_dir && depth > ctx->args->max_depth) {
        free(path);
        return;
    }

    /* Check if path was already visited — returns 0 if NEW, non-zero if SEEN */
    if (visited_check_and_insert(ctx->args->visited, path) != 0) {
        free(path);
        return;
    }

    /* path is new — try to hand it to the queue (copies it via strdup) */
    int rc = queue_try_push(ctx->args->url_queue, path, depth);
    if (rc == 1) {
        /* queue is full: do the work ourselves (process_path frees path) */
        process_path(ctx->args, path, depth);
        return;
    }
    free(path); /* pushed (0) or shut down (-1): the queue made its own copy */
}

/*
 * save_page — saves a copy of the file to disk.
 * Creates a file at <out_dir>/pages/<docid>.txt
 *
 * Returns  0 on success.
 * Returns -1 if the file could not be written.
 */
static int save_page(const char *out_dir, int docid, const FetchResult *res) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/pages/%d.txt", out_dir, docid);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        LOG_ERROR("Cannot open %s for writing: %s", path, strerror(errno));
        return -1;
    }

    size_t wrote = fwrite(res->data, 1, res->size, fp); /* write the content */
    fclose(fp);
    return wrote == res->size ? 0 : -1;
}

/*
 * send_ipc_msg — sends a CrawlMsg to the indexer over the UNIX socket.
 * Uses ipc_lock to ensure only ONE thread sends at a time.
 * Without the lock, two threads sending simultaneously would mix up messages.
 *
 * Uses a loop to handle partial sends — send() may not send all bytes at once.
 *
 * Returns  0 on success.
 * Returns -1 if send() failed.
 */
static int send_ipc_msg(int sockfd, const CrawlMsg *msg,
                        pthread_mutex_t *lock) {
    pthread_mutex_lock(lock); /* lock — only this thread can send now */

    ssize_t sent  = 0;
    ssize_t total = (ssize_t)sizeof(CrawlMsg); /* total bytes to send */
    const char *buf = (const char *)msg;        /* treat struct as byte array */

    /* loop until all bytes are sent (send() may send partial data) */
    while (sent < total) {
        ssize_t n = send(sockfd, buf + sent, (size_t)(total - sent), 0);
        if (n <= 0) {
            pthread_mutex_unlock(lock); /* always unlock before returning */
            return -1; /* send failed */
        }
        sent += n; /* move forward by bytes sent */
    }

    pthread_mutex_unlock(lock); /* unlock — other threads can send now */
    return 0; /* success */
}

/*
 * process_path — handles ONE path taken from the queue (or found while the queue was full).
 *   - if it is a FOLDER: list its entries and queue them (see entry_callback)
 *   - if it is a FILE:   read it → save a copy → send its metadata to the indexer
 *
 * Takes ownership of path and always frees it.
 * Returns 1 if the worker should stop (file limit reached), otherwise 0.
 */
static int process_path(WorkerArgs *args, char *path, int depth) {

    /* ── Step 1: What kind of thing is this path? ── */
    struct stat st;
    if (lstat(path, &st) != 0) {
        LOG_WARN("cannot stat %s: %s", path, strerror(errno));
        bump(args, args->pages_failed, 1);
        free(path);
        return 0;
    }

    /* ── Step 2a: A folder — list it and queue everything inside ── */
    if (S_ISDIR(st.st_mode)) {
        LOG_INFO("scanning folder depth=%d %s", depth, path);
        LinkCtx ctx = { args, depth };
        if (list_dir(path, entry_callback, &ctx) < 0)
            bump(args, args->pages_failed, 1);
        free(path);
        return 0; /* folders are not documents */
    }

    /* ── Step 2b: A file — apply the filters ── */
    if (!S_ISREG(st.st_mode) ||
        !has_extension(path, args->ext) ||
        st.st_size > args->max_size) {
        bump(args, args->pages_skipped, 1); /* wrong type, wrong extension or too big */
        free(path);
        return 0;
    }

    /* ── Step 3: Check if we've hit the file limit ── */
    pthread_mutex_lock(args->counter_lock); /* lock before reading/writing counters */

    if (*args->pages_fetched >= args->max_pages) {
        /* limit reached — skip this file and shut everything down */
        (*args->pages_skipped)++;
        pthread_mutex_unlock(args->counter_lock);
        queue_shutdown(args->url_queue);     /* wake up all other waiting threads */
        free(path);
        return 1;
    }

    /* reserve a unique docid for this file */
    int docid = (*args->next_docid)++; /* get current value then increment */
    (*args->pages_fetched)++;          /* count this file as fetched */
    pthread_mutex_unlock(args->counter_lock);

    /* ── Step 4: Read the file from disk ── */
    FetchResult result;
    if (fetch_file(path, &result) < 0) {
        /* read failed — give back the reserved slot and count as failed */
        pthread_mutex_lock(args->counter_lock);
        (*args->pages_fetched)--;
        (*args->pages_failed)++;
        pthread_mutex_unlock(args->counter_lock);
        free(path);
        return 0;
    }

    /* ── Step 5: Save a copy under <out>/pages/<docid>.txt ── */
    if (save_page(args->out_dir, docid, &result) < 0) {
        free(result.data);
        pthread_mutex_lock(args->counter_lock);
        (*args->pages_fetched)--;
        (*args->pages_failed)++;
        pthread_mutex_unlock(args->counter_lock);
        free(path);
        return 0;
    }
    free(result.data); /* done with the content — free it */

    LOG_INFO("read depth=%d docid=%d %s", depth, docid, path);

    /* ── Step 6: Send metadata to the indexer via IPC ── */
    CrawlMsg msg;
    memset(&msg, 0, sizeof(msg)); /* zero out all fields first */
    msg.docid = (uint32_t)docid;
    msg.depth = (uint32_t)depth;
    snprintf(msg.filepath, IPC_PATH_MAX, "%s/pages/%d.txt", args->out_dir, docid);
    strncpy(msg.src_path, path, IPC_SRC_MAX - 1);

    /* send the message to the indexer — protected by ipc_lock */
    if (send_ipc_msg(args->ipc_fd, &msg, args->ipc_lock) < 0)
        LOG_ERROR("IPC send failed for docid=%d", docid);

    free(path);
    return 0;
}

/*
 * worker_thread — the main function each worker thread runs.
 * Each worker loops: pop a path from the queue and process it.
 * It stops when queue_pop returns -1 (no more work, or shutdown requested)
 * or when process_path reports that the file limit was reached.
 *
 * arg — pointer to the shared WorkerArgs struct (same for all threads)
 */
void *worker_thread(void *arg) {
    WorkerArgs *args = arg; /* cast void* back to WorkerArgs* */

    char *path  = NULL; /* will hold the path popped from the queue */
    int   depth = 0;    /* will hold the depth of that path */

    while (queue_pop(args->url_queue, &path, &depth) == 0) {
        int stop = process_path(args, path, depth); /* takes ownership of path */
        path = NULL;
        if (stop) break;
    }
    return NULL; /* thread exits */
}

/*
 * threadpool_create — creates N worker threads and starts them all running.
 * Allocates the ThreadPool struct and the array of thread IDs.
 * Calls pthread_create for each thread, passing the shared WorkerArgs.
 *
 * n    — number of worker threads to create
 * args — the shared WorkerArgs all threads will use
 */
ThreadPool *threadpool_create(int n, WorkerArgs args) {
    ThreadPool *tp  = malloc(sizeof(ThreadPool));  /* allocate the thread pool */
    tp->threads     = malloc(sizeof(pthread_t) * (size_t)n); /* allocate thread ID array */
    tp->num_threads = n;    /* store number of threads */
    tp->args        = args; /* store the shared arguments */

    /* create each worker thread */
    for (int i = 0; i < n; i++)
        pthread_create(&tp->threads[i], NULL, worker_thread, &tp->args);

    return tp; /* return the pool — caller uses threadpool_wait_and_destroy later */
}

/*
 * threadpool_wait_and_destroy — waits for all threads to finish then frees memory.
 * pthread_join blocks until thread i has exited.
 * Must wait for ALL threads before freeing shared data (queue, visited set, etc.)
 */
void threadpool_wait_and_destroy(ThreadPool *tp) {
    /* wait for each thread to finish */
    for (int i = 0; i < tp->num_threads; i++)
        pthread_join(tp->threads[i], NULL); /* block until thread i exits */

    free(tp->threads); /* free the thread ID array */
    free(tp);          /* free the thread pool struct */
}
