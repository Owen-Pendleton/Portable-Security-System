/* Mechanism D: log framing, recovery, rollover, events/stats split,
 * interrupted ENTRYs, stats retention, clip commit, quota. */
#include "t.h"

#include "store/clip.h"
#include "store/crc32.h"
#include "store/evlog.h"

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct counts {
	int boots, events, stats, opens, interrupted, other;
	uint64_t ids[8];
	uint32_t flags[8];
	uint32_t images[8];
};

static int count_visitor(uint32_t seg, uint16_t type, uint16_t ver, const void *p, uint32_t len, void *arg)
{
	struct counts *c = arg;

	(void)seg;
	(void)ver;
	if (type == PSS_REC_EVENT && len == sizeof(struct pss_rec_event)) {
		struct pss_rec_event ev;

		memcpy(&ev, p, sizeof ev);
		if (c->events < 8) {
			c->ids[c->events] = ev.event_id;
			c->flags[c->events] = ev.flags;
			c->images[c->events] = ev.n_images;
		}
		if (ev.flags & PSS_EVF_INTERRUPTED)
			c->interrupted++;
		c->events++;
	} else if (type == PSS_REC_EVENT_OPEN) {
		c->opens++;
	} else if (type == PSS_REC_BOOT) {
		c->boots++;
	} else if (type == PSS_REC_STATS) {
		c->stats++;
	} else {
		c->other++;
	}
	return 0;
}

static int append_event(struct pss_store *s, uint64_t id)
{
	struct pss_rec_event ev;

	memset(&ev, 0, sizeof ev);
	ev.event_id = id;
	ev.kind = PSS_EV_MOTION_ONLY;
	return store_append(s, PSS_LOG_EVENTS, PSS_REC_EVENT, &ev, sizeof ev);
}

static int append_stats(struct pss_store *s)
{
	struct pss_rec_stats st;

	memset(&st, 0, sizeof st);
	return store_append(s, PSS_LOG_STATS, PSS_REC_STATS, &st, sizeof st);
}

static int open_entry(struct pss_store *s, uint64_t id)
{
	struct pss_rec_event_open o;

	memset(&o, 0, sizeof o);
	o.event_id = id;
	o.start_mono_ns = 1000;
	o.start_wall_ns = 2000;
	o.kind = PSS_EV_ENTRY;
	return store_event_open(s, &o);
}

static void seg_path(char *buf, size_t n, const char *root, const char *log, uint32_t no)
{
	snprintf(buf, n, "%s/%s/seg-%06u.pel", root, log, no);
}

static void write_file(const char *path, const void *data, size_t len, int flags)
{
	int fd = open(path, O_WRONLY | O_CREAT | flags, 0644);

	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(write(fd, data, len) == (ssize_t)len);
	close(fd);
}

static int exists(const char *root, const char *rel)
{
	char path[512];

	snprintf(path, sizeof path, "%s/%s", root, rel);
	return access(path, F_OK) == 0;
}

static void test_crc_known_value(void)
{
	CHECK(pss_crc32("123456789", 9) == 0xCBF43926u);
}

static void test_append_and_scan(void)
{
	char root[256];
	struct pss_store s;
	struct counts c = { 0 };

	t_mkdtemp(root);
	CHECK(store_open(&s, root) == 0);
	CHECK(s.next_event_id == 1);
	CHECK(append_event(&s, 1) == 0);
	CHECK(append_event(&s, 2) == 0);
	CHECK(append_event(&s, 3) == 0);
	CHECK(store_sync(&s, PSS_LOG_EVENTS) == 0);
	store_close(&s);

	CHECK(store_scan(root, PSS_LOG_EVENTS, count_visitor, &c) == 0);
	CHECK(c.events == 3);
	CHECK(c.ids[0] == 1 && c.ids[1] == 2 && c.ids[2] == 3);
	CHECK(c.other == 0);

	CHECK(store_open(&s, root) == 0);
	CHECK(s.next_event_id == 4);
	CHECK(s.recovered.valid_records == 3);
	CHECK(s.recovered.truncated_bytes == 0);
	store_close(&s);
	CHECK(exists(root, "events") && exists(root, "stats") && exists(root, "clips"));
	CHECK(!exists(root, "log"));
	t_rm_rf(root);
}

