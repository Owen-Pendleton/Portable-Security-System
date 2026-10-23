#include "store/dirlist.h"

#include "common/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int push(struct dirlist *dl, const char *name)
{
	if (dl->n == dl->cap) {
		size_t cap = dl->cap ? dl->cap * 2 : 16;
		char **names = realloc(dl->names, cap * sizeof *names);

		if (names == NULL)
			return -1;
		dl->names = names;
		dl->cap = cap;
	}
	dl->names[dl->n] = strdup(name);
	if (dl->names[dl->n] == NULL)
		return -1;
	dl->n++;
	return 0;
}

int dirlist_read(int dir_fd, struct dirlist *dl)
{
	DIR *d = NULL;
	int fd, ret = -1;

	memset(dl, 0, sizeof *dl);
	fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		pss_log("dirlist: openat: %s", strerror(errno));
		goto out;
	}
	d = fdopendir(fd);
	if (d == NULL) {
		pss_log("dirlist: fdopendir: %s", strerror(errno));
		goto out;
	}
	fd = -1; /* owned by d now */

	for (;;) {
		struct dirent *e;

		errno = 0;
		e = readdir(d);
		if (e == NULL) {
			if (errno != 0) {
				pss_log("dirlist: readdir: %s", strerror(errno));
				goto out;
			}
			break;
		}
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (push(dl, e->d_name) != 0) {
			pss_log("dirlist: out of memory");
			goto out;
		}
	}
	ret = 0;
out:
	if (d != NULL && closedir(d) != 0)
		pss_log("dirlist: closedir: %s", strerror(errno));
	if (fd >= 0)
		close(fd);
	if (ret != 0)
		dirlist_free(dl);
	return ret;
}

void dirlist_free(struct dirlist *dl)
{
	for (size_t i = 0; i < dl->n; i++)
		free(dl->names[i]);
	free(dl->names);
	memset(dl, 0, sizeof *dl);
}
