#include "store/evlog.h"

#include "common/log.h"
#include "store/clip.h"
#include "store/crc32.h"
#include "store/dirlist.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* "seg-NNNNNN.pel" */
static int parse_seg_name(const char *name, uint32_t *no)
{
	uint32_t v = 0;

	if (strlen(name) != 14 || strncmp(name, "seg-", 4) != 0 || strcmp(name + 10, ".pel") != 0)
		return -1;
	for (int i = 4; i < 10; i++) {
		if (name[i] < '0' || name[i] > '9')
			return -1;
		v = v * 10 + (uint32_t)(name[i] - '0');
	}
	*no = v;
	return 0;
}

static void seg_name(char buf[16], uint32_t no)
{
	snprintf(buf, 16, "seg-%06" PRIu32 ".pel", no);
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return (x > y) - (x < y);
}

/* Sorted segment numbers in log_dir_fd. *out is malloc'd (NULL if none). */
static int list_segments(int log_dir_fd, uint32_t **out, size_t *n)
{
	struct dirlist dl;
	uint32_t *nos = NULL;
	size_t count = 0;

	*out = NULL;
	*n = 0;
	if (dirlist_read(log_dir_fd, &dl) != 0)
		return -1;
	if (dl.n > 0) {
		nos = malloc(dl.n * sizeof *nos);
		if (nos == NULL) {
			pss_log("store: out of memory listing segments");
			dirlist_free(&dl);
			return -1;
		}
	}
	for (size_t i = 0; i < dl.n; i++)
		if (parse_seg_name(dl.names[i], &nos[count]) == 0)
			count++;
	dirlist_free(&dl);
	if (count > 0)
		qsort(nos, count, sizeof *nos, cmp_u32);
	*out = nos;
	*n = count;
	return 0;
}

static int read_segment(int log_dir_fd, uint32_t no, uint8_t **buf, uint64_t *size)
{
	char name[16];
	struct stat st;
	uint8_t *p = NULL;
	uint64_t got = 0;
	int fd, ret = -1;

	seg_name(name, no);
	fd = openat(log_dir_fd, name, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		pss_log("store: open %s: %s", name, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) != 0) {
		pss_log("store: fstat %s: %s", name, strerror(errno));
		goto out;
	}
	p = malloc(st.st_size > 0 ? (size_t)st.st_size : 1);
	if (p == NULL) {
		pss_log("store: out of memory reading %s", name);
		goto out;
	}
	while (got < (uint64_t)st.st_size) {
		ssize_t r = read(fd, p + got, (size_t)st.st_size - got);

		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			pss_log("store: read %s: %s", name, strerror(errno));
			goto out;
		}
		if (r == 0)
			break; /* shrank under us; use what we have */
		got += (uint64_t)r;
	}
	*buf = p;
	*size = got;
	p = NULL;
	ret = 0;
out:
	free(p);
	if (close(fd) != 0)
		pss_log("store: close %s: %s", name, strerror(errno));
	return ret;
}

uint64_t store_valid_prefix(const uint8_t *buf, uint64_t size, uint64_t *n_records)
{
	uint64_t off = 0, n = 0;

	while (size - off >= PSS_REC_HDR + PSS_REC_TRAILER) {
		uint32_t len, crc;
		uint64_t total;

		memcpy(&len, buf + off, sizeof len);
		if (len > PSS_REC_MAX_PAYLOAD)
			break;
		total = PSS_REC_HDR + (uint64_t)len + PSS_REC_TRAILER;
		if (size - off < total)
			break;
		memcpy(&crc, buf + off + PSS_REC_HDR + len, sizeof crc);
		if (crc != pss_crc32(buf + off, PSS_REC_HDR + len))
			break;
		off += total;
		n++;
	}
	if (n_records != NULL)
		*n_records = n;
	return off;
}

static int visit_records(uint32_t seg_no, const uint8_t *buf, uint64_t valid, store_visit_fn fn, void *arg)
{
	uint64_t off = 0;

	while (off < valid) {
		uint32_t len;
		uint16_t type, ver;
		int r;

		memcpy(&len, buf + off, sizeof len);
		memcpy(&type, buf + off + 4, sizeof type);
		memcpy(&ver, buf + off + 6, sizeof ver);
		r = fn(seg_no, type, ver, buf + off + PSS_REC_HDR, len, arg);
		if (r != 0)
			return r;
		off += PSS_REC_HDR + (uint64_t)len + PSS_REC_TRAILER;
	}
	return 0;
}

