#ifndef PARSE_H
#define PARSE_H

/*
 * parse.h — declares the functions that look inside a directory.
 * Used by worker threads: every entry found in a directory becomes new work
 * (a subdirectory to scan, or a file to index).
 */

/*
 * EntryCallback — a function pointer type.
 * list_dir() calls this function once for EACH entry it finds.
 *   path      — heap-allocated full path of the entry (e.g. "testdata/os/threads.txt")
 *   is_dir    — 1 if the entry is a subdirectory, 0 if it is a regular file
 *   user_data — extra data passed through from the caller
 * The callback takes ownership of path and must free() it when done.
 */
typedef void (*EntryCallback)(char *path, int is_dir, void *user_data);

/*
 * list_dir — reads one directory and reports its entries.
 *   - Skips "." and ".." and hidden entries (names starting with '.')
 *   - Skips symbolic links (this also prevents infinite loops)
 *   - Only regular files and subdirectories are reported
 *
 * Returns  0 on success.
 * Returns -1 if the directory could not be opened (error is logged).
 */
int list_dir(const char *dir, EntryCallback callback, void *user_data);

/*
 * has_extension — checks whether a path ends with the given extension.
 *   has_extension("a/b/notes.TXT", ".txt")  → 1  (case-insensitive)
 *   has_extension("a/b/notes.md",  ".txt")  → 0
 * An empty or NULL ext means "accept every file" and always returns 1.
 */
int has_extension(const char *path, const char *ext);

#endif /* PARSE_H */
