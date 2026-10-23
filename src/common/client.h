#ifndef PSS_COMMON_CLIENT_H
#define PSS_COMMON_CLIENT_H

/*
 * Sensor-daemon side of the hub connection: connect with backoff, say HELLO,
 * and drop (and count) messages while the hub is away (DESIGN.md Section 6, hub row).
 */

#include "common/ipc.h"

#include <stdint.h>

struct pss_client {
	int fd;			/* -1 while disconnected */
	uint32_t role;
	uint32_t seq;
	uint64_t dropped;
	int64_t next_try_ns;
	int64_t backoff_ns;
	int warned;
	char path[108];
};

/* Returns 0 or -1 (socket path too long). */
int client_init(struct pss_client *c, uint32_t role);
/* If disconnected and the backoff has elapsed, try to connect. Returns 1 if it just connected. */
int client_poll_connect(struct pss_client *c, int64_t now_ns);
/* Sends with seq filled in. 0, or -1 if dropped (counted; connection closed on hard errors). */
int client_send(struct pss_client *c, struct pss_msg *m);
void client_disconnect(struct pss_client *c, int64_t now_ns, const char *why);
void client_close(struct pss_client *c);

#endif
