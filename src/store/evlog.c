#include "store/evlog.h"

#include "common/log.h"
#include "common/time.h"
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
#include <time.h>
#include <unistd.h>

static const char *log_name(enum pss_log_which which)
{
	return which == PSS_LOG_STATS ? "stats" : "events";
}

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

/* Sorted segment numbers in dir_fd. *out is malloc'd (NULL if none). */
static int list_segments(int dir_fd, uint32_t **out, size_t *n)
{
	struct dirlist dl;
	uint32_t *nos = NULL;
	size_t count = 0;

	*out = NULL;
	*n = 0;
	if (dirlist_read(dir_fd, &dl) != 0)
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

static int read_segment(int dir_fd, const char *logname, uint32_t no, uint8_t **buf, uint64_t *size)
{
	char name[16];
	struct stat st;
	uint8_t *p = NULL;
	uint64_t got = 0;
	int fd, ret = -1;

	seg_name(name, no);
	fd = openat(dir_fd, name, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		pss_log("store: open %s/%s: %s", logname, name, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) != 0) {
		pss_log("store: fstat %s/%s: %s", logname, name, strerror(errno));
		goto out;
	}
	p = malloc(st.st_size > 0 ? (size_t)st.st_size : 1);
	if (p == NULL) {
		pss_log("store: out of memory reading %s/%s", logname, name);
		goto out;
	}
	while (got < (uint64_t)st.st_size) {
		ssize_t r = read(fd, p + got, (size_t)st.st_size - got);

		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			pss_log("store: read %s/%s: %s", logname, name, strerror(errno));
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
		pss_log("store: close %s/%s: %s", logname, name, strerror(errno));
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

/* What store_open learns from the events log. */
struct open_scan {
	struct pss_store *s;
	struct pss_rec_event_open *opens;	/* EVENT_OPENs with no EVENT yet */
	size_t n, cap;
};

static void bump_next_id(struct pss_store *s, uint64_t id)
{
	if (id >= s->next_event_id)
		s->next_event_id = id + 1;
}

static int open_visitor(uint32_t seg_no, uint16_t type, uint16_t ver, const void *payload, uint32_t len, void *arg)
{
	struct open_scan *scan = arg;
	struct pss_store *s = scan->s;

	(void)seg_no;
	(void)ver;
	if (type == PSS_REC_EVENT && len == sizeof(struct pss_rec_event)) {
		struct pss_rec_event ev;

		memcpy(&ev, payload, sizeof ev);
		bump_next_id(s, ev.event_id);
		for (size_t i = 0; i < scan->n; i++) {
			if (scan->opens[i].event_id == ev.event_id) {
				scan->opens[i] = scan->opens[--scan->n];
				break;
			}
		}
	} else if (type == PSS_REC_EVENT_OPEN && len == sizeof(struct pss_rec_event_open)) {
		if (scan->n == scan->cap) {
			size_t cap = scan->cap ? scan->cap * 2 : 4;
			struct pss_rec_event_open *p = realloc(scan->opens, cap * sizeof *p);

			if (p == NULL) {
				pss_log("store: out of memory tracking open events");
				return -1;
			}
			scan->opens = p;
			scan->cap = cap;
		}
		memcpy(&scan->opens[scan->n], payload, sizeof scan->opens[0]);
		bump_next_id(s, scan->opens[scan->n].event_id);
		scan->n++;
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
static int create_segment(struct seglog *l, uint32_t no)
{
	char name[16];
	int fd;

	seg_name(name, no);
	fd = openat(l->dir_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0) {
		pss_log("store: create %s/%s: %s", l->name, name, strerror(errno));
		return -1;
	}
	if (fsync(l->dir_fd) != 0) {
		pss_log("store: fsync %s/: %s", l->name, strerror(errno));
		close(fd);
		return -1;
	}
	l->seg_fd = fd;
	l->seg_no = no;
	l->seg_size = 0;
	l->seg_started_ns = pss_mono_ns();
	return 0;
}

static int rollover(struct seglog *l)
{
	char name[16];

	seg_name(name, l->seg_no);
	if (fdatasync(l->seg_fd) != 0) {
		pss_log("store: fdatasync %s/%s: %s", l->name, name, strerror(errno));
		return -1;
	}
	if (close(l->seg_fd) != 0) {
		pss_log("store: close %s/%s: %s", l->name, name, strerror(errno));
		l->seg_fd = -1;
		return -1;
	}
	l->seg_fd = -1;
	if (create_segment(l, l->seg_no + 1) != 0)
		return -1;
	pss_log("store: %s rolled over to seg=%" PRIu32, l->name, l->seg_no);
	return 0;
}

/*
 * Recovers one log and opens its newest segment for append: every segment is
 * validated (and visited with fn, if given), and the newest is truncated at
 * its first bad record. 0 or -1.
 */
static int seglog_open(struct seglog *l, int root_fd, const char *name, store_visit_fn fn, void *arg,
		       uint64_t *valid_records, uint64_t *truncated_bytes)
{
	uint32_t *segs = NULL;
	size_t nsegs = 0;
	uint8_t *buf = NULL;
	uint64_t size = 0, valid = 0, nrec = 0;
	char seg[16];
	int ret = -1;

	l->name = name;
	l->dir_fd = open_dir_at(root_fd, name, 1);
	if (l->dir_fd < 0)
		goto out;
	if (list_segments(l->dir_fd, &segs, &nsegs) != 0)
		goto out;

	for (size_t i = 0; i < nsegs; i++) {
		if (read_segment(l->dir_fd, name, segs[i], &buf, &size) != 0)
			goto out;
		valid = store_valid_prefix(buf, size, &nrec);
		if (i + 1 < nsegs && valid < size)
			pss_log("store: WARNING %s seg=%" PRIu32 " (not newest) has %" PRIu64
				" bytes after its last valid record; left as is", name, segs[i], size - valid);
		if (fn != NULL && visit_records(segs[i], buf, valid, fn, arg) != 0)
			goto out;
		free(buf);
		buf = NULL;
	}

	if (nsegs == 0) {
		if (create_segment(l, 1) != 0)
			goto out;
	} else {
		uint32_t newest = segs[nsegs - 1];

		seg_name(seg, newest);
		l->seg_fd = openat(l->dir_fd, seg, O_WRONLY | O_APPEND | O_CLOEXEC);
		if (l->seg_fd < 0) {
			pss_log("store: open %s/%s for append: %s", name, seg, strerror(errno));
			goto out;
		}
		/* valid/size/nrec still describe the newest segment from the loop. */
		if (valid < size) {
			if (ftruncate(l->seg_fd, (off_t)valid) != 0) {
				pss_log("store: ftruncate %s/%s: %s", name, seg, strerror(errno));
				goto out;
			}
			if (fsync(l->seg_fd) != 0) {
				pss_log("store: fsync %s/%s: %s", name, seg, strerror(errno));
				goto out;
			}
		}
		l->seg_no = newest;
		l->seg_size = valid;
		l->seg_started_ns = pss_mono_ns();
	}
	*valid_records = nsegs ? nrec : 0;
	*truncated_bytes = nsegs ? size - valid : 0;
	pss_log("store: recovered %s seg=%" PRIu32 " valid=%" PRIu64 " truncated_bytes=%" PRIu64,
		name, l->seg_no, *valid_records, *truncated_bytes);
	ret = 0;
out:
	free(buf);
	free(segs);
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

static int seglog_append(struct seglog *l, uint16_t type, const void *payload, uint32_t len)
{
	uint16_t ver = PSS_REC_VER;
	uint32_t crc;
	size_t total = PSS_REC_HDR + (size_t)len + PSS_REC_TRAILER;

	if (len > PSS_REC_MAX_PAYLOAD) {
		pss_log("store: record type %u too large (%" PRIu32 " bytes)", type, len);
		errno = EMSGSIZE;
		return -1;
	}
	if (l->seg_fd < 0) {
		pss_log("store: append to %s with no open segment", l->name);
		errno = EBADF;
		return -1;
	}
	if (l->seg_size > 0 && l->seg_size + total > PSS_SEG_MAX && rollover(l) != 0)
		return -1;

	memcpy(l->buf, &len, 4);
	memcpy(l->buf + 4, &type, 2);
	memcpy(l->buf + 6, &ver, 2);
	if (len > 0)
		memcpy(l->buf + PSS_REC_HDR, payload, len);
	crc = pss_crc32(l->buf, PSS_REC_HDR + len);
	memcpy(l->buf + PSS_REC_HDR + len, &crc, 4);

	if (write_all(l->seg_fd, l->buf, total) != 0) {
		int saved = errno;

		pss_log("store: write %s seg=%" PRIu32 ": %s", l->name, l->seg_no, strerror(saved));
		/* Don't leave a torn record for the next append to follow. */
		if (ftruncate(l->seg_fd, (off_t)l->seg_size) != 0)
			pss_log("store: ftruncate after failed write: %s", strerror(errno));
		errno = saved;
		return -1;
	}
	l->seg_size += total;
	return 0;
}

static int seglog_sync(struct seglog *l)
{
	if (fdatasync(l->seg_fd) != 0) {
		pss_log("store: fdatasync %s seg=%" PRIu32 ": %s", l->name, l->seg_no, strerror(errno));
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

static void seglog_close(struct seglog *l)
{
	close_fd(&l->seg_fd, "segment");
	close_fd(&l->dir_fd, "log dir");
}

/* Deletes stats segments (never the current one) last written over 7 days ago. 0 or -1. */
static int stats_prune(struct pss_store *s, uint32_t *pruned)
{
	struct dirlist dl;
	struct stat st;
	time_t now = time(NULL);
	uint32_t no;
	int ret = 0;

	*pruned = 0;
	if (now == (time_t)-1) {
		pss_log("store: time: %s", strerror(errno));
		return -1;
	}
	if (dirlist_read(s->stats.dir_fd, &dl) != 0)
		return -1;
	for (size_t i = 0; i < dl.n; i++) {
		if (parse_seg_name(dl.names[i], &no) != 0 || no >= s->stats.seg_no)
			continue;
		if (fstatat(s->stats.dir_fd, dl.names[i], &st, AT_SYMLINK_NOFOLLOW) != 0) {
			pss_log("store: stat stats/%s: %s", dl.names[i], strerror(errno));
			ret = -1;
			continue;
		}
		if (now - st.st_mtime <= PSS_STATS_RETAIN_S)
			continue;
		if (unlinkat(s->stats.dir_fd, dl.names[i], 0) != 0) {
			pss_log("store: unlink stats/%s: %s", dl.names[i], strerror(errno));
			ret = -1;
			continue;
		}
		pss_log("store: pruned stats/%s (older than 7 days)", dl.names[i]);
		(*pruned)++;
	}
	dirlist_free(&dl);
	if (*pruned > 0 && fsync(s->stats.dir_fd) != 0) {
		pss_log("store: fsync stats/: %s", strerror(errno));
		ret = -1;
	}
	return ret;
}

/* Closes every ENTRY left open by a power cut, keeping any images already committed. 0 or -1. */
static int close_interrupted(struct pss_store *s, const struct open_scan *scan)
{
	for (size_t i = 0; i < scan->n; i++) {
		const struct pss_rec_event_open *o = &scan->opens[i];
		struct pss_rec_event e;

		memset(&e, 0, sizeof e);
		e.event_id = o->event_id;
		e.start_mono_ns = o->start_mono_ns;
		e.end_mono_ns = o->start_mono_ns; /* end unknown */
		e.start_wall_ns = o->start_wall_ns;
		e.kind = o->kind;
		e.flags = o->flags | PSS_EVF_INTERRUPTED;
		if (clip_count_images(s->clips_dir_fd, o->event_id, &e.n_images) != 0)
			return -1;
		if (seglog_append(&s->events, PSS_REC_EVENT, &e, sizeof e) != 0)
			return -1;
		pss_log("store: event %" PRIu64 " was open when power was lost; closed as interrupted"
			" (%" PRIu32 " images kept)", o->event_id, e.n_images);
		s->recovered.interrupted++;
	}
	if (scan->n > 0 && seglog_sync(&s->events) != 0)
		return -1;
	return 0;
}

int store_open(struct pss_store *s, const char *root)
{
	struct open_scan scan = { .s = s };
	struct store_recovery *rec = &s->recovered;
	uint64_t max_clip = 0;
	int ret = -1;

	memset(s, 0, sizeof *s);
	s->root_fd = s->clips_dir_fd = -1;
	s->events.dir_fd = s->events.seg_fd = -1;
	s->stats.dir_fd = s->stats.seg_fd = -1;
	s->stats_roll_ns = PSS_STATS_ROLL_NS;
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

	if (seglog_open(&s->events, s->root_fd, "events", open_visitor, &scan,
			&rec->valid_records, &rec->truncated_bytes) != 0)
		goto out;
	rec->seg_no = s->events.seg_no;

	if (seglog_open(&s->stats, s->root_fd, "stats", NULL, NULL,
			&rec->stats_valid_records, &rec->stats_truncated_bytes) != 0)
		goto out;
	rec->stats_seg_no = s->stats.seg_no;
	/* A fresh stats segment per boot (and per day, see store_append) keeps
	 * segments short enough to be deleted whole after 7 days. */
	if (s->stats.seg_size > 0 && rollover(&s->stats) != 0)
		goto out;
	if (stats_prune(s, &rec->stats_pruned) != 0)
		goto out;

	s->clips_dir_fd = open_dir_at(s->root_fd, "clips", 1);
	if (s->clips_dir_fd < 0)
		goto out;
	if (clip_cleanup_tmp(s->clips_dir_fd, &rec->tmp_removed) != 0)
		goto out;
	if (clip_max_id(s->clips_dir_fd, &max_clip) != 0)
		goto out;
	bump_next_id(s, max_clip);

	if (close_interrupted(s, &scan) != 0)
		goto out;
	ret = 0;
out:
	free(scan.opens);
	if (ret != 0)
		store_close(s);
	return ret;
}

int store_append(struct pss_store *s, enum pss_log_which which, uint16_t type, const void *payload, uint32_t len)
{
	struct seglog *l = which == PSS_LOG_STATS ? &s->stats : &s->events;
	uint32_t before = l->seg_no, pruned;

	if ((which == PSS_LOG_STATS) != (type == PSS_REC_STATS)) {
		pss_log("store: record type %u does not belong in the %s log", type, l->name);
		errno = EINVAL;
		return -1;
	}
	if (which == PSS_LOG_STATS && l->seg_size > 0 &&
	    pss_mono_ns() - l->seg_started_ns >= s->stats_roll_ns && rollover(l) != 0)
		return -1;
	if (seglog_append(l, type, payload, len) != 0)
		return -1;
	/* The record is written; a failed prune only delays retention, and is logged. */
	if (which == PSS_LOG_STATS && l->seg_no != before)
		stats_prune(s, &pruned);
	return 0;
}

int store_sync(struct pss_store *s, enum pss_log_which which)
{
	return seglog_sync(which == PSS_LOG_STATS ? &s->stats : &s->events);
}

int store_event_open(struct pss_store *s, const struct pss_rec_event_open *o)
{
	if (store_append(s, PSS_LOG_EVENTS, PSS_REC_EVENT_OPEN, o, sizeof *o) != 0)
		return -1;
	return seglog_sync(&s->events);
}

int store_event_close(struct pss_store *s, const struct pss_rec_event *e)
{
	if (store_append(s, PSS_LOG_EVENTS, PSS_REC_EVENT, e, sizeof *e) != 0)
		return -1;
	return seglog_sync(&s->events);
}

void store_close(struct pss_store *s)
{
	seglog_close(&s->events);
	seglog_close(&s->stats);
	close_fd(&s->clips_dir_fd, "clips/");
	close_fd(&s->root_fd, "data dir");
}

int store_scan(const char *root, enum pss_log_which which, store_visit_fn fn, void *arg)
{
	const char *name = log_name(which);
	uint32_t *segs = NULL;
	size_t nsegs = 0;
	uint8_t *buf = NULL;
	uint64_t size, valid;
	int root_fd, dir_fd = -1, ret = -1;

	root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root_fd < 0) {
		pss_log("store: open %s: %s", root, strerror(errno));
		return -1;
	}
	dir_fd = open_dir_at(root_fd, name, 0);
	if (dir_fd < 0)
		goto out;
	if (list_segments(dir_fd, &segs, &nsegs) != 0)
		goto out;
	ret = 0;
	for (size_t i = 0; i < nsegs && ret == 0; i++) {
		if (read_segment(dir_fd, name, segs[i], &buf, &size) != 0) {
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
	close_fd(&dir_fd, "log dir");
	close_fd(&root_fd, "data dir");
	return ret;
}