static void test_torn_tail_is_truncated(void)
{
	char root[256], seg[300];
	const uint8_t garbage[] = { 0x30, 0, 0, 0, 2, 0, 1, 0, 0xAA, 0xBB, 0xCC };
	struct pss_store s;
	struct counts c = { 0 };

	t_mkdtemp(root);
	CHECK(store_open(&s, root) == 0);
	CHECK(append_event(&s, 1) == 0);
	CHECK(append_event(&s, 2) == 0);
	store_close(&s);

	/* A power cut mid-write: header promises 48 bytes, only 3 made it. */
	seg_path(seg, sizeof seg, root, "events", 1);
	write_file(seg, garbage, sizeof garbage, O_APPEND);

	CHECK(store_open(&s, root) == 0);
	CHECK(s.recovered.valid_records == 2);
	CHECK(s.recovered.truncated_bytes == sizeof garbage);
	CHECK(append_event(&s, 3) == 0);
	store_close(&s);

	CHECK(store_scan(root, PSS_LOG_EVENTS, count_visitor, &c) == 0);
	CHECK(c.events == 3);
	CHECK(c.ids[2] == 3);
	t_rm_rf(root);
}

static void test_bad_crc_stops_recovery(void)
{
	char root[256], seg[300];
	struct pss_store s;
	struct counts c = { 0 };
	const uint64_t rec = PSS_REC_HDR + sizeof(struct pss_rec_event) + PSS_REC_TRAILER;
	uint8_t flip = 0xFF;
	int fd;

	t_mkdtemp(root);
	CHECK(store_open(&s, root) == 0);
	CHECK(append_event(&s, 1) == 0);
	CHECK(append_event(&s, 2) == 0);
	CHECK(append_event(&s, 3) == 0);
	store_close(&s);

	/* Corrupt one payload byte of record 2. */
	seg_path(seg, sizeof seg, root, "events", 1);
	fd = open(seg, O_WRONLY);
	CHECK(fd >= 0);
	CHECK(pwrite(fd, &flip, 1, (off_t)(rec + PSS_REC_HDR + 3)) == 1);
	close(fd);

	CHECK(store_open(&s, root) == 0);
	CHECK(s.recovered.valid_records == 1);
	CHECK(s.recovered.truncated_bytes == 2 * rec);
	CHECK(s.next_event_id == 2);
	store_close(&s);

	CHECK(store_scan(root, PSS_LOG_EVENTS, count_visitor, &c) == 0);
	CHECK(c.events == 1);
	t_rm_rf(root);
}

static void test_zero_filled_tail(void)
{
	char root[256], seg[300];
	uint8_t zeros[512] = { 0 };
	struct pss_store s;

	t_mkdtemp(root);
	CHECK(store_open(&s, root) == 0);
	CHECK(append_event(&s, 1) == 0);
	store_close(&s);
	seg_path(seg, sizeof seg, root, "events", 1);
	write_file(seg, zeros, sizeof zeros, O_APPEND);

	CHECK(store_open(&s, root) == 0);
	CHECK(s.recovered.valid_records == 1);
	CHECK(s.recovered.truncated_bytes == sizeof zeros);
	store_close(&s);
	t_rm_rf(root);
}

static void test_rollover(void)
{
	char root[256], seg[300];
	static uint8_t big[PSS_REC_MAX_PAYLOAD];
	struct pss_store s;
	struct counts c = { 0 };
	const int n = 1100; /* 1100 * 4108 B > 4 MiB */
	int ok = 1;

	t_mkdtemp(root);
	CHECK(store_open(&s, root) == 0);
	for (int i = 0; i < n; i++)
		ok &= store_append(&s, PSS_LOG_STATS, PSS_REC_STATS, big, sizeof big) == 0;
	CHECK(ok);
	CHECK(s.stats.seg_no == 2);
	store_close(&s);

	seg_path(seg, sizeof seg, root, "stats", 2);
	CHECK(access(seg, F_OK) == 0);
	CHECK(store_scan(root, PSS_LOG_STATS, count_visitor, &c) == 0);
	CHECK(c.stats == n);
	t_rm_rf(root);
}

