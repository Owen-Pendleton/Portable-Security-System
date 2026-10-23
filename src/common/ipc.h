#ifndef PSS_COMMON_IPC_H
#define PSS_COMMON_IPC_H

/*
 * Daemon <-> hub messages over AF_UNIX SOCK_SEQPACKET (DESIGN.md Section 2): one
 * fixed-size struct per packet, so boundaries are preserved and a dead peer
 * shows up as EOF/EPIPE. All processes run on the same Pi, so the struct is
 * sent in native layout.
 */

#include <stddef.h>
#include <stdint.h>

#define PSS_IPC_VER 1
#define PSS_NBANDS 8

enum pss_role {
	PSS_ROLE_NONE = 0,
	PSS_ROLE_PIR = 1,
	PSS_ROLE_CAM = 2,
	PSS_ROLE_MIC = 3,
};

enum pss_msg_type {
	PSS_MSG_HELLO = 1,	/* daemon -> hub, first message */
	PSS_MSG_HEARTBEAT,	/* daemon -> hub, every 5 s */
	PSS_MSG_PIR_EDGE,	/* pird -> hub */
	PSS_MSG_CAM_TRIGGER,	/* hub -> camd */
	PSS_MSG_CAM_DONE,	/* camd -> hub */
	PSS_MSG_MIC_FRAME,	/* micd -> hub, 50 Hz */
};

struct pss_msg {
	uint16_t type;
	uint16_t ver;
	uint32_t seq;
	int64_t mono_ns;	/* CLOCK_MONOTONIC; for PIR edges, the kernel's edge timestamp */
	union {
		struct { uint32_t role; uint32_t pid; } hello;
		struct { uint64_t dropped; uint64_t count; } hb;
		struct { uint32_t rising; uint32_t pad; } pir;
		struct { uint64_t event_id; uint32_t frames; uint32_t interval_ms; } cam_trigger;
		/* status: 0 ok, >0 rpicam exit code (128+sig if killed), <0 -errno in camd */
		struct { uint64_t event_id; int32_t status; uint32_t frames; } cam_done;
		struct { float rms; float flux; float band[PSS_NBANDS]; } mic;
	} u;
};

const char *pss_role_name(uint32_t role);

/* Zeroes *m (no uninitialised padding on the wire), sets type and mono_ns = now. */
void pss_msg_init(struct pss_msg *m, uint16_t type);

/* "$PSS_RUN_DIR/hub.sock". Returns 0 or -1 (path too long). */
int ipc_sock_path(char *buf, size_t n);

/* Nonblocking listening socket at path (stale socket file removed). fd or -1, logged. */
int ipc_listen(const char *path);
/* Nonblocking connected socket. fd or -1 with errno set; not logged (callers retry). */
int ipc_connect(const char *path);

/* 0 or -1 with errno. */
int ipc_send(int fd, struct pss_msg *m);
/* 1 = message, 0 = peer closed, -1 = error with errno (EAGAIN, EBADMSG size, EPROTO version). */
int ipc_recv(int fd, struct pss_msg *m);

#endif
