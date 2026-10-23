/*
 * pss: user CLI (Design.md §7). M3 has `pss events`, which reads the event log
 * straight from disk (read-only, through the store's scanner). The hub query
 * socket and timeline/status/health come later.
 */
#include "common/util.h"
#include "store/clip.h"
#include "store/evlog.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct print_ctx {
	int clips_fd;
	int all;
	uint64_t events, boots, stats, unknown;
};

static void usage(void)
{
	fprintf(stderr, "usage: pss events [--all] [--data=DIR]\n"
			"  --all   also show per-minute stats records\n"
			"  --data  data directory (default $PSS_DATA_DIR or /var/lib/pss)\n");
}

static void fmt_wall(char *buf, size_t n, int64_t wall_ns)
{
	time_t t = (time_t)(wall_ns / 1000000000LL);
	struct tm tm;

	if (localtime_r(&t, &tm) == NULL || strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm) == 0)
		snprintf(buf, n, "%-19s", "(bad time)");
}

static void fmt_dur(char *buf, size_t n, int64_t ns)
{
	int64_t s = (ns + 500000000LL) / 1000000000LL;

	if (s >= 60)
		snprintf(buf, n, "%" PRId64 "m%02" PRId64 "s", s / 60, s % 60);
	else
		snprintf(buf, n, "%" PRId64 "s", s);
}

static int print_visitor(uint32_t seg, uint16_t type, uint16_t ver, const void *p, uint32_t len, void *arg)
{
	struct print_ctx *c = arg;
	char when[32], dur[32], clip[48], where[64];

	(void)seg;
	if (ver != PSS_REC_VER) {
		c->unknown++;
		return 0;
	}
	if (type == PSS_REC_EVENT && len == sizeof(struct pss_rec_event)) {
		struct pss_rec_event ev;

		memcpy(&ev, p, sizeof ev);
		fmt_wall(when, sizeof when, ev.start_wall_ns);
		fmt_dur(dur, sizeof dur, ev.end_mono_ns - ev.start_mono_ns);
		if (ev.n_images > 0 && clip_final_name(clip, sizeof clip, ev.event_id) == 0)
			snprintf(where, sizeof where, faccessat(c->clips_fd, clip, F_OK, 0) == 0
				 ? "clips/%s" : "clips/%s (evicted)", clip);
		else
			snprintf(where, sizeof where, "-");
		printf("%s  %-7s #%-4" PRIu64 " %7s  edges=%-3" PRIu32 " images=%-3" PRIu32 " %s%s%s%s\n",
		       when, ev.kind == PSS_EV_MOTION_ONLY ? "MOTION" : ev.kind == PSS_EV_ENTRY ? "ENTRY" : "SOUND",
		       ev.event_id, dur, ev.pir_edges, ev.n_images, where,
		       (ev.flags & PSS_EVF_NO_IMAGES) ? "  [no-images]" : "",
		       (ev.flags & PSS_EVF_AUDIO_OFFLINE) ? "  [audio-offline]" : "",
		       (ev.flags & PSS_EVF_CLOSED_AT_SHUTDOWN) ? "  [closed-at-shutdown]" : "");
		c->events++;
	} else if (type == PSS_REC_BOOT && len == sizeof(struct pss_rec_boot)) {
		struct pss_rec_boot b;

		memcpy(&b, p, sizeof b);
		fmt_wall(when, sizeof when, b.wall_ns);
		printf("%s  BOOT    #%" PRIu64 "\n", when, b.boot_no);
		c->boots++;
	} else if (type == PSS_REC_STATS && len == sizeof(struct pss_rec_stats)) {
		struct pss_rec_stats s;

		c->stats++;
		if (!c->all)
			return 0;
		memcpy(&s, p, sizeof s);
		fmt_wall(when, sizeof when, s.wall_ns);
		printf("%s  stats   edges=%" PRIu64 " bursts=%" PRIu64 " cam_fail=%" PRIu64
		       " mic_frames=%" PRIu64 " sensors=%s%s%s\n",
		       when, s.pir_edges, s.cam_bursts, s.cam_failures, s.mic_frames,
		       (s.sensors_ok & (1u << 1)) ? "pir " : "", (s.sensors_ok & (1u << 2)) ? "cam " : "",
		       (s.sensors_ok & (1u << 3)) ? "mic" : "");
	} else {
		c->unknown++;
	}
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "all", no_argument, NULL, 'a' },
		{ "data", required_argument, NULL, 'd' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct print_ctx c = { .clips_fd = -1 };
	const char *data = NULL;
	char clips[PATH_MAX];
	int opt, r;

	if (argc < 2 || strcmp(argv[1], "events") != 0) {
		usage();
		return 2;
	}
	optind = 2;
	while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (opt) {
		case 'a': c.all = 1; break;
		case 'd': data = optarg; break;
		default: usage(); return opt == 'h' ? 0 : 2;
		}
	}
	if (data == NULL)
		data = pss_data_dir();

	r = snprintf(clips, sizeof clips, "%s/clips", data);
	if (r < 0 || (size_t)r >= sizeof clips) {
		fprintf(stderr, "pss: data path too long\n");
		return 1;
	}
	c.clips_fd = open(clips, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (c.clips_fd < 0)
		fprintf(stderr, "pss: %s: %s (clip status unknown)\n", clips, strerror(errno));

	r = store_scan(data, print_visitor, &c);
	if (c.clips_fd >= 0)
		close(c.clips_fd);
	if (r != 0) {
		fprintf(stderr, "pss: could not read the event log in %s\n", data);
		return 1;
	}
	printf("-- %" PRIu64 " events, %" PRIu64 " boots, %" PRIu64 " stats records%s\n",
	       c.events, c.boots, c.stats, c.unknown ? " (some records of unknown type skipped)" : "");
	return 0;
}
