#include <getopt.h>      /* for getopt_long() — parses command line arguments */
#include <stdio.h>       /* for FILE, fopen, fclose, fprintf, fgets, fseek */
#include <stdlib.h>      /* for malloc, free, exit */
#include <string.h>      /* for memset, strncpy, strerror */
#include <unistd.h>      /* for close(), unlink() — closing/deleting socket file */
#include <sys/socket.h>  /* for socket(), bind(), listen(), accept(), recv() */
#include <sys/un.h>      /* for sockaddr_un — UNIX domain socket address */
#include <sys/stat.h>    /* for mkdir() — creating directories */
#include <errno.h>       /* for errno and strerror() — error descriptions */

#include "indexer.h"         /* InvertedIndex, tokenize, index_insert, write_index */
#include "../common/ipc.h"   /* CrawlMsg — message format from crawler */
#include "../common/log.h"   /* LOG_INFO, LOG_WARN, LOG_ERROR — logging */

/*
 * recv_full — reads exactly len bytes from a socket.
 * Regular recv() may return fewer bytes than requested (partial read).
 * This function loops until ALL len bytes have been received.
 *
 * fd  — the socket file descriptor to read from
 * buf — where to store the received bytes
 * len — exactly how many bytes to read
 *
 * Returns number of bytes read on success.
 * Returns 0 or negative if connection closed or error.
 */
static ssize_t recv_full(int fd, void *buf, size_t len) {
    size_t  got = 0;      /* how many bytes received so far */
    char   *p   = buf;    /* pointer into buffer where next bytes go */

    while (got < len) {
        /* receive up to (len - got) more bytes */
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) return n; /* connection closed or error */
        got += (size_t)n;     /* advance by bytes received */
    }
    return (ssize_t)got; /* all bytes received successfully */
}

/*
 * append_doc_map — adds one line to docs.tsv for each page indexed.
 * docs.tsv is the document map: docid → source path → filepath → depth
 * Opened in append mode ("a") so each call adds to the existing file.
 *
 * Format of each line: "docid TAB src_path TAB filepath TAB depth\n"
 * Example: "5\ttestdata/os/threads.txt\tdata/pages/5.txt\t1\n"
 *
 * index_dir — directory where docs.tsv lives
 * msg       — the CrawlMsg received from the crawler
 */
static void append_doc_map(const char *index_dir, const CrawlMsg *msg) {
    char path[512];
    /* build path to docs.tsv */
    snprintf(path, sizeof(path), "%s/docs.tsv", index_dir);

    FILE *fp = fopen(path, "a"); /* "a" = append — don't overwrite existing data */
    if (!fp) {
        LOG_ERROR("Cannot open docs.tsv: %s", strerror(errno));
        return;
    }

    /* write one tab-separated line for this document */
    fprintf(fp, "%u\t%s\t%s\t%u\n",
            msg->docid,    /* unique file ID */
            msg->src_path, /* the original file path */
            msg->filepath, /* where the copy was saved on disk */
            msg->depth);   /* how deep in the folder tree this was found */
    fclose(fp);
}

/*
 * IndexCtx — context passed to the token callback.
 * Contains everything the callback needs to add a word to the index.
 */
typedef struct {
    InvertedIndex *idx;   /* the in-memory index to insert words into */
    uint32_t       docid; /* the page ID being tokenized */
} IndexCtx;

/*
 * token_callback — called by tokenize() for each valid word found.
 * Inserts the word → docid relationship into the inverted index.
 *
 * word      — the lowercase word found (e.g. "linux")
 * user_data — our IndexCtx struct cast to void*
 */
static void token_callback(const char *word, void *user_data) {
    IndexCtx *ctx = user_data;           /* cast back to IndexCtx */
    index_insert(ctx->idx, word, ctx->docid); /* add word→page to index */
}

/*
 * process_document — reads a saved text file and adds its words to the index.
 *
 * Steps:
 *   1. Open the saved text file using the filepath from the CrawlMsg
 *   2. Read entire file into memory
 *   3. Call tokenize() to extract all valid words
 *   4. tokenize() calls token_callback() for each word
 *   5. token_callback() calls index_insert() for each word
 *
 * idx — the in-memory inverted index being built
 * msg — the CrawlMsg from the crawler (has filepath and docid)
 */
static void process_document(InvertedIndex *idx, const CrawlMsg *msg) {
    /* open the saved text file */
    FILE *f = fopen(msg->filepath, "r");
    if (!f) {
        LOG_WARN("Cannot open %s: %s", msg->filepath, strerror(errno));
        return; /* skip this document — log warning but don't crash */
    }

    /* get file size by seeking to end */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); /* current position = file size */
    rewind(f);          /* go back to beginning */

    /* allocate buffer for entire file content */
    char *buf = malloc((size_t)sz + 1); /* +1 for null terminator */
    if (!buf) { fclose(f); return; }    /* out of memory — skip */

    /* read entire file into buffer */
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0'; /* null-terminate so it's a valid C string */
    fclose(f);

    /* tokenize the text and add all words to the index */
    IndexCtx ctx = { idx, msg->docid }; /* set up context for callback */
    tokenize(buf, token_callback, &ctx);
    free(buf); /* done with file content */

    /* log that this page was indexed */
    LOG_INFO("Indexed docid=%-5u  %s", msg->docid, msg->src_path);
}

