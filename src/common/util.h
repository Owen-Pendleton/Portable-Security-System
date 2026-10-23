#ifndef PSS_COMMON_UTIL_H
#define PSS_COMMON_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* $PSS_RUN_DIR (default /run/pss): sockets. */
const char *pss_run_dir(void);
/* $PSS_DATA_DIR (default /var/lib/pss): log/, clips/, state/. */
const char *pss_data_dir(void);

/* Strict decimal parse into [min, max]. Returns 0 or -1. */
int pss_parse_u32(const char *s, uint32_t min, uint32_t max, uint32_t *out);

/* Blocks the given signals and returns a nonblocking signalfd for them, or -1. */
int pss_signalfd(const int *sigs, size_t n);

/* Nonblocking CLOCK_MONOTONIC timerfd; first_ns must be > 0. Returns fd or -1. */
int pss_timerfd(int64_t first_ns, int64_t period_ns);
/* Consumes pending expirations. Returns 0 (including "none pending") or -1. */
int pss_timerfd_drain(int fd);

#endif
