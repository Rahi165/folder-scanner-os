#include <getopt.h>    /* for getopt_long() — parses command line arguments */
#include <stdio.h>     /* for printf, fprintf, fopen, fgets, fseek, fread */
#include <stdlib.h>    /* for malloc, realloc, free, qsort, bsearch, exit */
#include <string.h>    /* for strcmp, strncpy, memset */
#include <stdint.h>    /* for uint32_t — 32-bit unsigned integer */
#include <ctype.h>     /* for tolower — convert characters to lowercase */
#include <sys/types.h> /* for system types */

#include "../common/ipc.h" /* for IPC_SRC_MAX, IPC_PATH_MAX — max string lengths */

/*
 * ══════════════════════════════════════════════════════════════════════
 * Dictionary — loads dict.tsv into memory for fast word lookups
 * ══════════════════════════════════════════════════════════════════════
 */

/* maximum length of a search term */
#define MAX_TERM_LEN 128

/*
 * DictEntry — one entry loaded from dict.tsv.
 * Represents one word and where to find its postings in postings.bin.
 */
typedef struct {
    char term[MAX_TERM_LEN]; /* the word (e.g. "linux") */
    long offset;             /* byte offset into postings.bin where docids start */
    int  df;                 /* document frequency — how many docids to read */
} DictEntry;

/* global array of all dictionary entries loaded from dict.tsv */
static DictEntry *dict_entries = NULL;
static int        dict_count   = 0;  /* how many entries are loaded */
static int        dict_cap     = 0;  /* current capacity of the array */

/*
 * cmp_dict_entry — comparison function for sorting and searching DictEntries.
 * Used by qsort() to sort entries alphabetically after loading.
 * Used by bsearch() to find a specific word quickly.
 * Returns negative if a < b, 0 if equal, positive if a > b.
 */
static int cmp_dict_entry(const void *a, const void *b) {
    return strcmp(((const DictEntry *)a)->term,
                  ((const DictEntry *)b)->term);
}

/*
 * load_dict — reads dict.tsv and loads all entries into memory.
 * Each line in dict.tsv has format: "word TAB byte_offset TAB df"
 * After loading, sorts all entries alphabetically for fast binary search.
 *
 * index_dir — the directory containing dict.tsv (e.g. "data/index")
 */
static void load_dict(const char *index_dir) {
    char path[512];
    /* build path to dict.tsv */
    snprintf(path, sizeof(path), "%s/dict.tsv", index_dir);

    FILE *f = fopen(path, "r"); /* open for reading */
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return; }

    char line[256];
    /* read one line at a time */
    while (fgets(line, sizeof(line), f)) {
        char term[MAX_TERM_LEN];
        long offset;
        int  df;

        /* parse the three fields from each line */
        if (sscanf(line, "%127s\t%ld\t%d", term, &offset, &df) != 3) continue;

        /* grow the array if needed */
        if (dict_count >= dict_cap) {
            dict_cap = dict_cap ? dict_cap * 2 : 65536; /* double each time */
            dict_entries = realloc(dict_entries,
                                   sizeof(DictEntry) * (size_t)dict_cap);
        }

        /* store the entry */
        strncpy(dict_entries[dict_count].term, term, MAX_TERM_LEN - 1);
        dict_entries[dict_count].term[MAX_TERM_LEN - 1] = '\0';
        dict_entries[dict_count].offset = offset; /* where postings are in bin file */
        dict_entries[dict_count].df     = df;     /* how many pages contain this word */
        dict_count++;
    }
    fclose(f);

    /* sort all entries alphabetically — needed for bsearch() to work */
    if (dict_count > 0)
        qsort(dict_entries, (size_t)dict_count, sizeof(DictEntry), cmp_dict_entry);
}

/*
 * dict_lookup — searches for a word in the dictionary using binary search.
 * Binary search is very fast — finds word in O(log n) steps.
 * For 9000 words: at most 14 comparisons to find any word.
 *
 * term — the word to search for (must be lowercase)
 * Returns pointer to DictEntry if found, NULL if word not in index.
 */
static const DictEntry *dict_lookup(const char *term) {
    if (!dict_entries || dict_count == 0) return NULL;

    /* create a key entry for bsearch */
    DictEntry key;
    strncpy(key.term, term, MAX_TERM_LEN - 1);
    key.term[MAX_TERM_LEN - 1] = '\0';

    /* binary search through sorted array */
    return bsearch(&key, dict_entries, (size_t)dict_count,
                   sizeof(DictEntry), cmp_dict_entry);
}

/*
 * ══════════════════════════════════════════════════════════════════════
 * Document map — maps docid → URL
 * Loaded from docs.tsv so we can print URLs in query results
 * ══════════════════════════════════════════════════════════════════════
 */

/* maximum number of documents we can handle */
#define MAX_DOCS 100000

