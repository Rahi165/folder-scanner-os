#include "queue.h"
#include <stdlib.h>
#include <string.h>

/*
 * queue_create — creates a new empty bounded queue.
 * Allocates memory for the queue and its items array.
 * Initializes the mutex and both condition variables.
 * capacity    — max number of URLs the queue can hold at once.
 * num_workers — number of worker threads that will call queue_pop.
 */
URLQueue *queue_create(int capacity, int num_workers) {
    URLQueue *q     = calloc(1, sizeof(URLQueue)); /* allocate queue, zero all fields */
    q->items        = calloc((size_t)capacity, sizeof(QueueItem)); /* allocate item array */
    q->capacity     = capacity;       /* set max size */
    q->num_workers  = num_workers;    /* remember how many threads use this queue */
    pthread_mutex_init(&q->lock,     NULL); /* initialize the mutex */
    pthread_cond_init(&q->not_full,  NULL); /* initialize "not full" signal */
    pthread_cond_init(&q->not_empty, NULL); /* initialize "not empty" signal */
    return q;
}

/*
 * queue_destroy — frees all memory used by the queue.
 * Frees any URLs still waiting in the queue (not yet crawled).
 * Destroys the mutex and condition variables.
 * Called once at the end of the program during cleanup.
 */
void queue_destroy(URLQueue *q) {
    /* Free any URLs still left in the queue */
    for (int i = 0; i < q->count; i++)
        free(q->items[(q->head + i) % q->capacity].url);
    free(q->items);                        /* free the items array */
    pthread_mutex_destroy(&q->lock);       /* destroy the mutex */
    pthread_cond_destroy(&q->not_full);    /* destroy "not full" condition */
    pthread_cond_destroy(&q->not_empty);   /* destroy "not empty" condition */
    free(q);                               /* free the queue itself */
}

/*
 * queue_push — adds a URL to the back of the queue.
 * If the queue is full, this thread BLOCKS and waits until space opens up.
 * This is called "backpressure" — it slows down producers when queue is full.
 * Once space is available, the URL is copied (strdup) and added.
 * Signals worker threads that a new URL is available.
 * Returns  0 on success.
 * Returns -1 if queue is shut down (caller should stop pushing).
 */
int queue_push(URLQueue *q, const char *url, int depth) {
    pthread_mutex_lock(&q->lock); /* lock — only this thread can touch the queue now */

    /* Wait while queue is full and not shut down */
    while (q->count == q->capacity && !q->shutdown)
        pthread_cond_wait(&q->not_full, &q->lock); /* sleep until "not full" signal */

    /* If shutdown happened while waiting, exit */
    if (q->shutdown) { pthread_mutex_unlock(&q->lock); return -1; }

    /* Add the new URL to the back of the queue (tail position) */
    q->items[q->tail].url   = strdup(url); /* copy the URL string onto the heap */
    q->items[q->tail].depth = depth;       /* store the depth */
    q->tail = (q->tail + 1) % q->capacity; /* move tail forward (wrap around if needed) */
    q->count++;                             /* one more item in the queue */

    /* Track the highest count ever seen (for crawl summary) */
    if (q->count > q->max_count_ever) q->max_count_ever = q->count;

    pthread_cond_signal(&q->not_empty);  /* wake up a waiting worker thread */
    pthread_mutex_unlock(&q->lock);      /* unlock — other threads can access queue now */
    return 0;
}

/*
 * queue_try_push — same as queue_push, but returns immediately instead of waiting.
 * Returns  0 if the item was added.
 * Returns  1 if the queue is full (the caller still owns the item and must handle it).
 * Returns -1 if the queue is shut down.
 */
int queue_try_push(URLQueue *q, const char *url, int depth) {
    pthread_mutex_lock(&q->lock);

    if (q->shutdown)             { pthread_mutex_unlock(&q->lock); return -1; }
    if (q->count == q->capacity) { pthread_mutex_unlock(&q->lock); return  1; }

    q->items[q->tail].url   = strdup(url);
    q->items[q->tail].depth = depth;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    if (q->count > q->max_count_ever) q->max_count_ever = q->count;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/*
 * queue_pop — removes and returns a URL from the front of the queue.
 * If the queue is empty, this thread BLOCKS and waits for a new URL.
 * Also checks if ALL workers are idle AND queue is empty — if so,
 * there are no more URLs to crawl and it triggers automatic shutdown.
 * Sets *url_out to the URL (caller must free it after use).
 * Sets *depth_out to the depth of that URL.
 * Returns  0 on success.
 * Returns -1 if queue is shut down and empty (no more work to do).
 */
int queue_pop(URLQueue *q, char **url_out, int *depth_out) {
    pthread_mutex_lock(&q->lock); /* lock — only this thread can touch queue now */
    q->idle_workers++;            /* mark this thread as idle (waiting for work) */

    /* Wait while queue is empty and not shut down */
    while (q->count == 0 && !q->shutdown) {
        /* If ALL workers are idle AND queue is empty → no more URLs will ever come */
        if (q->idle_workers == q->num_workers) {
            q->shutdown = 1;                          /* trigger shutdown */
            pthread_cond_broadcast(&q->not_empty);    /* wake ALL waiting threads */
            break;
        }
        pthread_cond_wait(&q->not_empty, &q->lock); /* sleep until "not empty" signal */
    }

    q->idle_workers--; /* this thread is no longer idle — it's about to get work */

    /* If queue is still empty after waking up → shutdown, return -1 */
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1; /* tell the worker thread to exit */
    }

    /* Take the URL from the front of the queue (head position) */
    QueueItem item = q->items[q->head];  /* copy the item */
    q->items[q->head].url = NULL;        /* clear the slot */
    q->head  = (q->head + 1) % q->capacity; /* move head forward (wrap if needed) */
    q->count--;                              /* one less item in the queue */

    pthread_cond_signal(&q->not_full);  /* wake up any thread waiting to push */
    pthread_mutex_unlock(&q->lock);     /* unlock — other threads can access queue now */

    *url_out   = item.url;   /* give the URL to the caller (caller must free it) */
    *depth_out = item.depth; /* give the depth to the caller */
    return 0;
}

/*
 * queue_shutdown — forces all threads to stop immediately.
 * Sets shutdown = 1 so all push/pop calls return -1.
 * Wakes up ALL threads waiting on either condition variable.
 * Called when --max-pages is reached or SIGINT (Ctrl+C) is received.
 */
void queue_shutdown(URLQueue *q) {
    pthread_mutex_lock(&q->lock);           /* lock before changing shutdown flag */
    q->shutdown = 1;                         /* signal all threads to stop */
    pthread_cond_broadcast(&q->not_full);    /* wake up all threads waiting to push */
    pthread_cond_broadcast(&q->not_empty);   /* wake up all threads waiting to pop */
    pthread_mutex_unlock(&q->lock);          /* unlock */
}

/*
 * queue_max_size — returns the highest number of items ever in the queue.
 * Used in the crawl summary line: "Max queue depth: 1748"
 */
int queue_max_size(URLQueue *q) {
    return q->max_count_ever;
}
