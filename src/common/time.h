#ifndef PSS_COMMON_TIME_H
#define PSS_COMMON_TIME_H

#include <stdint.h>

#define PSS_NS_PER_MS INT64_C(1000000)
#define PSS_NS_PER_S INT64_C(1000000000)

/* CLOCK_MONOTONIC for every interval/ordering decision (DESIGN.md Section 5 "Time"). */
int64_t pss_mono_ns(void);
/* CLOCK_REALTIME, recorded alongside mono so a wrong wall clock can be fixed later. */
int64_t pss_wall_ns(void);

#endif
