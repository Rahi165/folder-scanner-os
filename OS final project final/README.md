# Multithreaded Folder Scanner Pipeline

Three programs written in C: **crawler** (a folder scanner), **indexer**, and **query**.

The crawler walks a folder tree with a pool of worker threads, reads every text file it finds,
and sends the file's metadata to the indexer. The indexer builds an on-disk inverted index
(word → list of files). The query tool searches that index.

```
folder → list entries → read file → save copy → send metadata via IPC → build inverted index → query
```

## Dependencies

- `gcc` (or `clang`), `make`, and POSIX threads. **No libcurl and no internet needed.**
  - Linux (Ubuntu/Debian): `sudo apt install build-essential`
  - macOS: `xcode-select --install`
  - Windows: use **WSL2** (Ubuntu) and install `build-essential` — the code uses POSIX APIs.

## Build & Run

```bash
make all       # compile all three programs into build/ and link binaries
make run       # demo: scan testdata/ with 4 threads, then run a sample query
make test      # automated checks (scan, filters, depth, limits, 1 vs 8 threads)
make clean     # remove build/, binaries, and generated data files
```

Manual run sequence — always start the indexer first:

```bash
./indexer --ipc /tmp/crawl.sock --out data/index
./crawler --root testdata --max-depth 3 --max-pages 200 -t 8 \
          --out data --ipc /tmp/crawl.sock
./query --index data/index operating systems threads
```

### Crawler options

| Option | Meaning | Default |
|--------|---------|---------|
| `--root <dir>` | Folder to scan (required) | — |
| `--out <dir>` | Output folder for saved copies (`<dir>/pages/`) (required) | — |
| `--ipc <path>` | UNIX socket path of the indexer (required) | — |
| `--max-depth <D>` | How many subfolder levels to descend below the root | 3 |
| `--max-pages <N>` | Stop after indexing this many files | 100 |
| `-t <threads>` | Number of worker threads | 4 |
| `--ext <.txt>` | Only index files with this extension (case-insensitive) | `.txt` |
| `--max-size <bytes>` | Skip files larger than this | 1048576 (1 MB) |

**Scanning rules**
- Depth: files directly inside the root folder have depth 0; each subfolder level adds 1.
- Hidden entries (names starting with `.`) and symbolic links are ignored. Ignoring symlinks means a
  link pointing back up the tree can never cause an infinite loop.
- Files that fail the extension or size filter are counted as *skipped*; files that cannot be
  opened or read are counted as *failed*.

## Work Queue Design

`src/crawler/queue.c` implements a **bounded ring buffer** of `(path, depth)` pairs.

- **Capacity**: 4096 items (configurable at creation).
- **Synchronization**: one mutex + two condition variables (`not_full`, `not_empty`).
- **Backpressure**: `queue_push()` blocks the caller when the ring is full; it wakes when a consumer removes an item.
- **Deadlock avoidance**: workers are both consumers *and* producers (a folder produces new work). If every
  worker blocked in `queue_push()` on a full queue, nobody would be left to pop and the program would hang.
  Workers therefore use `queue_try_push()`, which never blocks; when it reports "full", the worker handles that
  entry itself instead of waiting (see `entry_callback` in `threadpool.c`).
- **Frontier exhaustion detection**: an `idle_workers` counter is incremented inside `queue_pop()` before a thread waits, and decremented when it acquires an item. When `idle_workers == num_workers` **and** `count == 0`, all workers are stuck waiting with nothing left to do — the queue sets `shutdown = 1` and broadcasts to let all threads exit cleanly. This check happens under the queue lock, making it race-free.
- **Forced stop**: `queue_shutdown()` sets the flag and broadcasts both condition variables; used when `--max-pages` is reached or `SIGINT` (Ctrl+C) fires.
- **High-water mark**: `max_count_ever` tracks the peak queue depth for the final summary.

## Visited Set Design

`src/crawler/visited.c` implements a **chaining hash table** with 65,536 buckets (power of 2 for fast masking).

- Hash function: **djb2** (`h = h * 33 ^ c`).
- Single mutex protects the entire table.
- `visited_check_and_insert()` performs the check and insert **in one lock acquisition** — this prevents TOCTOU races where two threads could both see a path as unseen and queue it twice.
- Returns `1` if already seen (skip), `0` if freshly inserted (process).

## IPC Protocol

Crawler and indexer communicate over a **UNIX domain stream socket** (`SOCK_STREAM`).

- The indexer binds and listens on the socket path (`--ipc`); the crawler connects after startup.
- Each message is a fixed-size `CrawlMsg` struct (2,568 bytes): `docid`, `depth`, `src_path[2048]`, `filepath[512]`.
  - `src_path` is where the file was found; `filepath` is the saved copy (`<out>/pages/<docid>.txt`).
- Fixed size eliminates framing complexity: one `send()` per message, one `recv_full()` loop per message.
- The crawler sends one message per successfully saved file.
- When scanning is complete, the crawler sends a **sentinel** with `docid = UINT32_MAX`, signalling the indexer to flush and exit.
- Multiple worker threads share the socket fd; a mutex (`ipc_lock`) serializes writes.

## Index Format

Files live in the directory passed to `--out` (indexer) / `--index` (query), plus the saved copies:

| File | Format | Description |
|------|--------|-------------|
| `pages/<docid>.txt` | plain text | Copy of each scanned file (written by the crawler) |
| `docs.tsv` | `docid TAB src_path TAB filepath TAB depth` | Document map, one line per file |
| `dict.tsv` | `term TAB byte_offset TAB df` | Dictionary: term → location in postings.bin |
| `postings.bin` | binary `uint32_t` values | Docid lists concatenated per term (no length prefix; `df` from dict.tsv gives the count) |

The indexer lower-cases words, keeps words of 3+ letters, and drops a small stop-word list.
The query tool sorts `dict.tsv` entries in memory after loading and uses `bsearch()` for O(log n) term lookup. It then `fseek()`s to `byte_offset` in `postings.bin` and reads `df` uint32 values. Postings are sorted before AND-intersection using the two-pointer technique (O(m+n)).

## Testing

`make test` runs `test.sh`, which builds nothing new but runs the pipeline on `testdata/` and checks:
file counts, multi-word AND queries, that hidden files and files with other extensions are ignored,
`--max-depth`, `--max-pages`, `--ext`, `--max-size`, a missing root folder, and that **1 thread and 8 threads
find exactly the same files**.

For extra checking of the threading code, rebuild with a sanitizer, for example:

```bash
make clean && make all CFLAGS="-Wall -Wextra -g -pthread -I./src -fsanitize=thread"
```

## Source Layout

```
src/
  common/   ipc.h         — CrawlMsg struct and IPC_SENTINEL_DOCID
            log.h / log.c — thread-safe logger (mutex-protected)
  crawler/  main.c        — CLI (getopt_long), SIGINT handler, main loop
            queue.h/c     — bounded path queue (ring buffer)
            visited.h/c   — visited-path hash set (djb2, atomic check+insert)
            fetch.h/c     — reads one file from disk into memory
            parse.h/c     — lists a folder's entries, extension filter
            threadpool.h/c— fixed-size pthread pool + worker_thread
  indexer/  main.c        — IPC server, receive loop, doc map append
            indexer.h/c   — InvertedIndex, tokenizer, stop words, flush
  query/    main.c        — load index, bsearch dict, two-pointer AND
testdata/                 — small sample folder tree used by `make run` and `make test`
test.sh                   — automated checks (`make test`)
```
