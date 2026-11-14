#ifndef PSS_SUPERVISOR_POLICY_H
#define PSS_SUPERVISOR_POLICY_H

/*
 * Restart rules (DESIGN.md Section 3, mechanism E). Pure logic, no I/O, so
 * it is testable off the Pi; supervisor.c feeds it child starts, heartbeats
 * and exits.
 *   - A child that exits or is killed is restarted after a backoff of
 *     1 s, 2 s, 4 s ... capped at 60 s. A child that ran 10 min before it
 *     died starts again from 1 s.
 *   - Exit code 3 means "device gone" (e.g. mic unplugged): retry every
 *     30 s, without growing the backoff.
 *   - Hung: no heartbeat for 15 s, or none at all within 60 s of start.
 */

#include <stdint.h>

#define SUP_EXIT_DEVICE_GONE 3

struct sup_policy {
	int64_t backoff_min_ns, backoff_max_ns;
	int64_t healthy_ns;	/* a run this long resets the backoff */
	int64_t gone_retry_ns;
	int64_t hb_timeout_ns;
	int64_t first_hb_ns;
};

struct sup_track {
	int64_t backoff_ns;	/* delay for the next ordinary restart */
	int64_t started_ns;
	int64_t last_hb_ns;
	int have_hb;
};

void sup_policy_default(struct sup_policy *p);
void sup_track_init(const struct sup_policy *p, struct sup_track *t);
void sup_track_started(struct sup_track *t, int64_t now_ns);
void sup_track_beat(struct sup_track *t, int64_t now_ns);
/* 1 if the running child should be SIGKILLed as hung. */
int sup_hung(const struct sup_policy *p, const struct sup_track *t, int64_t now_ns);
/* The child just ended. exit_code is its exit status, or -1 if a signal
 * killed it. Returns how long to wait before starting it again, and
 * advances the backoff. */
int64_t sup_restart_delay(const struct sup_policy *p, struct sup_track *t, int64_t now_ns, int exit_code);

#endif
