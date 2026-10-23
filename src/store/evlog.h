#ifndef PSS_STORE_EVLOG_H
#define PSS_STORE_EVLOG_H

/*
 * Mechanism D, event and stats logs (DESIGN.md Section 5): two append-only
 * logs of segment files, events/seg-NNNNNN.pel and stats/seg-NNNNNN.pel,
 * each record framed as
 *     [len u32][type u16][ver u16][payload: len bytes][crc32 u32]
 * with the CRC over header + payload. Native little-endian (Pi 5 and the lab
 * machines are both LE). Recovery at open truncates the newest segment of
 * each log at the first record with a bad length or CRC, and closes any
 * ENTRY that was opened but never closed as interrupted (power lost).
 */

#include <stdint.h>

#define PSS_SEG_MAX (4u * 1024 * 1024)
#define PSS_REC_MAX_PAYLOAD 4096u
#define PSS_REC_HDR 8u
#define PSS_REC_TRAILER 4u
#define PSS_REC_VER 1
#define PSS_STATS_RETAIN_S (7 * 24 * 3600)	/* whole stats segments older than this are deleted */
#define PSS_STATS_ROLL_NS (24 * 3600 * INT64_C(1000000000)) /* so a segment can age out */

enum pss_log_which {
	PSS_LOG_EVENTS = 0,
	PSS_LOG_STATS = 1,
};

enum pss_rec_type {
	PSS_REC_BOOT = 1,	/* events log */
	PSS_REC_EVENT = 2,	/* events log: a complete event, or the close of an EVENT_OPEN */
	PSS_REC_STATS = 3,	/* stats log only */
	PSS_REC_EVENT_OPEN = 4,	/* events log: ENTRY confirmed, made durable at once */
};

enum pss_event_kind {
	PSS_EV_MOTION_ONLY = 1,
	PSS_EV_ENTRY = 2,	/* needs fusion (M4) */
	PSS_EV_SOUND_ONLY = 3,	/* needs fusion (M4) */
};

enum pss_event_flag {
	PSS_EVF_NO_IMAGES = 1u << 0,
	PSS_EVF_AUDIO_OFFLINE = 1u << 1,
	PSS_EVF_CLOSED_AT_SHUTDOWN = 1u << 2,
	PSS_EVF_INTERRUPTED = 1u << 3,	/* opened, power lost before close; end unknown */
};

/* Payload structs have no implicit padding; read them back with memcpy (payloads are unaligned). */
struct pss_rec_boot {
	uint64_t boot_no;
	int64_t mono_ns;
	int64_t wall_ns;
};

/* Images, if any, live in clips/<event_id>.dir. */
struct pss_rec_event {
	uint64_t event_id;
	int64_t start_mono_ns;
	int64_t end_mono_ns;
	int64_t start_wall_ns;
	uint32_t kind;
	uint32_t flags;
	uint32_t pir_edges;
	uint32_t n_images;
};

struct pss_rec_event_open {
	uint64_t event_id;
	int64_t start_mono_ns;
	int64_t start_wall_ns;
	uint32_t kind;
	uint32_t flags;
};

struct pss_rec_stats {
	int64_t mono_ns;
	int64_t wall_ns;
	uint64_t pir_edges;
	uint64_t mic_frames;
	uint64_t cam_bursts;
	uint64_t cam_failures;
	uint32_t sensors_ok;	/* bit (1 << role) set if that daemon heartbeated recently */
	uint32_t pad;
};

struct store_recovery {
	uint32_t seg_no;		/* events log */
	uint64_t valid_records;
	uint64_t truncated_bytes;
	uint32_t stats_seg_no;		/* stats log, before the fresh segment started at open */
	uint64_t stats_valid_records;
	uint64_t stats_truncated_bytes;
	uint32_t stats_pruned;
	uint32_t interrupted;		/* dangling ENTRY opens closed as interrupted */
	uint32_t tmp_removed;
};

/* One append-only log: a directory of segments, the newest open for append. */
struct seglog {
	const char *name;	/* "events" or "stats" */
	int dir_fd;
	int seg_fd;
	uint32_t seg_no;
	uint64_t seg_size;
	int64_t seg_started_ns;	/* CLOCK_MONOTONIC when this process opened or created it */
	uint8_t buf[PSS_REC_HDR + PSS_REC_MAX_PAYLOAD + PSS_REC_TRAILER];
};

struct pss_store {
	int root_fd;
	int clips_dir_fd;
	struct seglog events;
	struct seglog stats;
	int64_t stats_roll_ns;	/* PSS_STATS_ROLL_NS; tests may shorten it */
	uint64_t next_event_id;	/* above every id in the log and in clips/ */
	uint64_t boot_no;	/* highest boot_no in the log */
	struct store_recovery recovered;
};

/* Creates root/{events,stats,clips} if needed, runs recovery, opens both logs
 * for append. Returns 0 or -1 (logged; *s is closed). */
int store_open(struct pss_store *s, const char *root);
/* Appends one record (not yet durable) to the given log. STATS records go to
 * the stats log and every other type to the events log. 0 or -1. */
int store_append(struct pss_store *s, enum pss_log_which which, uint16_t type, const void *payload, uint32_t len);
/* fdatasync the given log's current segment. 0 or -1. */
int store_sync(struct pss_store *s, enum pss_log_which which);
/* ENTRY confirmed: append EVENT_OPEN and make it durable. 0 or -1. */
int store_event_open(struct pss_store *s, const struct pss_rec_event_open *o);
/* Event ended (or a one-shot event): append EVENT and make it durable. 0 or -1. */
int store_event_close(struct pss_store *s, const struct pss_rec_event *e);
void store_close(struct pss_store *s);

/* Visitor for store_scan; nonzero return stops the scan and is returned. */
typedef int (*store_visit_fn)(uint32_t seg_no, uint16_t type, uint16_t ver,
			      const void *payload, uint32_t len, void *arg);

/* Read-only walk over every valid record of one log, oldest first. */
int store_scan(const char *root, enum pss_log_which which, store_visit_fn fn, void *arg);

/* Length of the valid record prefix of buf; *n_records gets the count (may be NULL). */
uint64_t store_valid_prefix(const uint8_t *buf, uint64_t size, uint64_t *n_records);

#endif