static int open_visitor(uint32_t seg_no, uint16_t type, uint16_t ver, const void *payload, uint32_t len, void *arg)
{
	struct pss_store *s = arg;

	(void)seg_no;
	(void)ver;
	if (type == PSS_REC_EVENT && len == sizeof(struct pss_rec_event)) {
		struct pss_rec_event ev;

		memcpy(&ev, payload, sizeof ev);
		if (ev.event_id >= s->next_event_id)
			s->next_event_id = ev.event_id + 1;
	} else if (type == PSS_REC_BOOT && len == sizeof(struct pss_rec_boot)) {
		struct pss_rec_boot b;

		memcpy(&b, payload, sizeof b);
		if (b.boot_no > s->boot_no)
			s->boot_no = b.boot_no;
	}
	return 0;
}

static int open_dir_at(int parent_fd, const char *name, int create)
{
	int fd;

	if (create && mkdirat(parent_fd, name, 0755) != 0 && errno != EEXIST) {
		pss_log("store: mkdir %s: %s", name, strerror(errno));
		return -1;
	}
	fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		pss_log("store: open %s: %s", name, strerror(errno));
	return fd;
}

/* Creates segment `no` and makes its directory entry durable. */
static int create_segment(struct pss_store *s, uint32_t no)
{
	char name[16];
	int fd;

	seg_name(name, no);
	fd = openat(s->log_dir_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0) {
		pss_log("store: create %s: %s", name, strerror(errno));
		return -1;
	}
	if (fsync(s->log_dir_fd) != 0) {
		pss_log("store: fsync log/: %s", strerror(errno));
		close(fd);
		return -1;
	}
	s->seg_fd = fd;
	s->seg_no = no;
	s->seg_size = 0;
	return 0;
}

int store_open(struct pss_store *s, const char *root)
{
	uint32_t *segs = NULL;
	size_t nsegs = 0;
	uint8_t *buf = NULL;
	uint64_t size = 0, valid = 0, nrec = 0, max_clip = 0;
	char name[16];
	int state_fd, ret = -1;

	memset(s, 0, sizeof *s);
	s->root_fd = s->log_dir_fd = s->clips_dir_fd = s->seg_fd = -1;
	s->next_event_id = 1;

	if (mkdir(root, 0755) != 0 && errno != EEXIST) {
		pss_log("store: mkdir %s: %s", root, strerror(errno));
		goto out;
	}
	s->root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (s->root_fd < 0) {
		pss_log("store: open %s: %s", root, strerror(errno));
		goto out;
	}
	s->log_dir_fd = open_dir_at(s->root_fd, "log", 1);
	if (s->log_dir_fd < 0)
		goto out;
	s->clips_dir_fd = open_dir_at(s->root_fd, "clips", 1);
	if (s->clips_dir_fd < 0)
		goto out;
	state_fd = open_dir_at(s->root_fd, "state", 1);
	if (state_fd < 0)
		goto out;
	if (close(state_fd) != 0)
		pss_log("store: close state/: %s", strerror(errno));

	if (list_segments(s->log_dir_fd, &segs, &nsegs) != 0)
		goto out;

	for (size_t i = 0; i < nsegs; i++) {
		if (read_segment(s->log_dir_fd, segs[i], &buf, &size) != 0)
			goto out;
		valid = store_valid_prefix(buf, size, &nrec);
		if (i + 1 < nsegs && valid < size)
			pss_log("store: WARNING seg=%" PRIu32 " (not newest) has %" PRIu64
				" bytes after its last valid record; left as is", segs[i], size - valid);
		visit_records(segs[i], buf, valid, open_visitor, s);
		free(buf);
		buf = NULL;
	}

	if (nsegs == 0) {
		if (create_segment(s, 1) != 0)
			goto out;
	} else {
		uint32_t newest = segs[nsegs - 1];

		seg_name(name, newest);
		s->seg_fd = openat(s->log_dir_fd, name, O_WRONLY | O_APPEND | O_CLOEXEC);
		if (s->seg_fd < 0) {
			pss_log("store: open %s for append: %s", name, strerror(errno));
			goto out;
		}
		/* valid/size/nrec still describe the newest segment from the loop. */
		if (valid < size) {
			if (ftruncate(s->seg_fd, (off_t)valid) != 0) {
				pss_log("store: ftruncate %s: %s", name, strerror(errno));
				goto out;
			}
			if (fsync(s->seg_fd) != 0) {
				pss_log("store: fsync %s: %s", name, strerror(errno));
				goto out;
			}
		}
		s->seg_no = newest;
		s->seg_size = valid;
	}
	s->recovered.seg_no = s->seg_no;
	s->recovered.valid_records = nsegs ? nrec : 0;
	s->recovered.truncated_bytes = nsegs ? size - valid : 0;
	pss_log("store: recovered seg=%" PRIu32 " valid=%" PRIu64 " truncated_bytes=%" PRIu64,
		s->recovered.seg_no, s->recovered.valid_records, s->recovered.truncated_bytes);

	if (clip_cleanup_tmp(s->clips_dir_fd, &s->recovered.tmp_removed) != 0)
		goto out;
	if (clip_max_id(s->clips_dir_fd, &max_clip) != 0)
		goto out;
	if (max_clip >= s->next_event_id)
		s->next_event_id = max_clip + 1;
	ret = 0;
out:
	free(buf);
	free(segs);
	if (ret != 0)
		store_close(s);
	return ret;
}

