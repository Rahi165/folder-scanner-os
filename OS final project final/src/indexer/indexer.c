#include "indexer.h"        /* our own header — all structs and declarations */
#include "../common/log.h"  /* for LOG_INFO, LOG_ERROR — logging */
#include <stdio.h>          /* for FILE, fopen, fwrite, fclose, fprintf */
#include <stdlib.h>         /* for malloc, calloc, free, strdup */
#include <string.h>         /* for strcmp, strdup */
#include <ctype.h>          /* for isalpha, tolower — character checking */

/*
 * hash_term — converts a word string into a bucket number.
 * Uses the djb2 algorithm (same as visited.c).
 * Starting value 5381, then for each character: h = h * 33 + character.
 * Returns a large number that gets mapped to 0..131071 using bitwise AND.
 */
static unsigned long hash_term(const char *s) {
    unsigned long h = 5381; /* djb2 starting value */
    int c;
    /* process each character of the word */
    while ((c = (unsigned char)*s++))
        h = ((h << 5) + h) + (unsigned long)c; /* h = h * 33 + c */
    return h;
}

/*
 * STOPWORDS — list of common words that are NOT useful for searching.
 * These words appear in almost every page so they don't help find specific pages.
 * Words in this list are ignored during tokenization.
 * NULL marks the end of the list.
 */
static const char *STOPWORDS[] = {
    "the","and","for","are","but","not","you","all",
    "can","had","her","was","one","our","out","his",
    "that","this","with","have","from","they","will",
    "been","were","said","each","which","their","time",
    NULL /* end marker */
};

/*
 * is_stopword — checks if a word should be ignored.
 * Compares word against every entry in STOPWORDS list.
 * Returns 1 if word IS a stop word → skip it.
 * Returns 0 if word is NOT a stop word → keep it.
 */
int is_stopword(const char *word) {
    for (int i = 0; STOPWORDS[i]; i++)
        if (strcmp(word, STOPWORDS[i]) == 0) return 1; /* found in list */
    return 0; /* not a stop word */
}

/*
 * index_create — creates a new empty inverted index.
 * calloc zeros all memory so all 131072 bucket pointers start as NULL.
 * Returns a heap-allocated InvertedIndex pointer.
 */
InvertedIndex *index_create(void) {
    InvertedIndex *idx = calloc(1, sizeof(InvertedIndex)); /* allocate + zero */
    return idx;
}

/*
 * index_destroy — frees all memory used by the index.
 * Loops through all 131072 buckets.
 * For each bucket, walks the linked list of IndexEntries.
 * For each IndexEntry, walks the linked list of Postings.
 * Frees everything in reverse order (postings → entries → index).
 */
void index_destroy(InvertedIndex *idx) {
    /* loop through every bucket */
    for (int b = 0; b < INDEX_BUCKETS; b++) {
        IndexEntry *e = idx->buckets[b]; /* head of this bucket's chain */
        while (e) {
            IndexEntry *enext = e->next;  /* save next entry before freeing */

            /* free all postings for this entry */
            Posting *p = e->postings;
            while (p) {
                Posting *pnext = p->next; /* save next posting before freeing */
                free(p);                  /* free the posting node */
                p = pnext;                /* move to next posting */
            }

            free(e->term); /* free the word string */
            free(e);       /* free the index entry itself */
            e = enext;     /* move to next entry in bucket */
        }
    }
    free(idx); /* free the index itself */
}

/*
 * index_insert — adds a word→page relationship to the index.
 *
 * Step 1: Hash the word to find which bucket it belongs to
 * Step 2: Search that bucket's linked list for the word
 *   - Found → use existing IndexEntry
 *   - Not found → create new IndexEntry and add to bucket
 * Step 3: Check if this docid is already in the postings list
 *   - Already there → skip (no duplicates)
 *   - Not there → add new Posting node to the front of the list
 *
 * Example: index_insert(idx, "linux", 5)
 *   → finds or creates IndexEntry for "linux"
 *   → adds docid=5 to its postings list
 *   → increments df (document frequency)
 */
void index_insert(InvertedIndex *idx, const char *term, uint32_t docid) {
    /* Step 1: find which bucket this word belongs to */
    /* & (INDEX_BUCKETS - 1) is fast way to do % 131072 */
    unsigned long bkt = hash_term(term) & (INDEX_BUCKETS - 1);

    /* Step 2: search the bucket's linked list for this word */
    IndexEntry *e = idx->buckets[bkt];
    while (e) {
        if (strcmp(e->term, term) == 0) break; /* found the word */
        e = e->next;                            /* try next entry */
    }

    /* if word not found → create a new IndexEntry */
    if (!e) {
        e           = calloc(1, sizeof(IndexEntry)); /* allocate new entry */
        e->term     = strdup(term);                  /* copy the word string */
        e->next     = idx->buckets[bkt];             /* point to old head */
        idx->buckets[bkt] = e;                       /* new entry is now head */
        idx->num_terms++;                            /* one more unique word */
    }

    /* Step 3: check for duplicate docid — don't add same page twice */
    for (Posting *p = e->postings; p; p = p->next)
        if (p->docid == docid) return; /* already have this page — skip */

    /* docid not in list → add new Posting to the front */
    Posting *node = malloc(sizeof(Posting)); /* allocate new posting */
    node->docid   = docid;                   /* set the page ID */
    node->next    = e->postings;             /* point to old head */
    e->postings   = node;                    /* new posting is now head */
    e->df++;                                 /* one more page contains this word */
}