static void test_stats_kept_apart_from_events(void)
{
	char root[256], seg[300];
	const uint8_t garbage[] = { 0x48, 0, 0, 0, 3, 0, 1, 0, 0x01 };
	struct pss_store s;
	struct counts ev = { 0 }, st = { 0 };

	t_mkdtemp(root);
	CHECK(store_open(&s, root) == 0);
	CHECK(append_event(&s, 1) == 0);
	CHECK(append_stats(&s) == 0);
	CHECK(append_stats(&s) == 0);
	/* Each record type has one home. */
	CHECK(store_append(&s, PSS_LOG_EVENTS, PSS_REC_STATS, "x", 1) == -1);
	CHECK(store_append(&s, PSS_LOG_STATS, PSS_REC_EVENT, "x", 1) == -1);
	store_close(&s);

	/* Torn tail in the stats log is recovered too. */
	seg_path(seg, sizeof seg, root, "stats", 1);
	write_file(seg, garbage, sizeof garbage, O_APPEND);
	CHECK(store_open(&s, root) == 0);
	CHECK(s.recovered.stats_valid_records == 2);
	CHECK(s.recovered.stats_truncated_bytes == sizeof garbage);
	CHECK(s.stats.seg_no == 2); /* fresh stats segment per boot */
	store_close(&s);

	CHECK(store_scan(root, PSS_LOG_EVENTS, count_visitor, &ev) == 0);
	CHECK(store_scan(root, PSS_LOG_STATS, count_visitor, &st) == 0);
	CHECK(ev.events == 1 && ev.stats == 0);
	CHECK(st.stats == 2 && st.events == 0);
	t_rm_rf(root);
}

static void test_open_without_close_is_interrupted(void)
{
	char root[256], path[512];
	struct pss_store s;
	struct counts c = { 0 };

	t_mkdtemp(root);
	CHECK(store_open(&s, root) == 0);
	CHECK(open_entry(&s, 1) == 0);
	store_close(&s); /* power lost before the close */

	/* Images committed before the cut are kept with the event. */
	snprintf(path, sizeof path, "%s/clips/1.dir", root);
	CHECK(mkdir(path, 0755) == 0);
	snprintf(path, sizeof path, "%s/clips/1.dir/img0000.jpg", root);
	write_file(path, "j", 1, O_TRUNC);

	CHECK(store_open(&s, root) == 0);
	CHECK(s.recovered.interrupted == 1);
	CHECK(s.next_event_id == 2);
	store_close(&s);

	/* A second restart must not close it again. */
	CHECK(store_open(&s, root) == 0);
	CHECK(s.recovered.interrupted == 0);
	store_close(&s);

	CHECK(store_scan(root, PSS_LOG_EVENTS, count_visitor, &c) == 0);
	CHECK(c.opens == 1);
	CHECK(c.events == 1 && c.interrupted == 1);
	CHECK(c.ids[0] == 1);
	CHECK(c.images[0] == 1);
	t_rm_rf(root);
}

static void test_open_and_close_not_interrupted(void)
{
	char root[256];
	struct pss_store s;
	struct pss_rec_event e;
	struct counts c = { 0 };

	t_mkdtemp(root);
	CHECK(store_open(&s, root) == 0);
	CHECK(open_entry(&s, 1) == 0);
	memset(&e, 0, sizeof e);
	e.event_id = 1;
	e.kind = PSS_EV_ENTRY;
	CHECK(store_event_close(&s, &e) == 0);
	store_close(&s);

	CHECK(store_open(&s, root) == 0);
	CHECK(s.recovered.interrupted == 0);
	store_close(&s);

	CHECK(store_scan(root, PSS_LOG_EVENTS, count_visitor, &c) == 0);
	CHECK(c.opens == 1 && c.events == 1 && c.interrupted == 0);
	t_rm_rf(root);
}

static void test_stats_prune_old_segments(void)
{
	char root[256], seg[300];
	struct pss_store s;
	struct timespec old[2];

	t_mkdtemp(root);
	CHECK(store_open(&s, root) == 0);	/* stats seg 1 */
	CHECK(append_stats(&s) == 0);
	store_close(&s);
	CHECK(store_open(&s, root) == 0);	/* seg 1 non-empty -> fresh seg 2 */
	CHECK(append_stats(&s) == 0);
	store_close(&s);

	/* Seg 1 last written 8 days ago; seg 2 is recent. */
	seg_path(seg, sizeof seg, root, "stats", 1);
	old[0].tv_sec = old[1].tv_sec = time(NULL) - 8 * 24 * 3600;
	old[0].tv_nsec = old[1].tv_nsec = 0;
	CHECK(utimensat(AT_FDCWD, seg, old, 0) == 0);

	CHECK(store_open(&s, root) == 0);	/* -> fresh seg 3, prune */
	CHECK(s.recovered.stats_pruned == 1);
	store_close(&s);
	CHECK(!exists(root, "stats/seg-000001.pel"));
	CHECK(exists(root, "stats/seg-000002.pel"));
	CHECK(exists(root, "stats/seg-000003.pel"));
	t_rm_rf(root);
}