static int write_all(int fd, const uint8_t *p, size_t len)
{
	while (len > 0) {
		ssize_t w = write(fd, p, len);

		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0)
			return -1;
		p += w;
		len -= (size_t)w;
	}
	return 0;
}

static int rollover(struct pss_store *s)
{
	char name[16];

	seg_name(name, s->seg_no);
	if (fdatasync(s->seg_fd) != 0) {
		pss_log("store: fdatasync %s: %s", name, strerror(errno));
		return -1;
	}
	if (close(s->seg_fd) != 0) {
		pss_log("store: close %s: %s", name, strerror(errno));
		s->seg_fd = -1;
		return -1;
	}
	s->seg_fd = -1;
	if (create_segment(s, s->seg_no + 1) != 0)
		return -1;
	pss_log("store: rolled over to seg=%" PRIu32, s->seg_no);
	return 0;
}

int store_append(struct pss_store *s, uint16_t type, const void *payload, uint32_t len)
{
	uint16_t ver = PSS_REC_VER;
	uint32_t crc;
	size_t total = PSS_REC_HDR + (size_t)len + PSS_REC_TRAILER;

	if (len > PSS_REC_MAX_PAYLOAD) {
		pss_log("store: record type %u too large (%" PRIu32 " bytes)", type, len);
		errno = EMSGSIZE;
		return -1;
	}
	if (s->seg_fd < 0) {
		pss_log("store: append with no open segment");
		errno = EBADF;
		return -1;
	}
	if (s->seg_size > 0 && s->seg_size + total > PSS_SEG_MAX && rollover(s) != 0)
		return -1;

	memcpy(s->buf, &len, 4);
	memcpy(s->buf + 4, &type, 2);
	memcpy(s->buf + 6, &ver, 2);
	if (len > 0)
		memcpy(s->buf + PSS_REC_HDR, payload, len);
	crc = pss_crc32(s->buf, PSS_REC_HDR + len);
	memcpy(s->buf + PSS_REC_HDR + len, &crc, 4);

	if (write_all(s->seg_fd, s->buf, total) != 0) {
		int saved = errno;

		pss_log("store: write seg=%" PRIu32 ": %s", s->seg_no, strerror(saved));
		/* Don't leave a torn record for the next append to follow. */
		if (ftruncate(s->seg_fd, (off_t)s->seg_size) != 0)
			pss_log("store: ftruncate after failed write: %s", strerror(errno));
		errno = saved;
		return -1;
	}
	s->seg_size += total;
	return 0;
}

int store_sync(struct pss_store *s)
{
	if (fdatasync(s->seg_fd) != 0) {
		pss_log("store: fdatasync seg=%" PRIu32 ": %s", s->seg_no, strerror(errno));
		return -1;
	}
	return 0;
}

static void close_fd(int *fd, const char *what)
{
	if (*fd >= 0 && close(*fd) != 0)
		pss_log("store: close %s: %s", what, strerror(errno));
	*fd = -1;
}

void store_close(struct pss_store *s)
{
	close_fd(&s->seg_fd, "segment");
	close_fd(&s->clips_dir_fd, "clips/");
	close_fd(&s->log_dir_fd, "log/");
	close_fd(&s->root_fd, "data dir");
}

int store_scan(const char *root, store_visit_fn fn, void *arg)
{
	uint32_t *segs = NULL;
	size_t nsegs = 0;
	uint8_t *buf = NULL;
	uint64_t size, valid;
	int root_fd, log_fd = -1, ret = -1;

	root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root_fd < 0) {
		pss_log("store: open %s: %s", root, strerror(errno));
		return -1;
	}
	log_fd = open_dir_at(root_fd, "log", 0);
	if (log_fd < 0)
		goto out;
	if (list_segments(log_fd, &segs, &nsegs) != 0)
		goto out;
	ret = 0;
	for (size_t i = 0; i < nsegs && ret == 0; i++) {
		if (read_segment(log_fd, segs[i], &buf, &size) != 0) {
			ret = -1;
			break;
		}
		valid = store_valid_prefix(buf, size, NULL);
		ret = visit_records(segs[i], buf, valid, fn, arg);
		free(buf);
		buf = NULL;
	}
out:
	free(segs);
	close_fd(&log_fd, "log/");
	close_fd(&root_fd, "data dir");
	return ret;
}
