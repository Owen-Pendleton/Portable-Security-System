#ifndef PSS_CAPTURE_PIR_LOGIC_H
#define PSS_CAPTURE_PIR_LOGIC_H

/*
 * Hardware-independent part of pird, so it is testable off the Pi.
 * HC-SR501 false-triggers for ~60 s after power-up (DESIGN.md Section 6), so edges in
 * that window are not forwarded; repeated edges in the same direction (e.g.
 * a missed edge in poll mode) are dropped so the hub sees a clean rise/fall
 * sequence.
 */

#include <stdint.h>

enum pir_verdict {
	PIR_FORWARD = 0,
	PIR_WARMUP = 1,
	PIR_DUPLICATE = 2,
};

struct pir_filter {
	int64_t warmup_until_ns;
	int level;		/* 1 high, 0 low */
	uint64_t forwarded;
	uint64_t suppressed;
};

void pir_filter_init(struct pir_filter *f, int64_t start_ns, int64_t warmup_ns, int initial_level);
enum pir_verdict pir_filter_edge(struct pir_filter *f, int rising, int64_t ts_ns);

#endif
