#ifndef PSS_STORE_DIRLIST_H
#define PSS_STORE_DIRLIST_H

#include <stddef.h>

/* Snapshot of a directory's entry names (without "." and ".."), so callers
 * can unlink/rename while walking it. */
struct dirlist {
	char **names;
	size_t n;
	size_t cap;
};

/* Returns 0 or -1 (logged). On failure *dl is left empty. */
int dirlist_read(int dir_fd, struct dirlist *dl);
void dirlist_free(struct dirlist *dl);

#endif
