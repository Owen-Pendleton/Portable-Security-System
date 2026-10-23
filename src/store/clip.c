#include "store/clip.h"

#include "common/log.h"
#include "store/dirlist.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static int has_suffix(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);

	return n >= m && strcmp(s + n - m, suffix) == 0;
}

int clip_tmp_name(char *buf, size_t n, uint64_t event_id)
{
	int r = snprintf(buf, n, "%" PRIu64 ".dir.tmp", event_id);

	return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

int clip_final_name(char *buf, size_t n, uint64_t event_id)
{
	int r = snprintf(buf, n, "%" PRIu64 ".dir", event_id);

	return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

/* Parses "<digits>.dir" exactly. */
static int parse_final_name(const char *name, uint64_t *id)
{
	uint64_t v = 0;
	size_t len = strlen(name), i;

	if (len < 5 || strcmp(name + len - 4, ".dir") != 0)
		return -1;
	for (i = 0; i < len - 4; i++) {
		if (name[i] < '0' || name[i] > '9')
			return -1;
		v = v * 10 + (uint64_t)(name[i] - '0');
	}
	*id = v;
	return 0;
}

/* Removes a directory whose entries are all plain files (our burst dirs). */
static int remove_dir_at(int parent_fd, const char *name)
{
	struct dirlist dl;
	int fd, ret = -1;

	fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		pss_log("clip: open %s: %s", name, strerror(errno));
		return -1;
	}
	if (dirlist_read(fd, &dl) != 0)
		goto out;
	for (size_t i = 0; i < dl.n; i++) {
		if (unlinkat(fd, dl.names[i], 0) != 0) {
			pss_log("clip: unlink %s/%s: %s", name, dl.names[i], strerror(errno));
			dirlist_free(&dl);
			goto out;
		}
	}
	dirlist_free(&dl);
	if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0) {
		pss_log("clip: rmdir %s: %s", name, strerror(errno));
		goto out;
	}
	ret = 0;
out:
	if (close(fd) != 0)
		pss_log("clip: close %s: %s", name, strerror(errno));
	return ret;
}

/* Unlinks a file, or removes a one-level directory. */
static int remove_entry(int parent_fd, const char *name)
{
	if (unlinkat(parent_fd, name, 0) == 0)
		return 0;
	if (errno == EISDIR)
		return remove_dir_at(parent_fd, name);
	pss_log("clip: unlink %s: %s", name, strerror(errno));
	return -1;
}

