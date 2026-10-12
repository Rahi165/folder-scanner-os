#include "parse.h"           /* our own header — EntryCallback and declarations */
#include "../common/ipc.h"   /* for IPC_SRC_MAX — longest path we can send to the indexer */
#include "../common/log.h"   /* for logging problems */
#include <dirent.h>          /* for opendir, readdir, closedir */
#include <sys/stat.h>        /* for lstat, S_ISDIR, S_ISREG */
#include <string.h>          /* for strlen, strdup, strcasecmp */
#include <strings.h>         /* for strcasecmp */
#include <stdlib.h>          /* for free */
#include <stdio.h>           /* for snprintf */
#include <errno.h>           /* for errno */

/*
 * list_dir — opens a directory and calls callback for every usable entry.
 *
 * How it works:
 *   1. opendir() opens the directory
 *   2. readdir() returns one entry at a time (NULL when there are no more)
 *   3. For each entry, build the full path "dir/name"
 *   4. lstat() tells us what kind of thing it is (file, directory, symlink...)
 *      lstat (not stat) does NOT follow symbolic links, so a link that points
 *      back up the tree can never trap us in an infinite loop.
 *   5. Regular files and directories are passed to the callback
 */
int list_dir(const char *dir, EntryCallback callback, void *user_data) {
    DIR *dp = opendir(dir);
    if (!dp) {
        LOG_WARN("cannot open directory %s: %s", dir, strerror(errno));
        return -1;
    }

    /* ignore trailing slashes so "data/" and "data" produce the same paths */
    size_t dlen = strlen(dir);
    while (dlen > 1 && dir[dlen - 1] == '/') dlen--;

    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        const char *name = ent->d_name;

        /* skip ".", ".." and hidden files such as .DS_Store or .git */
        if (name[0] == '.') continue;

        /* build the full path: "<dir>/<name>" */
        char full[IPC_SRC_MAX];
        int n = snprintf(full, sizeof(full), "%.*s/%s", (int)dlen, dir, name);
        if (n < 0 || (size_t)n >= sizeof(full)) {
            LOG_WARN("path too long, skipping %s/%s", dir, name);
            continue;
        }

        struct stat st;
        if (lstat(full, &st) != 0) {
            LOG_WARN("cannot stat %s: %s", full, strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode))
            callback(strdup(full), 1, user_data);   /* a subdirectory to scan later */
        else if (S_ISREG(st.st_mode))
            callback(strdup(full), 0, user_data);   /* a regular file to index */
        /* anything else (symlink, socket, device...) is ignored */
    }

    closedir(dp);
    return 0;
}

/*
 * has_extension — returns 1 if path ends with ext (case-insensitive).
 * Example: path = "notes/Lecture1.TXT", ext = ".txt" → 1
 */
int has_extension(const char *path, const char *ext) {
    if (!ext || !ext[0]) return 1;            /* no filter → accept everything */

    size_t plen = strlen(path);
    size_t elen = strlen(ext);
    if (plen < elen) return 0;                /* path shorter than the extension */

    return strcasecmp(path + plen - elen, ext) == 0;   /* compare the last elen chars */
}
