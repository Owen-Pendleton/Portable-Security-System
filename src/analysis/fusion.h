#ifndef PSS_ANALYSIS_FUSION_H
#define PSS_ANALYSIS_FUSION_H

/*
 * Motion + sound coincidence (DESIGN.md Section 4, fusion). An entry is
 * confirmed when a PIR rise and a sound onset fall within +-T_co of each
 * other, in either order. Only timestamps are compared (the sensors' own
 * CLOCK_MONOTONIC times), so the result doesn't depend on which message
 * reaches the hub first. Pure logic, no I/O, so it is testable off the Pi.
 */

#include <stdint.h>

struct fusion {
	int64_t t_co_ns;
	int64_t last_rise_ns, last_onset_ns;
	int have_rise, have_onset;
};

void fusion_init(struct fusion *f, int64_t t_co_ns);
/* Records a PIR rise at ts_ns. Returns 1 if a sound onset lies within
 * +-T_co of it (and stores onset time minus rise time in *dt_ns if not
 * NULL), else 0. */
int fusion_rise(struct fusion *f, int64_t ts_ns, int64_t *dt_ns);
/* Records a sound onset at ts_ns. Returns 1 if a PIR rise lies within
 * +-T_co of it (dt as above: onset time minus rise time), else 0. */
int fusion_onset(struct fusion *f, int64_t ts_ns, int64_t *dt_ns);
/* A sensor disconnected: forget its history so it can't confirm an entry. */
void fusion_forget(struct fusion *f, int pir, int mic);

#endif
