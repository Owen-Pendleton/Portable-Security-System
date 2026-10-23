#ifndef PSS_STORE_EVLOG_H
#define PSS_STORE_EVLOG_H

/*
 * Mechanism D, event log (Design.md §5): append-only segment files
 * log/seg-NNNNNN.pel, each record framed as
 *     [len u32][type u16][ver u16][payload: len bytes][crc32 u32]
 * with the CRC over header + payload. Native little-endian (Pi 5 and the lab
 * machines are both LE). Recovery at open truncates the newest segment at the
 * first record with a bad length or CRC.
 */

#include <stdint.h>

#define PSS_SEG_MAX (4u * 1024 * 1024)
#define PSS_REC_MAX_PAYLOAD 4096u
#define PSS_REC_HDR 8u
#define PSS_REC_TRAILER 4u
#define PSS_REC_VER 1

enum pss_rec_type {
	PSS_REC_BOOT = 1,
	PSS_REC_EVENT = 2,
	PSS_REC_STATS = 3,
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
	uint32_t seg_no;
	uint64_t valid_records;
	uint64_t truncated_bytes;
	uint32_t tmp_removed;
};

struct pss_store {
	int root_fd;
	int log_dir_fd;
	int clips_dir_fd;
	int seg_fd;
	uint32_t seg_no;
	uint64_t seg_size;
	uint64_t next_event_id;	/* above every id in the log and in clips/ */
	uint64_t boot_no;	/* highest boot_no in the log */
	struct store_recovery recovered;
	uint8_t buf[PSS_REC_HDR + PSS_REC_MAX_PAYLOAD + PSS_REC_TRAILER];
};

/* Creates root/{log,clips,state} if needed, runs recovery, opens the newest
 * segment for append. Returns 0 or -1 (logged; *s is closed). */
int store_open(struct pss_store *s, const char *root);
/* Appends one record (not yet durable). Rolls to a new segment past PSS_SEG_MAX. 0 or -1. */
int store_append(struct pss_store *s, uint16_t type, const void *payload, uint32_t len);
/* fdatasync the current segment. 0 or -1. */
int store_sync(struct pss_store *s);
void store_close(struct pss_store *s);

/* Visitor for store_scan; nonzero return stops the scan and is returned. */
typedef int (*store_visit_fn)(uint32_t seg_no, uint16_t type, uint16_t ver,
			      const void *payload, uint32_t len, void *arg);

/* Read-only walk over every valid record of every segment, oldest first. */
int store_scan(const char *root, store_visit_fn fn, void *arg);

/* Length of the valid record prefix of buf; *n_records gets the count (may be NULL). */
uint64_t store_valid_prefix(const uint8_t *buf, uint64_t size, uint64_t *n_records);

#endif
