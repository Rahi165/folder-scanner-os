#include <getopt.h>      /* for getopt_long() — parses command line arguments */
#include <stdio.h>       /* for printf, fprintf, snprintf */
#include <stdlib.h>      /* for exit(), atoi() */
#include <string.h>      /* for memset, strncpy */
#include <unistd.h>      /* for sleep(), close() */
#include <signal.h>      /* for signal handling (Ctrl+C) */
#include <sys/stat.h>    /* for mkdir() — creating directories */
#include <sys/socket.h>  /* for socket(), connect(), send() */
#include <sys/un.h>      /* for sockaddr_un — UNIX domain socket address */
#include <time.h>        /* for clock_gettime() — measuring runtime */
#include <pthread.h>     /* for pthread_mutex_t */

#include "queue.h"           /* path queue */
#include "visited.h"         /* visited set */
#include "threadpool.h"      /* worker threads */
#include "../common/ipc.h"   /* CrawlMsg — message sent to indexer */
#include "../common/log.h"   /* logging */

/*
 * CrawlerConfig — holds all command line arguments.
 * Filled in by parse_args() at startup.
 */
typedef struct {
    char *root_dir;    /* --root   : folder to scan */
    int   max_depth;   /* --max-depth : how many subfolder levels to descend */
    int   max_pages;   /* --max-pages : stop after this many files */
    int   num_threads; /* -t          : number of worker threads */
    char *out_dir;     /* --out       : directory to save file copies and index */
    char *ipc_path;    /* --ipc       : UNIX socket path to indexer */
    char *ext;         /* --ext       : only index files with this extension */
    long  max_size;    /* --max-size  : skip files larger than this (bytes) */
} CrawlerConfig;

/*
 * print_usage — prints how to use the crawler then exits.
 * Called when -h flag is used or required arguments are missing.
 */
static void print_usage(const char *prog) {
    fprintf(stderr,
        "USAGE: %s --root <dir> --max-depth <D> --max-pages <N>"
        " -t <threads> --out <dir> --ipc <path>"
        " [--ext <.txt>] [--max-size <bytes>]\n", prog);
}

/*
 * parse_args — reads command line arguments and fills CrawlerConfig.
 * Uses getopt_long() which handles both --long and -short flags.
 * Default values: max_depth=3, max_pages=100, num_threads=4,
 * ext=".txt", max_size=1048576 (1 MB)
 * Exits with error if required args (--root, --out, --ipc) are missing.
 */
static CrawlerConfig parse_args(int argc, char **argv) {
    /* set default values */
    CrawlerConfig cfg = { NULL, 3, 100, 4, NULL, NULL, ".txt", 1048576 };

    /* define all valid command line options */
    static struct option opts[] = {
        { "root",      required_argument, 0, 'r' }, /* --root <dir> */
        { "max-depth", required_argument, 0, 'd' }, /* --max-depth <D> */
        { "max-pages", required_argument, 0, 'n' }, /* --max-pages <N> */
        { "out",       required_argument, 0, 'o' }, /* --out <dir> */
        { "ipc",       required_argument, 0, 'i' }, /* --ipc <path> */
        { "ext",       required_argument, 0, 'e' }, /* --ext <.txt> */
        { "max-size",  required_argument, 0, 'z' }, /* --max-size <bytes> */
        { "help",      no_argument,       0, 'h' }, /* --help or -h */
        { 0, 0, 0, 0 }                              /* end marker */
    };

    int c;
    /* loop through all arguments until none left */
    while ((c = getopt_long(argc, argv, "t:h", opts, NULL)) != -1) {
        switch (c) {
            case 'r': cfg.root_dir    = optarg;       break; /* save root folder */
            case 'd': cfg.max_depth   = atoi(optarg); break; /* convert string to int */
            case 'n': cfg.max_pages   = atoi(optarg); break;
            case 't': cfg.num_threads = atoi(optarg); break;
            case 'o': cfg.out_dir     = optarg;       break;
            case 'i': cfg.ipc_path    = optarg;       break;
            case 'e': cfg.ext         = optarg;       break;
            case 'z': cfg.max_size    = atol(optarg); break;
            case 'h': print_usage(argv[0]); exit(0);  /* show help and exit */
            default:  print_usage(argv[0]); exit(1);  /* unknown flag — exit with error */
        }
    }

    /* check required arguments — exit if any are missing */
    if (!cfg.root_dir || !cfg.out_dir || !cfg.ipc_path) {
        print_usage(argv[0]); exit(1);
    }
    if (cfg.num_threads < 1) {
        fprintf(stderr, "-t must be at least 1\n"); exit(1);
    }
    return cfg;
}