int clip_commit(int clips_dir_fd, uint64_t event_id, uint32_t *n_images)
{
	char tmp[48], final[48];
	struct dirlist dl;
	uint32_t n = 0;
	int dir_fd, ret = -1;

	if (clip_tmp_name(tmp, sizeof tmp, event_id) != 0 || clip_final_name(final, sizeof final, event_id) != 0)
		return -1;
	dir_fd = openat(clips_dir_fd, tmp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dir_fd < 0) {
		pss_log("clip: open %s: %s", tmp, strerror(errno));
		return -1;
	}
	if (dirlist_read(dir_fd, &dl) != 0)
		goto out;
	for (size_t i = 0; i < dl.n; i++) {
		int fd = openat(dir_fd, dl.names[i], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

		if (fd < 0) {
			pss_log("clip: open %s/%s: %s", tmp, dl.names[i], strerror(errno));
			dirlist_free(&dl);
			goto out;
		}
		if (fsync(fd) != 0) {
			pss_log("clip: fsync %s/%s: %s", tmp, dl.names[i], strerror(errno));
			close(fd);
			dirlist_free(&dl);
			goto out;
		}
		if (close(fd) != 0)
			pss_log("clip: close %s/%s: %s", tmp, dl.names[i], strerror(errno));
		if (has_suffix(dl.names[i], ".jpg"))
			n++;
	}
	dirlist_free(&dl);
	if (fsync(dir_fd) != 0) {
		pss_log("clip: fsync %s: %s", tmp, strerror(errno));
		goto out;
	}
	if (renameat(clips_dir_fd, tmp, clips_dir_fd, final) != 0) {
		pss_log("clip: rename %s -> %s: %s", tmp, final, strerror(errno));
		goto out;
	}
	if (fsync(clips_dir_fd) != 0) {
		pss_log("clip: fsync clips/: %s", strerror(errno));
		goto out;
	}
	*n_images = n;
	ret = 0;
out:
	if (close(dir_fd) != 0)
		pss_log("clip: close %s: %s", tmp, strerror(errno));
	return ret;
}

int clip_discard(int clips_dir_fd, uint64_t event_id)
{
	char tmp[48];
	struct stat st;

	if (clip_tmp_name(tmp, sizeof tmp, event_id) != 0)
		return -1;
	if (fstatat(clips_dir_fd, tmp, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		if (errno == ENOENT)
			return 0;
		pss_log("clip: stat %s: %s", tmp, strerror(errno));
		return -1;
	}
	if (remove_entry(clips_dir_fd, tmp) != 0)
		return -1;
	pss_log("clip: discarded %s", tmp);
	return 0;
}

int clip_cleanup_tmp(int clips_dir_fd, uint32_t *removed)
{
	struct dirlist dl;
	int ret = 0;

	*removed = 0;
	if (dirlist_read(clips_dir_fd, &dl) != 0)
		return -1;
	for (size_t i = 0; i < dl.n; i++) {
		if (!has_suffix(dl.names[i], ".tmp"))
			continue;
		if (remove_entry(clips_dir_fd, dl.names[i]) != 0) {
			ret = -1;
			continue;
		}
		pss_log("store: removed interrupted clip %s", dl.names[i]);
		(*removed)++;
	}
	dirlist_free(&dl);
	if (*removed > 0 && fsync(clips_dir_fd) != 0) {
		pss_log("clip: fsync clips/: %s", strerror(errno));
		ret = -1;
	}
	return ret;
}

int clip_max_id(int clips_dir_fd, uint64_t *max_id)
{
	struct dirlist dl;
	uint64_t id;

	*max_id = 0;
	if (dirlist_read(clips_dir_fd, &dl) != 0)
		return -1;
	for (size_t i = 0; i < dl.n; i++)
		if (parse_final_name(dl.names[i], &id) == 0 && id > *max_id)
			*max_id = id;
	dirlist_free(&dl);
	return 0;
}

struct clip_info {
	uint64_t id;
	uint64_t bytes;
	const char *name;
};

static int cmp_clip(const void *a, const void *b)
{
	uint64_t x = ((const struct clip_info *)a)->id, y = ((const struct clip_info *)b)->id;

	return (x > y) - (x < y);
}

static int dir_bytes(int parent_fd, const char *name, uint64_t *bytes)
{
	struct dirlist dl;
	struct stat st;
	int fd, ret = -1;

	*bytes = 0;
	fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		pss_log("clip: open %s: %s", name, strerror(errno));
		return -1;
	}
	if (dirlist_read(fd, &dl) != 0)
		goto out;
	for (size_t i = 0; i < dl.n; i++) {
		if (fstatat(fd, dl.names[i], &st, AT_SYMLINK_NOFOLLOW) != 0) {
			pss_log("clip: stat %s/%s: %s", name, dl.names[i], strerror(errno));
			dirlist_free(&dl);
			goto out;
		}
		*bytes += (uint64_t)st.st_size;
	}
	dirlist_free(&dl);
	ret = 0;
out:
	if (close(fd) != 0)
		pss_log("clip: close %s: %s", name, strerror(errno));
	return ret;
}

int clip_enforce_quota(int clips_dir_fd, uint64_t quota_bytes, uint64_t need_bytes,
		       uint32_t floor_pct, uint32_t *evicted)
{
	struct dirlist dl;
	struct clip_info *clips = NULL;
	struct statvfs vfs;
	uint64_t used = 0, free_bytes, total_bytes;
	size_t n = 0;
	int ret = -1;

	*evicted = 0;
	if (dirlist_read(clips_dir_fd, &dl) != 0)
		return -1;
	if (dl.n > 0) {
		clips = malloc(dl.n * sizeof *clips);
		if (clips == NULL) {
			pss_log("clip: out of memory in quota check");
			goto out;
		}
	}
	for (size_t i = 0; i < dl.n; i++) {
		if (parse_final_name(dl.names[i], &clips[n].id) != 0)
			continue;
		if (dir_bytes(clips_dir_fd, dl.names[i], &clips[n].bytes) != 0)
			goto out;
		clips[n].name = dl.names[i];
		used += clips[n].bytes;
		n++;
	}
	if (n > 0)
		qsort(clips, n, sizeof *clips, cmp_clip);

	for (size_t i = 0; i < n && used + need_bytes > quota_bytes; i++) {
		if (remove_dir_at(clips_dir_fd, clips[i].name) != 0)
			goto out;
		pss_log("clip: quota: evicted %s (%" PRIu64 " bytes)", clips[i].name, clips[i].bytes);
		used -= clips[i].bytes;
		(*evicted)++;
	}
	if (*evicted > 0 && fsync(clips_dir_fd) != 0) {
		pss_log("clip: fsync clips/: %s", strerror(errno));
		goto out;
	}

	if (fstatvfs(clips_dir_fd, &vfs) != 0) {
		pss_log("clip: statvfs: %s", strerror(errno));
		goto out;
	}
	free_bytes = (uint64_t)vfs.f_bavail * vfs.f_frsize;
	total_bytes = (uint64_t)vfs.f_blocks * vfs.f_frsize;
	if (free_bytes < need_bytes || (free_bytes - need_bytes) * 100 < (uint64_t)floor_pct * total_bytes) {
		pss_log("store: free %.1f%% < floor %" PRIu32 "%%, clips disabled",
			total_bytes ? 100.0 * (double)free_bytes / (double)total_bytes : 0.0, floor_pct);
		ret = 1;
		goto out;
	}
	ret = 0;
out:
	free(clips);
	dirlist_free(&dl);
	return ret;
}