/*
 * tokenize — extracts words from plain text and calls callback for each valid word.
 *
 * How it works:
 *   - Scans through the text character by character
 *   - Builds words from alphabetic characters
 *   - Converts each character to lowercase
 *   - When a non-alphabetic character is found → end of word
 *   - Filters out words shorter than 3 characters (not useful)
 *   - Filters out stop words (too common to be useful)
 *   - Calls callback for each valid word
 *
 * Example input: "Linux Kernel threads"
 *   → finds "Linux" → lowercase "linux" → callback("linux")
 *   → finds "Kernel" → lowercase "kernel" → callback("kernel")
 *   → finds "threads" → callback("threads")
 */
void tokenize(const char *text, TokenCallback callback, void *user_data) {
    char word[256]; /* buffer to build current word */
    int  wlen = 0;  /* current word length */

    for (const char *p = text; *p; p++) {
        if (isalpha((unsigned char)*p)) {
            /* alphabetic character — add to current word (lowercase) */
            if (wlen < (int)sizeof(word) - 1)
                word[wlen++] = (char)tolower((unsigned char)*p);
        } else {
            /* non-alphabetic character — end of current word */
            if (wlen >= 3) {
                /* word is long enough — check if it's a stop word */
                word[wlen] = '\0'; /* null-terminate the word */
                if (!is_stopword(word))
                    callback(word, user_data); /* valid word — call callback */
            }
            wlen = 0; /* reset for next word */
        }
    }

    /* handle last word in text (no trailing non-alpha character) */
    if (wlen >= 3) {
        word[wlen] = '\0';
        if (!is_stopword(word)) callback(word, user_data);
    }
}

/*
 * write_index — saves the in-memory index to two files on disk.
 *
 * File 1: dict.tsv — the dictionary
 *   Format: "word TAB byte_offset TAB df\n"
 *   Example: "linux\t1234\t17\n"
 *   → word "linux" has 17 pages, postings start at byte 1234 in postings.bin
 *
 * File 2: postings.bin — the postings lists
 *   Format: raw binary uint32_t values concatenated together
 *   Example: [5][0][4][31][32]... (page IDs as 4-byte integers)
 *   The query tool seeks to byte_offset and reads df uint32_t values
 *
 * offset tracks current position in postings.bin as we write each posting.
 * This offset is saved in dict.tsv so the query tool can jump directly to
 * the right position without reading the whole file.
 */
void write_index(InvertedIndex *idx, const char *index_dir) {
    char dict_path[512], post_path[512];

    /* build file paths */
    snprintf(dict_path, sizeof(dict_path), "%s/dict.tsv",     index_dir);
    snprintf(post_path, sizeof(post_path), "%s/postings.bin", index_dir);

    /* open both files */
    FILE *dict_fp = fopen(dict_path, "w");   /* text file for dictionary */
    FILE *post_fp = fopen(post_path, "wb");  /* binary file for postings */

    if (!dict_fp || !post_fp) {
        LOG_ERROR("Cannot open index files in %s", index_dir);
        if (dict_fp) fclose(dict_fp);
        if (post_fp) fclose(post_fp);
        return;
    }

    long offset = 0; /* tracks current byte position in postings.bin */

    /* loop through all 131072 buckets */
    for (int i = 0; i < INDEX_BUCKETS; i++) {
        /* loop through all entries in this bucket's linked list */
        for (IndexEntry *e = idx->buckets[i]; e; e = e->next) {

            /* write one line to dict.tsv: word TAB offset TAB df */
            fprintf(dict_fp, "%s\t%ld\t%d\n", e->term, offset, e->df);

            /* write all docids for this word to postings.bin */
            for (Posting *p = e->postings; p; p = p->next) {
                fwrite(&p->docid, sizeof(uint32_t), 1, post_fp);
                offset += (long)sizeof(uint32_t); /* advance offset by 4 bytes */
            }
        }
    }

    fclose(dict_fp); /* close dictionary file */
    fclose(post_fp); /* close postings file */

    /* log how many terms were written */
    LOG_INFO("Index flushed: %d terms to %s", idx->num_terms, index_dir);
}
