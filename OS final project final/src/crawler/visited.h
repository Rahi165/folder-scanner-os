#ifndef VISITED_H
#define VISITED_H

#include <pthread.h>

/* Number of buckets in the hash table — must be a power of 2 */
/* 65536 buckets means URLs are spread across 65536 slots */
/* More buckets = fewer collisions = faster lookups */
#define VISITED_BUCKETS 65536

/*
 * VisitedNode — one entry in the hash table.
 * Each bucket holds a linked list of VisitedNodes.
 * If two URLs hash to the same bucket, they form a chain.
 */
typedef struct VisitedNode {
    char               *url;   /* heap-allocated URL string */
    struct VisitedNode *next;  /* pointer to next node in the chain (or NULL) */
} VisitedNode;

/*
 * VisitedSet — the full hash table that tracks all visited URLs.
 * Uses chaining (linked lists) to handle collisions.
 */
typedef struct {
    VisitedNode    *buckets[VISITED_BUCKETS]; /* array of 65536 linked list heads */
    pthread_mutex_t lock;                     /* mutex — makes check+insert atomic */
    int             count;                    /* total number of URLs seen so far */
} VisitedSet;

/*
 * visited_create — creates and initializes a new empty visited set.
 * Returns a heap-allocated VisitedSet pointer.
 * Called once at startup before any threads are created.
 */
VisitedSet *visited_create(void);

/*
 * visited_destroy — frees all memory used by the visited set.
 * Frees every URL string and every node in every bucket's linked list.
 * Called once at the end of the program during cleanup.
 */
void visited_destroy(VisitedSet *vs);

/*
 * visited_check_and_insert — the most important function.
 * Atomically checks if a URL has been seen AND inserts it if not.
 * Both operations happen under ONE lock acquisition — this prevents
 * two threads from crawling the same URL at the same time (TOCTOU bug).
 *
 * Returns 1 if the URL was ALREADY seen → caller should SKIP it.
 * Returns 0 if the URL is NEW and inserted → caller should CRAWL it.
 */
int visited_check_and_insert(VisitedSet *vs, const char *url);

#endif /* VISITED_H */
