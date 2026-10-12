#ifndef INDEXER_H
#define INDEXER_H

/*
 * indexer.h — declares all data structures and functions for building
 * the inverted index. The indexer reads the saved text files, extracts
 * words, and builds a lookup table: word → list of pages containing it.
 */

#include <stdint.h>  /* for uint32_t */

/*
 * INDEX_BUCKETS — number of slots in the hash table.
 * Must be a power of 2 so we can use fast bitwise AND instead of division.
 * 131072 buckets means words are spread across 131072 slots.
 * More buckets = fewer collisions = faster lookups.
 */
#define INDEX_BUCKETS 131072

/*
 * Posting — one entry in a word's postings list.
 * Represents ONE page that contains this word.
 * Multiple postings form a linked list for each word.
 *
 * Example: word "linux" appears in pages 0, 4, 31, 32...
 *   postings: 0 → 4 → 31 → 32 → NULL
 */
typedef struct Posting {
    uint32_t        docid; /* the unique ID of the page containing this word */
    struct Posting *next;  /* pointer to next posting in the list (or NULL) */
} Posting;

/*
 * IndexEntry — one entry in the hash table.
 * Represents ONE word and ALL the pages that contain it.
 * Multiple IndexEntries in the same bucket form a linked list (hash chaining).
 */
typedef struct IndexEntry {
    char              *term;     /* heap-allocated word string (e.g. "linux") */
    Posting           *postings; /* linked list of all pages containing this word */
    int                df;       /* document frequency — how many pages contain this word */
    struct IndexEntry *next;     /* next entry in this bucket's chain (or NULL) */
} IndexEntry;

/*
 * InvertedIndex — the full in-memory index.
 * A hash table with INDEX_BUCKETS slots.
 * Each slot holds a linked list of IndexEntry structs.
 * Built in memory while indexing, then flushed to disk when done.
 */
typedef struct {
    IndexEntry *buckets[INDEX_BUCKETS]; /* array of 131072 linked list heads */
    int         num_terms;              /* total number of unique words indexed */
} InvertedIndex;

/*
 * index_create — creates a new empty inverted index.
 * All bucket pointers start as NULL (empty).
 * Returns a heap-allocated InvertedIndex pointer.
 */
InvertedIndex *index_create(void);

/*
 * index_destroy — frees all memory used by the index.
 * Frees every posting, every index entry, every word string.
 * Called once at program shutdown after write_index().
 */
void index_destroy(InvertedIndex *idx);

/*
 * index_insert — adds a word→page relationship to the index.
 * If the word doesn't exist yet → creates a new IndexEntry.
 * If the word exists → adds docid to its postings list.
 * Prevents duplicate docids — same word appearing twice in one page
 * only gets one posting for that page.
 *
 * term  — the word to insert (e.g. "linux")
 * docid — the page ID where this word was found
 */
void index_insert(InvertedIndex *idx, const char *term, uint32_t docid);

/*
 * TokenCallback — function pointer type for the tokenizer.
 * tokenize() calls this function for each valid word it finds.
 * word      — the lowercase word found (e.g. "linux")
 * user_data — extra data passed through from the caller
 */
typedef void (*TokenCallback)(const char *word, void *user_data);

/*
 * tokenize — extracts words from plain text and calls callback for each.
 *
 * Steps:
 *   1. Extract alphabetic words
 *   2. Convert to lowercase
 *   3. Skip words shorter than 3 characters
 *   4. Skip stop words (common words like "the", "and", "for")
 *   5. Call callback(word, user_data) for each valid word
 *
 * text      — the full text content of a file
 * callback  — function called for each valid word found
 * user_data — passed through to callback unchanged
 */
void tokenize(const char *text, TokenCallback callback, void *user_data);

/*
 * is_stopword — checks if a word is a common stop word.
 * Stop words are very common words that don't help with searching.
 * Examples: "the", "and", "for", "are", "but"
 * Returns 1 if it IS a stop word (skip it).
 * Returns 0 if it is NOT a stop word (keep it).
 */
int is_stopword(const char *word);

/*
 * write_index — saves the in-memory index to disk.
 * Creates two files in index_dir:
 *
 *   dict.tsv     — one line per word: "word TAB byte_offset TAB df"
 *                  byte_offset = where to find this word's postings in postings.bin
 *                  df = how many pages contain this word
 *
 *   postings.bin — raw binary file of uint32_t docids
 *                  each word's postings are stored consecutively
 *                  the query tool seeks to byte_offset and reads df docids
 *
 * Called once after all pages have been indexed (when sentinel received).
 */
void write_index(InvertedIndex *idx, const char *index_dir);

#endif /* INDEXER_H */
