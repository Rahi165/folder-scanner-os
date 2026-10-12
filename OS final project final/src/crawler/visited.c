#include "visited.h"
#include <stdlib.h>
#include <string.h>

/*
 * hash_url — converts a URL string into a number (hash).
 * Uses the djb2 algorithm — a simple and fast hashing algorithm.
 * The hash determines which bucket (slot) the URL goes into.
 *
 * How djb2 works:
 *   - Starts with h = 5381 (a magic number that works well)
 *   - For each character: h = h * 33 + character
 *   - Returns the final number
 *
 * Example: "https://wikipedia.org" → some large number like 1234567890
 * That number % 65536 = the bucket index (0 to 65535)
 */
static unsigned long hash_url(const char *url) {
    unsigned long h = 5381; /* starting value for djb2 hash */
    int c;
    /* process each character of the URL one by one */
    while ((c = (unsigned char)*url++))
        h = ((h << 5) + h) + (unsigned long)c; /* h = h * 33 + c */
    return h; /* return the final hash number */
}

/*
 * visited_create — creates a new empty visited set.
 * calloc zeros all memory so all 65536 bucket pointers start as NULL.
 * Initializes the mutex for thread safety.
 */
VisitedSet *visited_create(void) {
    VisitedSet *vs = calloc(1, sizeof(VisitedSet)); /* allocate + zero all buckets */
    pthread_mutex_init(&vs->lock, NULL);             /* initialize the mutex */
    return vs;
}

/*
 * visited_destroy — frees all memory used by the visited set.
 * Goes through all 65536 buckets and frees every node's URL string
 * and every node itself. Then destroys the mutex and frees the set.
 */
void visited_destroy(VisitedSet *vs) {
    /* loop through every bucket */
    for (int i = 0; i < VISITED_BUCKETS; i++) {
        VisitedNode *n = vs->buckets[i]; /* get the head of this bucket's chain */
        /* walk the linked list and free each node */
        while (n) {
            VisitedNode *tmp = n->next; /* save pointer to next node */
            free(n->url);               /* free the URL string */
            free(n);                    /* free the node itself */
            n = tmp;                    /* move to next node */
        }
    }
    pthread_mutex_destroy(&vs->lock); /* destroy the mutex */
    free(vs);                          /* free the set itself */
}

/*
 * visited_check_and_insert — checks if URL was seen and inserts if not.
 *
 * Step 1: Hash the URL to find which bucket to look in
 * Step 2: Lock the mutex (so no other thread can interfere)
 * Step 3: Search the bucket's linked list for this URL
 *   - If found → unlock and return 1 (already visited, skip it)
 *   - If not found → create a new node, add to front of list, return 0
 * Step 4: Unlock the mutex
 *
 * WHY one lock for both check AND insert?
 * If we used two separate locks (one to check, one to insert),
 * two threads could both check "is this URL visited?" at the same time,
 * both get "no", and both start crawling the same page — that's a bug
 * called a TOCTOU (Time Of Check To Time Of Use) race condition.
 * One lock for both operations prevents this completely.
 */
int visited_check_and_insert(VisitedSet *vs, const char *url) {
    /* Step 1: find which bucket this URL belongs to */
    /* & (VISITED_BUCKETS - 1) is a fast way to do % 65536 */
    unsigned long idx = hash_url(url) & (VISITED_BUCKETS - 1);

    /* Step 2: lock — only this thread can check/insert now */
    pthread_mutex_lock(&vs->lock);

    /* Step 3: search the linked list in this bucket for the URL */
    for (VisitedNode *n = vs->buckets[idx]; n; n = n->next) {
        if (strcmp(n->url, url) == 0) {
            /* URL found — already visited */
            pthread_mutex_unlock(&vs->lock);
            return 1; /* tell caller to SKIP this URL */
        }
    }

    /* URL not found — insert it as a new node at the front of the list */
    VisitedNode *node = malloc(sizeof(VisitedNode)); /* allocate new node */
    node->url        = strdup(url);                  /* copy the URL string */
    node->next       = vs->buckets[idx];             /* point to old head */
    vs->buckets[idx] = node;                         /* new node is now the head */
    vs->count++;                                     /* increment total URL count */

    /* Step 4: unlock — other threads can access the set now */
    pthread_mutex_unlock(&vs->lock);
    return 0; /* tell caller this URL is NEW — go crawl it */
}