/*
 * g_queue — global pointer to the path queue.
 * Needed by the SIGINT handler which runs outside of main().
 */
static URLQueue *g_queue = NULL;

/*
 * g_shutdown — flag set to 1 when Ctrl+C is pressed.
 * volatile sig_atomic_t — special type safe to use in signal handlers.
 */
static volatile sig_atomic_t g_shutdown = 0;

/*
 * sigint_handler — called when user presses Ctrl+C.
 * Sets shutdown flag and wakes up all threads so they exit cleanly.
 * Without this, pressing Ctrl+C would leave zombie processes.
 */
static void sigint_handler(int sig) {
    (void)sig;           /* suppress unused parameter warning */
    g_shutdown = 1;      /* set shutdown flag */
    if (g_queue) queue_shutdown(g_queue); /* wake up all waiting threads */
}

/*
 * mkdir_p — creates a directory and all parent directories.
 * Like "mkdir -p" in the terminal.
 * Example: mkdir_p("data/pages") creates both "data/" and "data/pages/"
 * Ignores errors if directory already exists.
 */
static void mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path); /* copy path to modifiable buffer */
    /* walk through each character looking for '/' */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';          /* temporarily end string here */
            mkdir(tmp, 0755);   /* create directory up to this point */
            *p = '/';           /* restore the '/' */
        }
    }
    mkdir(tmp, 0755); /* create the final directory */
}

/*
 * connect_to_indexer — connects to the indexer's UNIX socket.
 * Retries up to 10 times (1 second apart) in case indexer isn't ready yet.
 * The indexer must be started BEFORE the crawler.
 * Returns the socket file descriptor on success.
 * Exits the program if connection fails after 10 tries.
 */
static int connect_to_indexer(const char *ipc_path) {
    /* create a UNIX domain socket */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    /* set up the socket address with the path to the socket file */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;                              /* UNIX socket type */
    strncpy(addr.sun_path, ipc_path, sizeof(addr.sun_path) - 1); /* socket path */

    /* try to connect up to 10 times */
    for (int i = 0; i < 10; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            LOG_INFO("Connected to indexer at %s", ipc_path);
            return fd; /* connected successfully */
        }
        LOG_INFO("Waiting for indexer (%d/10)...", i + 1);
        sleep(1); /* wait 1 second before retrying */
    }
    fprintf(stderr, "Failed to connect to indexer at %s\n", ipc_path);
    exit(1); /* give up after 10 tries */
}

/*
 * main — the entry point of the crawler program.
 *
 * Order of operations:
 *   1. Parse command line arguments
 *   2. Set up signal handler for Ctrl+C
 *   3. Create output directories
 *   4. Connect to indexer via UNIX socket
 *   5. Initialize path queue and visited set
 *   6. Add the root folder to the queue
 *   7. Start timer
 *   8. Create thread pool (starts all worker threads)
 *   9. Wait for all threads to finish
 *  10. Send sentinel to indexer (signals "done")
 *  11. Print crawl summary
 *  12. Clean up all resources
 */