/*
 * create_server_socket — creates a UNIX domain socket and starts listening.
 * The crawler will connect to this socket to send page metadata.
 *
 * Steps:
 *   1. Remove any old socket file at ipc_path (from previous run)
 *   2. Create a new UNIX stream socket
 *   3. Bind it to the path (creates the socket file)
 *   4. Listen for connections (queue up to 1 connection)
 *
 * ipc_path — the file path for the socket (e.g. "/tmp/crawl.sock")
 * Returns the server socket file descriptor.
 */
static int create_server_socket(const char *ipc_path) {
    /* remove stale socket file from previous run */
    unlink(ipc_path);

    /* create a UNIX domain stream socket */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    /* set up the socket address */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX; /* UNIX domain socket type */
    strncpy(addr.sun_path, ipc_path, sizeof(addr.sun_path) - 1); /* socket path */

    /* bind socket to the path — creates the socket file */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }

    /* start listening — accept up to 1 queued connection (only crawler connects) */
    if (listen(fd, 1) < 0) { perror("listen"); exit(1); }

    return fd; /* return server socket descriptor */
}

/*
 * mkdir_p — creates a directory and all parent directories.
 * Same as "mkdir -p" in the terminal.
 */
static void mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/*
 * main — entry point of the indexer program.
 *
 * Order of operations:
 *   1. Parse --ipc and --out command line arguments
 *   2. Initialize logger
 *   3. Create output directory
 *   4. Create UNIX socket and wait for crawler to connect
 *   5. Accept crawler connection
 *   6. Loop: receive CrawlMsg → save to docs.tsv → tokenize → index
 *   7. When sentinel received → flush index to disk
 *   8. Clean up and exit
 */
int main(int argc, char *argv[]) {
    /* default values */
    char ipc_path[512] = "/tmp/crawl.sock"; /* UNIX socket path */
    char out_dir[512]  = "data/index";      /* output directory */

    /* define command line options */
    static struct option opts[] = {
        { "ipc",  required_argument, 0, 'i' }, /* --ipc <socket-path> */
        { "out",  required_argument, 0, 'o' }, /* --out <dir> */
        { "help", no_argument,       0, 'h' }, /* --help or -h */
        { 0, 0, 0, 0 }
    };

    int c;
    /* parse command line arguments */
    while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
        if (c == 'i') strncpy(ipc_path, optarg, sizeof(ipc_path) - 1);
        if (c == 'o') strncpy(out_dir,  optarg, sizeof(out_dir)  - 1);
        if (c == 'h') {
            /* print usage and exit cleanly */
            fprintf(stderr, "USAGE: ./indexer --ipc <socket-path> --out <dir>\n");
            exit(0);
        }
    }

    /* Step 2: initialize logger */
    log_init(NULL); /* NULL = log to screen */

    /* Step 3: create output directory */
    mkdir_p(out_dir); /* creates "data/index/" if it doesn't exist */

    /* Step 4: create server socket and start listening */
    int server_fd = create_server_socket(ipc_path);
    LOG_INFO("Indexer listening on %s", ipc_path);
    /* this is what you see: "[INFO] Indexer listening on /tmp/crawl.sock" */

    /* Step 5: wait for crawler to connect — blocks here until crawler starts */
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) { perror("accept"); return 1; }
    LOG_INFO("Crawler connected"); /* crawler has connected */

    /* Step 6: create empty inverted index in memory */
    InvertedIndex *idx = index_create();

    /* Step 7: main receive loop — process pages until sentinel received */
    CrawlMsg msg; /* buffer to hold each incoming message */

    while (1) {
        /* receive exactly sizeof(CrawlMsg) bytes from the crawler */
        ssize_t n = recv_full(client_fd, &msg, sizeof(msg));

        if (n <= 0) break; /* connection closed unexpectedly */

        /* check for sentinel — crawler sends this when done crawling */
        if (msg.docid == IPC_SENTINEL_DOCID) {
            LOG_INFO("Done signal received"); /* crawler finished */
            break; /* exit the loop and flush index */
        }

        /* save document metadata to docs.tsv */
        append_doc_map(out_dir, &msg);

        /* read the saved text file and add words to the index */
        process_document(idx, &msg);
    }

    /* Step 8: close connections */
    close(client_fd);  /* close connection to crawler */
    close(server_fd);  /* close listening socket */
    unlink(ipc_path);  /* remove the socket file from filesystem */

    /* Step 9: flush the in-memory index to disk */
    /* creates dict.tsv and postings.bin in out_dir */
    write_index(idx, out_dir);

    /* Step 10: cleanup */
    index_destroy(idx); /* free all memory used by the index */
    log_close();        /* flush and close logger */
    return 0;           /* exit successfully */
}