/*
 * doc_urls — array mapping docid to URL.
 * doc_urls[5] = "https://en.wikipedia.org/wiki/Linux" means page 5 is that URL.
 * Loaded from docs.tsv at startup.
 */
static char doc_urls[MAX_DOCS][IPC_SRC_MAX];

/*
 * load_docs — reads docs.tsv and fills the doc_urls array.
 * Each line in docs.tsv has format: "docid TAB url TAB filepath TAB depth"
 * We only need docid and url for the query tool.
 *
 * index_dir — the directory containing docs.tsv
 */
static void load_docs(const char *index_dir) {
    char path[512];
    /* build path to docs.tsv */
    snprintf(path, sizeof(path), "%s/docs.tsv", index_dir);

    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return; }

    char line[IPC_SRC_MAX + IPC_PATH_MAX + 32];
    /* read one line at a time */
    while (fgets(line, sizeof(line), f)) {
        unsigned int docid, depth;
        char url[IPC_SRC_MAX], fp[IPC_PATH_MAX];

        /* parse docid and url from line */
        if (sscanf(line, "%u\t%2047[^\t]\t%511[^\t]\t%u",
                   &docid, url, fp, &depth) >= 2) {
            if (docid < MAX_DOCS)
                strncpy(doc_urls[docid], url, IPC_SRC_MAX - 1); /* store URL */
        }
    }
    fclose(f);
}

/*
 * ══════════════════════════════════════════════════════════════════════
 * Postings + AND intersection
 * ══════════════════════════════════════════════════════════════════════
 */

/*
 * load_postings — reads a word's postings list from postings.bin.
 * Seeks to the byte offset stored in dict.tsv and reads df uint32_t values.
 *
 * pf    — file pointer to postings.bin (already open)
 * entry — the DictEntry for this word (has offset and df)
 *
 * Returns heap-allocated array of docids, or NULL on failure.
 * Caller must free() the returned array.
 */
static uint32_t *load_postings(FILE *pf, const DictEntry *entry) {
    /* jump to the right position in postings.bin */
    if (fseek(pf, entry->offset, SEEK_SET) != 0) return NULL;

    /* allocate array to hold all docids for this word */
    uint32_t *list = malloc(sizeof(uint32_t) * (size_t)entry->df);
    if (!list) return NULL;

    /* read exactly df docids from the file */
    if ((int)fread(list, sizeof(uint32_t), (size_t)entry->df, pf) != entry->df) {
        free(list); return NULL; /* couldn't read all expected docids */
    }
    return list; /* caller must free() */
}

/*
 * cmp_uint32 — comparison function for sorting uint32_t arrays.
 * Used by qsort() before intersection to ensure arrays are sorted.
 * Returns negative if a < b, 0 if equal, positive if a > b.
 */
static int cmp_uint32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/*
 * intersect — finds docids that appear in BOTH arrays (AND operation).
 * Uses the two-pointer technique for O(m+n) time complexity.
 *
 * How two-pointer works:
 *   - Start with pointer i at start of array a, pointer j at start of array b
 *   - If a[i] == b[j] → both arrays have this docid → add to result, advance both
 *   - If a[i] < b[j]  → a is behind → advance i
 *   - If a[i] > b[j]  → b is behind → advance j
 *   - Stop when either pointer reaches the end
 *
 * Both arrays must be sorted first (we sort them before calling this).
 *
 * a, b    — the two sorted arrays of docids
 * na, nb  — lengths of the two arrays
 * out_n   — set to the number of results found
 *
 * Returns heap-allocated array of matching docids, caller must free().
 */
static uint32_t *intersect(uint32_t *a, int na,
                            uint32_t *b, int nb,
                            int *out_n) {
    /* sort both arrays (required for two-pointer to work) */
    qsort(a, (size_t)na, sizeof(uint32_t), cmp_uint32);
    qsort(b, (size_t)nb, sizeof(uint32_t), cmp_uint32);

    /* result can be at most min(na, nb) entries */
    uint32_t *result = malloc(sizeof(uint32_t) *
                              (size_t)(na < nb ? na : nb));

    int i = 0, j = 0, count = 0;

    /* two-pointer intersection */
    while (i < na && j < nb) {
        if (a[i] == b[j]) {
            /* both arrays have this docid — it's in the intersection */
            result[count++] = a[i];
            i++; j++; /* advance both pointers */
        } else if (a[i] < b[j]) {
            i++; /* a is behind — advance a */
        } else {
            j++; /* b is behind — advance b */
        }
    }

    *out_n = count; /* set number of results found */
    return result;  /* caller must free() */
}

/*
 * ══════════════════════════════════════════════════════════════════════
 * main — entry point of the query tool
 * ══════════════════════════════════════════════════════════════════════
 *
 * Steps:
 *   1. Parse --index flag and query terms from command line
 *   2. Load docs.tsv → doc_urls[] array
 *   3. Load dict.tsv → dict_entries[] array (sorted)
 *   4. Open postings.bin
 *   5. Look up first query term → get its postings list
 *   6. For each additional term → intersect with current result
 *   7. Print matching docids and URLs
 */
