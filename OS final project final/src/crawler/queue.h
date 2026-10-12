#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>

/*
 * URLQueue — bounded, thread-safe ring buffer of (url, depth) pairs.
 *
 * Design:
 *  - Mutex + two condition variables (not_full, not_empty) for backpressure.
 *  - idle_workers counter: when idle_workers == num_workers AND count == 0,
 *    the frontier is exhausted and the queue shuts itself down.
 *  - queue_shutdown() wakes all blocked threads for a forced stop.
 */

/* One item stored in the queue — a URL and how deep it was found */
typedef struct {
    char *url;    /* heap-allocated URL string — caller frees after queue_pop */
    int   depth;  /* crawl depth of this URL (0 = seed, 1 = one click away, etc.) */
} QueueItem;

/* The main queue structure — holds all URLs waiting to be crawled */
typedef struct {
    QueueItem      *items;          /* array of QueueItems (ring buffer) */
    int             capacity;       /* maximum number of URLs the queue can hold */
    int             head;           /* index of the next item to pop (front of line) */
    int             tail;           /* index where next item will be pushed (back of line) */
    int             count;          /* current number of URLs in the queue */
    int             max_count_ever; /* highest count ever seen (used in crawl summary) */
    int             num_workers;    /* total number of worker threads using this queue */
    int             idle_workers;   /* number of threads currently waiting for a URL */
    int             shutdown;       /* set to 1 to stop all threads */
    pthread_mutex_t lock;           /* mutex — only one thread can modify queue at a time */
    pthread_cond_t  not_full;       /* condition variable — signals when queue has space */
    pthread_cond_t  not_empty;      /* condition variable — signals when queue has items */
} URLQueue;

/*
 * queue_create — creates and initializes a new empty queue.
 * capacity    — maximum number of URLs the queue can hold at once.
 * num_workers — number of worker threads that will call queue_pop.
 * Returns a heap-allocated URLQueue pointer.
 */
URLQueue *queue_create(int capacity, int num_workers);

/*
 * queue_destroy — frees all memory used by the queue.
 * Frees any remaining URLs still in the queue before destroying.
 */
void queue_destroy(URLQueue *q);

/*
 * queue_push — adds a URL to the back of the queue.
 * Blocks (waits) if the queue is full until space becomes available.
 * Returns  0 on success.
 * Returns -1 if the queue is shut down.
 */
int queue_push(URLQueue *q, const char *url, int depth);

/*
 * queue_try_push — like queue_push, but NEVER blocks.
 * Returns  0 if the item was added.
 * Returns  1 if the queue is full (nothing was added — caller keeps the item).
 * Returns -1 if the queue is shut down.
 * Workers use this one: if every worker blocked in queue_push at the same
 * time, nobody would be left to pop and the program would deadlock.
 */
int queue_try_push(URLQueue *q, const char *url, int depth);

/*
 * queue_pop — removes and returns a URL from the front of the queue.
 * Blocks (waits) if the queue is empty until a URL is available.
 * Sets *url_out (caller must free) and *depth_out on success.
 * Returns  0 on success.
 * Returns -1 if the queue is shut down and empty (no more work).
 */
int queue_pop(URLQueue *q, char **url_out, int *depth_out);

/*
 * queue_shutdown — signals all threads to stop.
 * Sets shutdown = 1 and wakes up ALL waiting threads so they can exit.
 * Called when max pages is reached or crawl is complete.
 */
void queue_shutdown(URLQueue *q);

/*
 * queue_max_size — returns the highest number of items ever in the queue.
 * Used in the crawl summary to show "Max queue depth".
 */
int queue_max_size(URLQueue *q);

#endif /* QUEUE_H */
