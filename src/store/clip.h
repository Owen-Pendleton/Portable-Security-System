#ifndef PSS_STORE_CLIP_H
#define PSS_STORE_CLIP_H

/*
 * Mechanism D, clip store (DESIGN.md Section 5): a burst is written into
 * clips/<id>.dir.tmp, then committed by fsync(files) -> fsync(tmp dir) ->
 * rename to clips/<id>.dir -> fsync(clips/). Anything still named *.tmp at
 * startup is an interrupted write and is deleted.
 */

#include <stddef.h>
#include <stdint.h>

/* "<id>.dir.tmp" / "<id>.dir". Return 0 or -1 if buf is too small. */
int clip_tmp_name(char *buf, size_t n, uint64_t event_id);
int clip_final_name(char *buf, size_t n, uint64_t event_id);

/* Makes the burst durable under its final name; *n_images = number of *.jpg. 0 or -1. */
int clip_commit(int clips_dir_fd, uint64_t event_id, uint32_t *n_images);
/*
 * Burst number burst_no (1-based) of an event that may already have a
 * committed clips/<id>.dir. If it doesn't, this is clip_commit. Otherwise
 * each file in clips/<id>.dir.tmp is fsynced and renamed into clips/<id>.dir
 * as "b<burst_no>-<name>", then clips/<id>.dir is fsynced, the empty .tmp
 * removed and clips/ fsynced. A crash midway leaves a partial burst (the rest
 * stays in .tmp and is removed at startup); earlier bursts are untouched.
 * *n_images = number of *.jpg added. 0 or -1.
 */
int clip_append(int clips_dir_fd, uint64_t event_id, uint32_t burst_no, uint32_t *n_images);
/* Removes clips/<id>.dir.tmp if present. 0 or -1. */
int clip_discard(int clips_dir_fd, uint64_t event_id);
/* Removes every *.tmp entry (file or one-level dir), logging each. 0 or -1. */
int clip_cleanup_tmp(int clips_dir_fd, uint32_t *removed);
/* Number of *.jpg in committed clips/<id>.dir (0 if it doesn't exist). 0 or -1. */
int clip_count_images(int clips_dir_fd, uint64_t event_id, uint32_t *n_images);
/* Highest <id> among committed clips/<id>.dir (0 if none). 0 or -1. */
int clip_max_id(int clips_dir_fd, uint64_t *max_id);

/*
 * Evicts the oldest committed clips until used + need_bytes <= quota_bytes.
 * Returns 0 (room made), 1 (filesystem free space would drop below floor_pct:
 * clips disabled, event records still go through), or -1 on error.
 */
int clip_enforce_quota(int clips_dir_fd, uint64_t quota_bytes, uint64_t need_bytes,
		       uint32_t floor_pct, uint32_t *evicted);

#endif
