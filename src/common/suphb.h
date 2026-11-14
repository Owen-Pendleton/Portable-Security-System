#ifndef PSS_COMMON_SUPHB_H
#define PSS_COMMON_SUPHB_H

/*
 * Heartbeat to the supervisor (DESIGN.md Section 3, mechanism E). The
 * supervisor hands each child one end of a socketpair and names it in
 * $PSS_SUP_FD. Run any other way (by hand, deploy/run-m3.sh, tests) there
 * is no supervisor and both calls do nothing.
 *
 * Sensor daemons get this for free: client_init() calls suphb_init(), and
 * every PSS_MSG_HEARTBEAT sent through client_send() is copied here, even
 * while the hub is down. The hub calls both itself.
 */

#include <stdint.h>

/* Reads $PSS_SUP_FD once, marks the fd close-on-exec (so camd's
 * rpicam-still doesn't inherit it) and unsets the variable. */
void suphb_init(void);
/* Nonblocking; a full socket drops the beat (the next one comes in 5 s). */
void suphb_beat(uint64_t count, uint64_t dropped);

#endif