int main(int argc, char *argv[]) {
    /* Step 1: parse command line arguments */
    CrawlerConfig cfg = parse_args(argc, argv);

    /* Step 2: initialize logger — NULL means log to screen */
    log_init(NULL);

    /* Step 3: install signal handler for Ctrl+C (SIGINT) */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler; /* our handler function */
    sigaction(SIGINT, &sa, NULL);   /* register it with the OS */

    /* Step 4: create output directories */
    char pages_dir[512];
    snprintf(pages_dir, sizeof(pages_dir), "%s/pages", cfg.out_dir);
    mkdir_p(pages_dir); /* creates "data/pages/" */

    /* Step 5: make sure the root folder exists and really is a folder */
    struct stat root_st;
    if (stat(cfg.root_dir, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        fprintf(stderr, "Root folder not found or not a directory: %s\n", cfg.root_dir);
        return 1;
    }

    /* the extension filter needs a leading dot: "txt" becomes ".txt" */
    char ext_buf[64];
    if (cfg.ext[0] && cfg.ext[0] != '.') {
        snprintf(ext_buf, sizeof(ext_buf), ".%s", cfg.ext);
        cfg.ext = ext_buf;
    }

    /* Step 6: connect to the indexer */
    int ipc_fd = connect_to_indexer(cfg.ipc_path);

    /* Step 7: create the path queue and visited set */
    URLQueue   *q  = queue_create(4096, cfg.num_threads); /* max 4096 paths in queue */
    VisitedSet *vs = visited_create();
    g_queue = q; /* save pointer for SIGINT handler */

    /* Step 8: add the root folder to the queue as the first item of work */
    visited_check_and_insert(vs, cfg.root_dir); /* mark root as visited */
    queue_push(q, cfg.root_dir, 0);             /* push root at depth 0 */

    /* Step 9: set up shared counters — all start at 0 */
    int pages_fetched = 0; /* total files successfully read */
    int pages_failed  = 0; /* total files/folders that failed */
    int pages_skipped = 0; /* total files skipped (extension, size, limit) */
    int next_docid    = 0; /* next unique file ID to assign */

    /* mutexes to protect shared counters and IPC socket */
    pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t ipc_lock     = PTHREAD_MUTEX_INITIALIZER;

    /* pack everything into WorkerArgs — passed to all threads */
    WorkerArgs args = {
        .url_queue     = q,              /* shared path queue */
        .visited       = vs,             /* shared visited set */
        .max_depth     = cfg.max_depth,  /* folder depth limit */
        .ipc_fd        = ipc_fd,         /* socket to indexer */
        .out_dir       = cfg.out_dir,    /* where to save file copies */
        .ext           = cfg.ext,        /* extension filter */
        .max_size      = cfg.max_size,   /* size filter */
        .pages_fetched = &pages_fetched, /* pointer so threads can update it */
        .pages_failed  = &pages_failed,
        .pages_skipped = &pages_skipped,
        .max_pages     = cfg.max_pages,  /* file limit */
        .next_docid    = &next_docid,    /* pointer so threads can get unique IDs */
        .counter_lock  = &counter_lock,  /* mutex for counters */
        .ipc_lock      = &ipc_lock,      /* mutex for socket */
    };

    /* Step 10: start the timer */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0); /* record start time */

    /* Step 11: create thread pool — this starts all worker threads */
    ThreadPool *tp = threadpool_create(cfg.num_threads, args);

    /* Step 12: wait for ALL threads to finish — blocks here until done */
    threadpool_wait_and_destroy(tp);

    /* Step 13: stop the timer */
    clock_gettime(CLOCK_MONOTONIC, &t1); /* record end time */
    /* calculate elapsed time in seconds */
    double elapsed = (double)(t1.tv_sec  - t0.tv_sec) +
                     (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    /* Step 14: send sentinel message to indexer — signals "crawling is done" */
    CrawlMsg sentinel;
    memset(&sentinel, 0, sizeof(sentinel));      /* zero all fields */
    sentinel.docid = IPC_SENTINEL_DOCID;         /* special value = UINT32_MAX */
    send(ipc_fd, &sentinel, sizeof(sentinel), 0); /* send to indexer */
    close(ipc_fd);                                /* close the socket */

    /* Step 15: print the crawl summary — what you saw in the output */
    printf("\n=== Scan Summary ===\n");
    printf("Files indexed  : %d\n", pages_fetched);  /* successfully read */
    printf("Files failed   : %d\n", pages_failed);   /* could not open/read */
    printf("Files skipped  : %d\n", pages_skipped);  /* wrong extension, too big, limit */
    printf("Max queue depth: %d\n", queue_max_size(q)); /* peak queue size */
    printf("Runtime        : %.2f s\n", elapsed);    /* total time in seconds */

    /* Step 16: clean up all resources */
    queue_destroy(q);              /* free the path queue */
    visited_destroy(vs);           /* free the visited set */
    pthread_mutex_destroy(&counter_lock); /* destroy mutexes */
    pthread_mutex_destroy(&ipc_lock);
    log_close();                   /* close the logger */
    return 0;                      /* exit successfully */
}