static void test_stats_roll_by_age(void)
{
	char root[256];
	struct pss_store s;
	struct counts c = { 0 };

	t_mkdtemp(root);
	CHECK(store_open(&s, root) == 0);
	s.stats_roll_ns = 1; /* every non-empty segment is "a day old" */
	CHECK(append_stats(&s) == 0);
	CHECK(append_stats(&s) == 0);
	CHECK(append_stats(&s) == 0);
	CHECK(s.stats.seg_no == 3);
	store_close(&s);
	CHECK(store_scan(root, PSS_LOG_STATS, count_visitor, &c) == 0);
	CHECK(c.stats == 3);
	t_rm_rf(root);
}

static void make_burst(const char *root, const char *name, int files, size_t bytes)
{
	char path[512];
	static uint8_t data[4096];

	snprintf(path, sizeof path, "%s/clips/%s", root, name);
	CHECK(mkdir(path, 0755) == 0);
	for (int i = 0; i < files; i++) {
		snprintf(path, sizeof path, "%s/clips/%s/img%04d.jpg", root, name, i);
		write_file(path, data, bytes, O_TRUNC);
	}
}

static void test_clip_commit(void)
{
	char root[256];
	struct pss_store s;
	uint32_t n = 0;

	t_mkdtemp(root);
	CHECK(store_open(&s, root) == 0);
	make_burst(root, "4.dir.tmp", 2, 100);
	CHECK(clip_commit(s.clips_dir_fd, 4, &n) == 0);
	CHECK(n == 2);
	CHECK(exists(root, "clips/4.dir/img0001.jpg"));
	CHECK(!exists(root, "clips/4.dir.tmp"));
	CHECK(clip_count_images(s.clips_dir_fd, 4, &n) == 0 && n == 2);
	CHECK(clip_count_images(s.clips_dir_fd, 5, &n) == 0 && n == 0);
	store_close(&s);

	/* A committed clip id must never be reused, even with no event record. */
	CHECK(store_open(&s, root) == 0);
	CHECK(s.next_event_id == 5);
	store_close(&s);
	t_rm_rf(root);
}

static void test_tmp_cleanup_on_open(void)
{
	char root[256], path[512];
	struct pss_store s;

	t_mkdtemp(root);
	CHECK(store_open(&s, root) == 0);
	store_close(&s);
	make_burst(root, "3.dir.tmp", 2, 10);
	snprintf(path, sizeof path, "%s/clips/7.wav.tmp", root);
	write_file(path, "x", 1, O_TRUNC);
	make_burst(root, "2.dir", 1, 10);

	CHECK(store_open(&s, root) == 0);
	CHECK(s.recovered.tmp_removed == 2);
	CHECK(!exists(root, "clips/3.dir.tmp"));
	CHECK(!exists(root, "clips/7.wav.tmp"));
	CHECK(exists(root, "clips/2.dir"));
	CHECK(clip_discard(s.clips_dir_fd, 99) == 0); /* absent is fine */
	store_close(&s);
	t_rm_rf(root);
}

static void test_quota_evicts_oldest(void)
{
	char root[256];
	struct pss_store s;
	uint32_t evicted = 0;

	t_mkdtemp(root);
	CHECK(store_open(&s, root) == 0);
	make_burst(root, "1.dir", 1, 1000);
	make_burst(root, "2.dir", 1, 1000);
	make_burst(root, "10.dir", 1, 1000);

	/* 3000 used + 1000 needed > 2500: evict 1 then 2 (numeric order, not "10" < "2"). */
	CHECK(clip_enforce_quota(s.clips_dir_fd, 2500, 1000, 0, &evicted) == 0);
	CHECK(evicted == 2);
	CHECK(!exists(root, "clips/1.dir"));
	CHECK(!exists(root, "clips/2.dir"));
	CHECK(exists(root, "clips/10.dir"));

	/* An impossible floor disables clips without evicting more. */
	CHECK(clip_enforce_quota(s.clips_dir_fd, 1u << 30, 0, 101, &evicted) == 1);
	CHECK(exists(root, "clips/10.dir"));
	store_close(&s);
	t_rm_rf(root);
}

int main(void)
{
	T_RUN(test_crc_known_value);
	T_RUN(test_append_and_scan);
	T_RUN(test_torn_tail_is_truncated);
	T_RUN(test_bad_crc_stops_recovery);
	T_RUN(test_zero_filled_tail);
	T_RUN(test_rollover);
	T_RUN(test_stats_kept_apart_from_events);
	T_RUN(test_open_without_close_is_interrupted);
	T_RUN(test_open_and_close_not_interrupted);
	T_RUN(test_stats_prune_old_segments);
	T_RUN(test_stats_roll_by_age);
	T_RUN(test_clip_commit);
	T_RUN(test_tmp_cleanup_on_open);
	T_RUN(test_quota_evicts_oldest);
	T_END();
}