int main(int argc, char *argv[]) {
    char index_dir[512] = "data/index"; /* default index directory */
    int  term_start     = -1;           /* index into argv where terms start */

    /* define command line options */
    static struct option opts[] = {
        { "index", required_argument, 0, 'x' }, /* --index <dir> */
        { "help",  no_argument,       0, 'h' }, /* --help or -h */
        { 0, 0, 0, 0 }
    };

    int c;
    /* parse command line arguments */
    while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
        if (c == 'x') strncpy(index_dir, optarg, sizeof(index_dir) - 1);
        if (c == 'h') {
            /* print usage and exit cleanly */
            fprintf(stderr, "USAGE: ./query --index <dir> <term1> [term2 ...]\n");
            return 0;
        }
    }

    /* remaining arguments after the flags are the query terms */
    term_start = optind; /* optind points to first non-flag argument */

    /* check that at least one query term was provided */
    if (term_start >= argc) {
        fprintf(stderr, "USAGE: query --index <dir> <term1> [term2 ...]\n");
        return 1;
    }

    /* Step 2: load document map (docid → URL) from docs.tsv */
    memset(doc_urls, 0, sizeof(doc_urls)); /* clear the array first */
    load_docs(index_dir);

    /* Step 3: load dictionary from dict.tsv */
    load_dict(index_dir);

    /* Step 4: open postings.bin for random access reads */
    char post_path[1024];
    snprintf(post_path, sizeof(post_path), "%s/postings.bin", index_dir);
    FILE *pf = fopen(post_path, "rb"); /* rb = read binary */
    if (!pf) { fprintf(stderr, "Cannot open %s\n", post_path); return 1; }

    /* Step 5: look up the FIRST query term */
    char first[MAX_TERM_LEN];
    strncpy(first, argv[term_start], MAX_TERM_LEN - 1);
    first[MAX_TERM_LEN - 1] = '\0';

    /* convert to lowercase — index only stores lowercase words */
    for (int i = 0; first[i]; i++)
        first[i] = (char)tolower((unsigned char)first[i]);

    /* look up in dictionary */
    const DictEntry *e = dict_lookup(first);
    if (!e) {
        /* word not found in index at all */
        printf("No documents matched all query terms.\n");
        fclose(pf); return 0;
    }

    /* load the first term's postings list */
    int       n;
    uint32_t *result = load_postings(pf, e);
    if (!result) {
        printf("No documents matched all query terms.\n");
        fclose(pf); return 0;
    }
    n = e->df; /* number of docids for first term */

    /* Step 6: AND with each remaining query term */
    /* example: "linux kernel" → start with linux's pages, intersect with kernel's pages */
    for (int i = term_start + 1; i < argc && n > 0; i++) {
        char term[MAX_TERM_LEN];
        strncpy(term, argv[i], MAX_TERM_LEN - 1);
        term[MAX_TERM_LEN - 1] = '\0';

        /* convert to lowercase */
        for (int j = 0; term[j]; j++)
            term[j] = (char)tolower((unsigned char)term[j]);

        /* look up this term in dictionary */
        const DictEntry *te = dict_lookup(term);
        if (!te) {
            /* this term not found → no pages can match ALL terms → done */
            free(result); result = NULL; n = 0; break;
        }

        /* load this term's postings list */
        uint32_t *postings = load_postings(pf, te);
        if (!postings) {
            free(result); result = NULL; n = 0; break;
        }

        /* intersect current result with this term's postings */
        int       new_n;
        uint32_t *new_result = intersect(result, n, postings, te->df, &new_n);
        free(result);   /* free old result */
        free(postings); /* free this term's postings */
        result = new_result; /* new result is the intersection */
        n      = new_n;      /* update count */
    }

    fclose(pf); /* done with postings.bin */

    /* Step 7: print results */
    if (!result || n == 0) {
        /* no pages matched all query terms */
        printf("No documents matched all query terms.\n");
        free(result);
        return 0;
    }

    /* print count and all matching pages */
    printf("Found %d matching document%s (AND across terms):\n",
           n, n == 1 ? "" : "s"); /* "document" vs "documents" */

    for (int i = 0; i < n; i++) {
        uint32_t docid = result[i];
        if (docid < MAX_DOCS && doc_urls[docid][0])
            printf("%u %s\n", docid, doc_urls[docid]); /* print docid and URL */
        else
            printf("%u (unknown)\n", docid); /* docid has no URL — shouldn't happen */
    }

    /* cleanup */
    free(result);       /* free the result array */
    free(dict_entries); /* free the dictionary */
    return 0;           /* exit successfully */
}
