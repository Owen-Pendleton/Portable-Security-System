/*
 * hub: the single writer (DESIGN.md Section 2). Accepts sensor daemons on
 * $PSS_RUN_DIR/hub.sock, turns PIR edges into events, asks camd for an image
 * burst per event, and commits everything through the store (mechanism D).
 *
 * Fusion (DESIGN.md Section 4, src/analysis/fusion.c): a PIR rise opens a
 * MOTION_ONLY event and a sound (an impulse or voice, below) opens a
 * SOUND_ONLY event, but neither takes images. Only when a rise and a sound
 * fall within +-T_co of
 * each other (either order) does the event become an ENTRY: it is made
 * durable at once and camd is asked for a burst. While the ENTRY stays open
 * and the device is watching, a burst repeats every --reburst-ms after the
 * first, until --reburst-cap-ms after it (default: at 0-4 min, 5 bursts).
 * Repeat bursts are appended to the same clips/<id>.dir (clip_append).
 *
 * Sound (DESIGN.md Section 4, src/analysis/audio.c): a per-band background
 * model, learned only while no event is open, and two detectors on it: an
 * impulse (latch, door: sudden, broadband, gone within 140 ms) and voice
 * (speech bands up for 400 ms within 2 s, not led by low rumble). An impulse
 * counts once; voice counts on every frame while it lasts, so talking keeps
 * an event open. --k-imp and --k-voice are placeholders until tuned on Pi
 * recordings (the heartbeat logs the largest scores heard).
 *
 * Arm state (DESIGN.md Section 4, src/analysis/arm.c): pird's button presses
 * and confirmed entries drive it, and every change is made durable as a
 * PSS_REC_ARM record, the last of which is restored at start. New events
 * open, and the camera fires, only while ARMED or in GRACE; an event already
 * open when the device is disarmed finishes normally. The LED (not wired
 * yet, Owen's side) is only logged.
 *
 * Exit codes: 0 on SIGTERM/SIGINT, 1 on error (including any store write
 * failure, so the supervisor restarts us and recovery runs), 2 on usage.
 */
#include "analysis/arm.h"
#include "analysis/audio.h"
#include "analysis/fusion.h"
#include "common/ipc.h"
#include "common/log.h"
#include "common/suphb.h"
#include "common/time.h"
#include "common/util.h"
#include "store/clip.h"
#include "store/evlog.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXIT_USAGE 2
#define MAX_CONNS 8
#define HB_FIRST_NS (5 * PSS_NS_PER_S)
#define HB_EVERY_NS (60 * PSS_NS_PER_S)
#define HB_STALE_NS (15 * PSS_NS_PER_S)
#define SUPHB_EVERY_NS (5 * PSS_NS_PER_S)	/* to the supervisor, like every daemon */
#define BURST_SLACK_NS (20 * PSS_NS_PER_S)
#define BURST_EST_BYTES (9ull * 1024 * 1024)	/* upper end of DESIGN.md Section 5's 6-9 MB */

enum { TAG_LISTEN = MAX_CONNS, TAG_SIG, TAG_TICK };

struct conn {
	int fd;
	uint32_t role;		/* PSS_ROLE_NONE until HELLO */
	int64_t last_hb_ns;
};

enum burst_state { BURST_NONE, BURST_PENDING, BURST_DONE };

struct event {
	int open;
	uint64_t id;
	uint32_t kind;		/* PSS_EV_ENTRY once confirmed; else set at close */
	int64_t start_mono_ns;
	int64_t start_wall_ns;
	int64_t last_activity_ns;
	int pir_high;
	uint32_t pir_edges;
	uint32_t flags;
	uint32_t n_images;
	enum burst_state burst;
	int64_t burst_deadline_ns;
	uint32_t bursts;	/* asked for, including failed ones */
	int64_t first_burst_ns;
};

struct hub {
	struct pss_store store;
	int store_open;
	int listen_fd, epoll_fd, sig_fd, tick_fd;
	struct conn conns[MAX_CONNS];
	struct event ev;
	struct audio mic;	/* background model + impulse/voice detectors (and their config) */
	struct fusion fu;
	struct arm arm;
	int fatal;		/* a store write failed inside a handler: exit so recovery runs */
	int64_t start_ns, next_hb_ns, next_suphb_ns;
	uint64_t pir_edges, mic_frames, cam_bursts, cam_failures, events_closed, mic_impulses, mic_voices;
	/* config */
	int64_t quiet_ns, t_co_ns, reburst_ns, reburst_cap_ns;
	uint32_t burst_frames, burst_interval_ms, floor_pct;
	uint64_t quota_bytes;
	char sock_path[108];
};

static struct hub hub;

static void usage(void)
{
	fprintf(stderr, "usage: hub [--quiet-ms=30000] [--burst-frames=20] [--burst-interval-ms=500]\n"
			"           [--quota-mb=2048] [--floor-pct=5]\n"
			"           [--k-imp=6] [--k-voice=3] [--warmup-frames=250] [--t-co-ms=5000]\n"
			"           [--exit-delay-ms=60000] [--grace-ms=30000]\n"
			"           [--reburst-ms=60000] [--reburst-cap-ms=300000]\n");
}

static struct conn *conn_for_role(struct hub *h, uint32_t role)
{
	for (int i = 0; i < MAX_CONNS; i++)
		if (h->conns[i].fd >= 0 && h->conns[i].role == role)
			return &h->conns[i];
	return NULL;
}

static int role_alive(struct hub *h, uint32_t role, int64_t now)
{
	struct conn *c = conn_for_role(h, role);

	return c != NULL && now - c->last_hb_ns < HB_STALE_NS;
}

static void trigger_burst(struct hub *h, int64_t now)
{
	struct event *ev = &h->ev;
	struct conn *cam = conn_for_role(h, PSS_ROLE_CAM);
	struct pss_msg m;
	uint32_t evicted = 0;
	int q;

	if (ev->bursts++ == 0)
		ev->first_burst_ns = now;
	if (cam == NULL) {
		pss_log("event %" PRIu64 ": camd not connected, no images (burst %" PRIu32 ")", ev->id, ev->bursts);
		return;
	}
	q = clip_enforce_quota(h->store.clips_dir_fd, h->quota_bytes, BURST_EST_BYTES, h->floor_pct, &evicted);
	if (q != 0) {
		pss_log("event %" PRIu64 ": %s, no images (burst %" PRIu32 ")", ev->id,
			q > 0 ? "clips disabled" : "quota check failed", ev->bursts);
		return;
	}
	pss_msg_init(&m, PSS_MSG_CAM_TRIGGER);
	m.u.cam_trigger.event_id = ev->id;
	m.u.cam_trigger.frames = h->burst_frames;
	m.u.cam_trigger.interval_ms = h->burst_interval_ms;
	if (ipc_send(cam->fd, &m) != 0) {
		pss_log("event %" PRIu64 ": trigger to camd failed: %s (burst %" PRIu32 ")", ev->id, strerror(errno), ev->bursts);
		return;
	}
	ev->burst = BURST_PENDING;
	ev->burst_deadline_ns = now + (int64_t)h->burst_frames * h->burst_interval_ms * PSS_NS_PER_MS + BURST_SLACK_NS;
	h->cam_bursts++;
}

/* Makes an arm state change durable and logs it (and the LED, until it is
 * wired). 0, or -1 on a store failure. */
static int write_arm(struct hub *h, const struct arm_change *c, int64_t now)
{
	struct pss_rec_arm r;
	static const char *const cause[] = { "?", "button", "exit delay over", "entry", "grace over", "restart" };
	struct arm prev = { .state = c->from };

	memset(&r, 0, sizeof r);
	r.mono_ns = now;
	r.wall_ns = pss_wall_ns();
	r.event_id = c->event_id;
	r.state = c->to;
	r.cause = c->cause;
	r.label = c->label;
	if (store_append(&h->store, PSS_LOG_EVENTS, PSS_REC_ARM, &r, sizeof r) != 0 ||
	    store_sync(&h->store, PSS_LOG_EVENTS) != 0) {
		pss_log("arm: store write failed; exiting so recovery runs");
		return -1;
	}
	if (c->event_id != 0)
		pss_log("arm: %s -> %s (%s), event %" PRIu64 "%s%s", arm_state_name(c->from), arm_state_name(c->to),
			cause[c->cause <= ARM_CAUSE_RESTORE ? c->cause : 0], c->event_id,
			c->label != ARM_LABEL_NONE ? " labelled " : "", arm_label_name(c->label));
	else
		pss_log("arm: %s -> %s (%s)", arm_state_name(c->from), arm_state_name(c->to),
			cause[c->cause <= ARM_CAUSE_RESTORE ? c->cause : 0]);
	if (arm_led(&prev) != arm_led(&h->arm) || c->cause == ARM_CAUSE_RESTORE)
		pss_log("arm: LED %s (not wired yet)", arm_led(&h->arm) == ARM_LED_BLINK ? "blink" : "off");
	return 0;
}

/* pir: 1 if a PIR rise opened the event, 0 for a sound onset. */
static void open_event(struct hub *h, int64_t ts, int64_t now, int pir)
{
	struct event *ev = &h->ev;

	memset(ev, 0, sizeof *ev);
	ev->open = 1;
	ev->id = h->store.next_event_id++;
	ev->start_mono_ns = ts;
	ev->start_wall_ns = pss_wall_ns() - (now - ts);
	ev->last_activity_ns = now;
	ev->pir_high = pir;
	ev->pir_edges = pir ? 1 : 0;
	if (!role_alive(h, PSS_ROLE_MIC, now))
		ev->flags |= PSS_EVF_AUDIO_OFFLINE;
	pss_log("event %" PRIu64 " opened (%s)", ev->id, pir ? "PIR rise" : "sound onset");
}

/* Motion and sound coincided (dt_ns = onset time - rise time): the open
 * event becomes an ENTRY, durable at once, and only now is the camera
 * fired. Once per event. On a store failure sets h->fatal. */
static void confirm_entry(struct hub *h, int64_t dt_ns, int64_t now)
{
	struct event *ev = &h->ev;
	struct pss_rec_event_open o;
	struct arm_change c;

	if (!ev->open || ev->kind == PSS_EV_ENTRY)
		return;
	if (!arm_watching(&h->arm)) {	/* disarmed mid-event: no entry, no camera */
		pss_log("event %" PRIu64 ": motion+sound, but %s: not an entry", ev->id, arm_state_name(h->arm.state));
		return;
	}
	ev->kind = PSS_EV_ENTRY;
	memset(&o, 0, sizeof o);
	o.event_id = ev->id;
	o.start_mono_ns = ev->start_mono_ns;
	o.start_wall_ns = ev->start_wall_ns;
	o.kind = PSS_EV_ENTRY;
	o.flags = ev->flags;
	if (store_event_open(&h->store, &o) != 0) {
		pss_log("event %" PRIu64 ": store write failed; exiting so recovery runs", ev->id);
		h->fatal = 1;
		return;
	}
	pss_log("event %" PRIu64 ": ENTRY confirmed (motion+sound, sound %+.1fs from motion)",
		ev->id, (double)dt_ns / PSS_NS_PER_S);
	trigger_burst(h, now);
	if (arm_entry(&h->arm, ev->id, now, &c) && write_arm(h, &c, now) != 0)
		h->fatal = 1;
}

/* Writes the event record and makes it durable. 0, or -1 on a store failure. */
static int close_event(struct hub *h, int64_t end_ns, uint32_t extra_flags)
{
	struct event *ev = &h->ev;
	struct pss_rec_event r;

	memset(&r, 0, sizeof r);
	r.event_id = ev->id;
	r.start_mono_ns = ev->start_mono_ns;
	r.end_mono_ns = end_ns;
	r.start_wall_ns = ev->start_wall_ns;
	if (ev->kind == PSS_EV_ENTRY)
		r.kind = PSS_EV_ENTRY;
	else
		r.kind = ev->pir_edges > 0 ? PSS_EV_MOTION_ONLY : PSS_EV_SOUND_ONLY;
	r.flags = ev->flags | extra_flags;
	if (ev->bursts > 0 && ev->n_images == 0)	/* a later burst failing doesn't count */
		r.flags |= PSS_EVF_NO_IMAGES;
	r.pir_edges = ev->pir_edges;
	r.n_images = ev->n_images;
	if (store_event_close(&h->store, &r) != 0) {
		pss_log("event %" PRIu64 ": store write failed; exiting so recovery runs", ev->id);
		return -1;
	}
	pss_log("event %" PRIu64 " closed: %s dur=%.1fs edges=%" PRIu32 " images=%" PRIu32 "%s%s",
		ev->id, r.kind == PSS_EV_ENTRY ? "ENTRY" : r.kind == PSS_EV_MOTION_ONLY ? "MOTION_ONLY" : "SOUND_ONLY", (double)(end_ns - ev->start_mono_ns) / PSS_NS_PER_S, r.pir_edges, r.n_images,
		(r.flags & PSS_EVF_NO_IMAGES) ? " no-images" : "",
		(r.flags & PSS_EVF_AUDIO_OFFLINE) ? " audio-offline" : "");
	h->events_closed++;
	ev->open = 0;
	return 0;
}

static void on_cam_done(struct hub *h, const struct pss_msg *m)
{
	struct event *ev = &h->ev;
	uint64_t id = m->u.cam_done.event_id;
	uint32_t n = 0;

	if (!ev->open || id != ev->id || ev->burst != BURST_PENDING) {
		pss_log("camd: late result for event %" PRIu64 ", discarding", id);
		clip_discard(h->store.clips_dir_fd, id);
		return;
	}
	ev->burst = BURST_DONE;
	if (m->u.cam_done.status != 0 || m->u.cam_done.frames == 0) {
		h->cam_failures++;
		pss_log("event %" PRIu64 ": burst %" PRIu32 " failed (status %" PRId32 ", %" PRIu32 " frames)",
			id, ev->bursts, m->u.cam_done.status, m->u.cam_done.frames);
		clip_discard(h->store.clips_dir_fd, id);
		return;
	}
	if (clip_append(h->store.clips_dir_fd, id, ev->bursts, &n) != 0) {
		h->cam_failures++;
		clip_discard(h->store.clips_dir_fd, id);
		return;
	}
	ev->n_images += n;
	pss_log("event %" PRIu64 ": burst %" PRIu32 " committed %" PRIu32 " images to clips/%" PRIu64 ".dir (%" PRIu32 " total)",
		id, ev->bursts, n, id, ev->n_images);
}

static void on_pir_edge(struct hub *h, const struct pss_msg *m, int64_t now)
{
	struct event *ev = &h->ev;
	int64_t dt = 0;

	h->pir_edges++;
	if (m->u.pir.rising) {
		int hit = fusion_rise(&h->fu, m->mono_ns, &dt);

		if (!ev->open) {
			if (arm_watching(&h->arm))	/* nothing is recorded while not armed */
				open_event(h, m->mono_ns, now, 1);
		} else {
			ev->pir_edges++;
			ev->pir_high = 1;
			ev->last_activity_ns = now;
		}
		if (hit)
			confirm_entry(h, dt, now);
	} else if (ev->open) {
		ev->pir_high = 0;
		ev->last_activity_ns = now;
	}
}

/* An impulse or voice at ts (sensor time): fusion's audio input. Opens a
 * SOUND_ONLY event if none is open, else keeps the open one alive. */
static void on_sound(struct hub *h, int64_t ts, int64_t now)
{
	struct event *ev = &h->ev;
	int64_t dt = 0;
	int hit = fusion_onset(&h->fu, ts, &dt);

	if (ev->open)
		ev->last_activity_ns = now;
	else if (arm_watching(&h->arm))	/* nothing is recorded while not armed */
		open_event(h, ts, now, 0);
	if (hit)
		confirm_entry(h, dt, now);
}

static void on_mic_frame(struct hub *h, const struct pss_msg *m, int64_t now)
{
	struct audio_result r;
	/* The background stops learning only once there is motion, so a person
	 * in view stays unusual. During a sound-only event it keeps learning, so
	 * a TV or radio left on becomes the room's normal sound instead of
	 * holding the event open (Bryan, 2026-11-18). */
	int learn = !h->ev.open || h->ev.pir_edges == 0;

	h->mic_frames++;
	if (audio_frame(&h->mic, m->mono_ns, m->u.mic.flux, m->u.mic.band, learn, &r) != 0) {
		pss_log("micd: non-finite frame ignored");
		return;
	}
	if (r.flags & AUDIO_IMPULSE) {
		h->mic_impulses++;
		pss_log("impulse: flux z=%.1f, decayed within %d ms", r.impulse_z, AUDIO_DECAY_FRAMES * 20);
		on_sound(h, r.impulse_ns, now);
	}
	if (r.flags & AUDIO_VOICE_START) {
		h->mic_voices++;
		pss_log("voice: speech bands z=%.1f, %d of the last %d frames", r.voice_z, AUDIO_VOICE_MIN,
			AUDIO_VOICE_WIN);
	}
	if (r.flags & AUDIO_VOICE)	/* every frame while voice lasts */
		on_sound(h, m->mono_ns, now);
}

static void conn_drop(struct hub *h, int idx, const char *why)
{
	struct conn *c = &h->conns[idx];

	if (epoll_ctl(h->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL) != 0)
		pss_log("epoll_ctl DEL: %s", strerror(errno));
	if (close(c->fd) != 0)
		pss_log("close conn: %s", strerror(errno));
	pss_log("%s disconnected (%s)", pss_role_name(c->role), why);
	if (c->role == PSS_ROLE_CAM && h->ev.open && h->ev.burst == BURST_PENDING) {
		h->ev.burst = BURST_DONE;
		h->cam_failures++;
	}
	if (c->role == PSS_ROLE_PIR) {
		h->ev.pir_high = 0; /* don't hold an event open on a dead sensor */
		fusion_forget(&h->fu, 1, 0);
	}
	if (c->role == PSS_ROLE_MIC) {
		/* relearn: the device or room may differ */
		audio_init(&h->mic, h->mic.k_imp, h->mic.k_voice, h->mic.warmup);
		fusion_forget(&h->fu, 0, 1);
	}
	c->fd = -1;
	c->role = PSS_ROLE_NONE;
}

static void handle_msg(struct hub *h, int idx, const struct pss_msg *m, int64_t now)
{
	struct conn *c = &h->conns[idx];

	if (c->role == PSS_ROLE_NONE && m->type != PSS_MSG_HELLO) {
		conn_drop(h, idx, "message before HELLO");
		return;
	}
	switch (m->type) {
	case PSS_MSG_HELLO:
		if (m->u.hello.role < PSS_ROLE_PIR || m->u.hello.role > PSS_ROLE_MIC) {
			conn_drop(h, idx, "bad role");
			return;
		}
		c->role = m->u.hello.role;
		c->last_hb_ns = now;
		pss_log("%s connected (pid %" PRIu32 ")", pss_role_name(c->role), m->u.hello.pid);
		break;
	case PSS_MSG_HEARTBEAT:
		c->last_hb_ns = now;
		break;
	case PSS_MSG_PIR_EDGE:
		on_pir_edge(h, m, now);
		break;
	case PSS_MSG_CAM_DONE:
		on_cam_done(h, m);
		break;
	case PSS_MSG_MIC_FRAME:
		on_mic_frame(h, m, now);
		break;
	case PSS_MSG_BUTTON: {
		struct arm_change ch;

		if (c->role != PSS_ROLE_PIR) {	/* the button is wired to pird only */
			pss_log("%s: button message ignored (not pird)", pss_role_name(c->role));
			break;
		}
		pss_log("arm button press #%" PRIu32, m->u.button.presses);
		if (arm_button(&h->arm, now, &ch) && write_arm(h, &ch, now) != 0)
			h->fatal = 1;
		break;
	}
	default:
		pss_log("%s: unknown message type %u", pss_role_name(c->role), m->type);
		break;
	}
}

static void conn_readable(struct hub *h, int idx, int64_t now)
{
	struct pss_msg m;
	int r;

	while (h->conns[idx].fd >= 0) {
		r = ipc_recv(h->conns[idx].fd, &m);
		if (r == 1) {
			handle_msg(h, idx, &m, now);
			continue;
		}
		if (r == 0)
			conn_drop(h, idx, "EOF");
		else if (errno != EAGAIN)
			conn_drop(h, idx, strerror(errno));
		return;
	}
}

static void accept_all(struct hub *h, int64_t now)
{
	for (;;) {
		struct epoll_event ee = { .events = EPOLLIN };
		int fd = accept4(h->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
		int idx = -1;

		if (fd < 0) {
			if (errno != EAGAIN && errno != EINTR)
				pss_log("accept4: %s", strerror(errno));
			return;
		}
		for (int i = 0; i < MAX_CONNS; i++) {
			if (h->conns[i].fd < 0) {
				idx = i;
				break;
			}
		}
		if (idx < 0) {
			pss_log("too many connections, refusing one");
			close(fd);
			continue;
		}
		ee.data.u32 = (uint32_t)idx;
		if (epoll_ctl(h->epoll_fd, EPOLL_CTL_ADD, fd, &ee) != 0) {
			pss_log("epoll_ctl ADD: %s", strerror(errno));
			close(fd);
			continue;
		}
		h->conns[idx] = (struct conn){ .fd = fd, .role = PSS_ROLE_NONE, .last_hb_ns = now };
	}
}

static const char *alive_str(struct hub *h, uint32_t role, int64_t now)
{
	if (role_alive(h, role, now))
		return "ok";
	return conn_for_role(h, role) ? "STALE" : "absent";
}

/* Stats record (1/min) doubles as the soak heartbeat line. 0 or -1. */
static int write_stats(struct hub *h, int64_t now)
{
	struct pss_rec_stats r;

	memset(&r, 0, sizeof r);
	r.mono_ns = now;
	r.wall_ns = pss_wall_ns();
	r.pir_edges = h->pir_edges;
	r.mic_frames = h->mic_frames;
	r.cam_bursts = h->cam_bursts;
	r.cam_failures = h->cam_failures;
	for (uint32_t role = PSS_ROLE_PIR; role <= PSS_ROLE_MIC; role++)
		if (role_alive(h, role, now))
			r.sensors_ok |= 1u << role;
	if (store_append(&h->store, PSS_LOG_STATS, PSS_REC_STATS, &r, sizeof r) != 0 ||
	    store_sync(&h->store, PSS_LOG_STATS) != 0)
		return -1;
	pss_log("hb: up=%" PRId64 "s pird=%s camd=%s micd=%s edges=%" PRIu64 " events=%" PRIu64
		" bursts=%" PRIu64 " cam_fail=%" PRIu64 " mic_frames=%" PRIu64 " impulses=%" PRIu64
		" voices=%" PRIu64 " imp_zmax=%.1f voice_zmax=%.1f",
		(now - h->start_ns) / PSS_NS_PER_S, alive_str(h, PSS_ROLE_PIR, now),
		alive_str(h, PSS_ROLE_CAM, now), alive_str(h, PSS_ROLE_MIC, now),
		h->pir_edges, h->events_closed, h->cam_bursts, h->cam_failures, h->mic_frames, h->mic_impulses,
		h->mic_voices, h->mic.imp_zmax, h->mic.voice_zmax);
	h->mic.imp_zmax = h->mic.voice_zmax = 0;
	return 0;
}

static int tick(struct hub *h, int64_t now)
{
	struct event *ev = &h->ev;
	struct arm_change c;

	if (now >= h->next_suphb_ns) {
		suphb_beat(h->events_closed, 0);
		h->next_suphb_ns = now + SUPHB_EVERY_NS;
	}
	if (arm_tick(&h->arm, now, &c) && write_arm(h, &c, now) != 0)
		return -1;
	if (ev->open && ev->burst == BURST_PENDING && now > ev->burst_deadline_ns) {
		pss_log("event %" PRIu64 ": camd result overdue, no images", ev->id);
		ev->burst = BURST_DONE;
		h->cam_failures++;
	}
	if (ev->open && !ev->pir_high && ev->burst != BURST_PENDING &&
	    now - ev->last_activity_ns >= h->quiet_ns && close_event(h, ev->last_activity_ns, 0) != 0)
		return -1;
	/* Repeat bursts, on a fixed schedule from the first so the count is
	 * cap/interval (rounded up) whatever the tick phase. Checked after the
	 * close above, so an event ready to close gets no extra burst. */
	if (ev->open && ev->kind == PSS_EV_ENTRY && ev->bursts > 0 && ev->burst != BURST_PENDING &&
	    arm_watching(&h->arm) && (int64_t)ev->bursts * h->reburst_ns < h->reburst_cap_ns &&
	    now >= ev->first_burst_ns + (int64_t)ev->bursts * h->reburst_ns) {
		pss_log("event %" PRIu64 ": still open, repeat burst %" PRIu32, ev->id, ev->bursts + 1);
		trigger_burst(h, now);
	}
	if (now >= h->next_hb_ns) {
		if (write_stats(h, now) != 0)
			return -1;
		h->next_hb_ns = now + HB_EVERY_NS;
	}
	return 0;
}

static int write_boot(struct hub *h)
{
	struct pss_rec_boot b;

	memset(&b, 0, sizeof b);
	b.boot_no = h->store.boot_no + 1;
	b.mono_ns = pss_mono_ns();
	b.wall_ns = pss_wall_ns();
	if (store_append(&h->store, PSS_LOG_EVENTS, PSS_REC_BOOT, &b, sizeof b) != 0 ||
	    store_sync(&h->store, PSS_LOG_EVENTS) != 0)
		return -1;
	h->store.boot_no = b.boot_no;
	pss_log("boot #%" PRIu64 " recorded; next event id %" PRIu64, b.boot_no, h->store.next_event_id);
	return 0;
}

static int epoll_add(struct hub *h, int fd, uint32_t tag)
{
	struct epoll_event ee = { .events = EPOLLIN, .data.u32 = tag };

	if (epoll_ctl(h->epoll_fd, EPOLL_CTL_ADD, fd, &ee) != 0) {
		pss_log("epoll_ctl ADD: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static int parse_args(struct hub *h, int argc, char **argv)
{
	static const struct option opts[] = {
		{ "quiet-ms", required_argument, NULL, 'q' },
		{ "burst-frames", required_argument, NULL, 'f' },
		{ "burst-interval-ms", required_argument, NULL, 'i' },
		{ "quota-mb", required_argument, NULL, 'Q' },
		{ "floor-pct", required_argument, NULL, 'F' },
		{ "k-imp", required_argument, NULL, 'z' },
		{ "k-voice", required_argument, NULL, 'd' },
		{ "warmup-frames", required_argument, NULL, 'w' },
		{ "t-co-ms", required_argument, NULL, 'T' },
		{ "exit-delay-ms", required_argument, NULL, 'x' },
		{ "grace-ms", required_argument, NULL, 'g' },
		{ "reburst-ms", required_argument, NULL, 'r' },
		{ "reburst-cap-ms", required_argument, NULL, 'R' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	uint32_t quiet_ms = 30000, quota_mb = 2048, t_co_ms = 5000, v;
	uint32_t exit_delay_ms = 60000, grace_ms = 30000;
	uint32_t reburst_ms = 60000, reburst_cap_ms = 300000;	/* 5 bursts, at 0-4 min */
	uint32_t k_imp, k_voice, warmup;
	int opt;

	h->burst_frames = 20;
	h->burst_interval_ms = 500;
	h->floor_pct = 5;
	k_imp = 6;		/* placeholder: tune from Pi recordings */
	k_voice = 3;		/* placeholder: tune from Pi recordings */
	warmup = 250;		/* 5 s of background before any detection */
	while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		if (opt != 'h' && opt != '?' && pss_parse_u32(optarg, 0, UINT32_MAX, &v) != 0)
			opt = '?';
		switch (opt) {
		case 'q': quiet_ms = v; break;
		case 'f': if (v < 1 || v > 1000) return -1; h->burst_frames = v; break;
		case 'i': if (v < 1 || v > 10000) return -1; h->burst_interval_ms = v; break;
		case 'Q': quota_mb = v; break;
		case 'F': if (v > 100) return -1; h->floor_pct = v; break;
		case 'z': if (v < 1 || v > 1000) return -1; k_imp = v; break;
		case 'd': if (v < 1 || v > 1000) return -1; k_voice = v; break;
		case 'w': if (v < 1) return -1; warmup = v; break;
		case 'T': if (v < 1 || v > 60000) return -1; t_co_ms = v; break;
		case 'x': if (v < 1 || v > 3600000) return -1; exit_delay_ms = v; break;
		case 'g': if (v < 1 || v > 3600000) return -1; grace_ms = v; break;
		case 'r': if (v < 1 || v > 3600000) return -1; reburst_ms = v; break;
		case 'R': if (v > 3600000) return -1; reburst_cap_ms = v; break;	/* 0: one burst */
		default: return -1;
		}
	}
	arm_init(&h->arm, (int64_t)exit_delay_ms * PSS_NS_PER_MS, (int64_t)grace_ms * PSS_NS_PER_MS);
	h->quiet_ns = (int64_t)quiet_ms * PSS_NS_PER_MS;
	h->t_co_ns = (int64_t)t_co_ms * PSS_NS_PER_MS;
	h->reburst_ns = (int64_t)reburst_ms * PSS_NS_PER_MS;
	h->reburst_cap_ns = (int64_t)reburst_cap_ms * PSS_NS_PER_MS;
	fusion_init(&h->fu, h->t_co_ns);
	audio_init(&h->mic, k_imp, k_voice, warmup);
	h->quota_bytes = (uint64_t)quota_mb * 1024 * 1024;
	return 0;
}

int main(int argc, char **argv)
{
	const int sigs[] = { SIGINT, SIGTERM };
	struct hub *h = &hub;
	int code = 1;

	pss_log_init("hub");
	suphb_init();
	h->listen_fd = h->epoll_fd = h->sig_fd = h->tick_fd = -1;
	for (int i = 0; i < MAX_CONNS; i++)
		h->conns[i].fd = -1;
	if (parse_args(h, argc, argv) != 0) {
		usage();
		return EXIT_USAGE;
	}

	h->sig_fd = pss_signalfd(sigs, 2);
	if (h->sig_fd < 0)
		goto out;
	if (mkdir(pss_run_dir(), 0755) != 0 && errno != EEXIST) {
		pss_log("mkdir %s: %s", pss_run_dir(), strerror(errno));
		goto out;
	}
	if (store_open(&h->store, pss_data_dir()) != 0)
		goto out;
	h->store_open = 1;
	if (write_boot(h) != 0)
		goto out;
	{
		struct arm_change c;
		int64_t t = pss_mono_ns();

		if (arm_restore(&h->arm, h->store.arm_state, h->store.arm_event_id, t, &c)) {
			if (write_arm(h, &c, t) != 0)
				goto out;
		} else {
			pss_log("arm: %s (restored)", arm_state_name(h->arm.state));
		}
	}
	if (ipc_sock_path(h->sock_path, sizeof h->sock_path) != 0)
		goto out;
	h->listen_fd = ipc_listen(h->sock_path);
	if (h->listen_fd < 0)
		goto out;
	h->tick_fd = pss_timerfd(PSS_NS_PER_S, PSS_NS_PER_S);
	if (h->tick_fd < 0)
		goto out;
	h->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (h->epoll_fd < 0) {
		pss_log("epoll_create1: %s", strerror(errno));
		goto out;
	}
	if (epoll_add(h, h->listen_fd, TAG_LISTEN) != 0 || epoll_add(h, h->sig_fd, TAG_SIG) != 0 ||
	    epoll_add(h, h->tick_fd, TAG_TICK) != 0)
		goto out;

	h->start_ns = pss_mono_ns();
	h->next_hb_ns = h->start_ns + HB_FIRST_NS;
	pss_log("start pid=%d data=%s sock=%s quiet=%" PRId64 "ms burst=%" PRIu32 "x%" PRIu32 "ms k_imp=%.0f k_voice=%.0f warmup=%" PRIu32 " t_co=%" PRId64 "ms reburst=%" PRId64 "ms cap=%" PRId64 "ms",
		(int)getpid(), pss_data_dir(), h->sock_path, h->quiet_ns / PSS_NS_PER_MS,
		h->burst_frames, h->burst_interval_ms, h->mic.k_imp, h->mic.k_voice, h->mic.warmup,
		h->t_co_ns / PSS_NS_PER_MS, h->reburst_ns / PSS_NS_PER_MS, h->reburst_cap_ns / PSS_NS_PER_MS);

	for (;;) {
		struct epoll_event evs[16];
		int n = epoll_wait(h->epoll_fd, evs, 16, -1);
		int64_t now;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			pss_log("epoll_wait: %s", strerror(errno));
			goto out;
		}
		now = pss_mono_ns();
		for (int i = 0; i < n; i++) {
			uint32_t tag = evs[i].data.u32;

			if (tag == TAG_SIG) {
				struct signalfd_siginfo si;

				if (read(h->sig_fd, &si, sizeof si) != (ssize_t)sizeof si)
					pss_log("signalfd read: %s", strerror(errno));
				else
					pss_log("signal %u, shutting down", si.ssi_signo);
				if (h->ev.open) {
					if (close_event(h, now, PSS_EVF_CLOSED_AT_SHUTDOWN) != 0)
						goto out;
				}
				code = 0;
				goto out;
			} else if (tag == TAG_LISTEN) {
				accept_all(h, now);
			} else if (tag == TAG_TICK) {
				if (pss_timerfd_drain(h->tick_fd) != 0 || tick(h, now) != 0)
					goto out;
			} else if (tag < MAX_CONNS) {
				conn_readable(h, (int)tag, now);
			}
			if (h->fatal)
				goto out;
		}
	}
out:
	for (int i = 0; i < MAX_CONNS; i++)
		if (h->conns[i].fd >= 0)
			close(h->conns[i].fd);
	if (h->epoll_fd >= 0)
		close(h->epoll_fd);
	if (h->listen_fd >= 0) {
		close(h->listen_fd);
		if (unlink(h->sock_path) != 0)
			pss_log("unlink %s: %s", h->sock_path, strerror(errno));
	}
	if (h->tick_fd >= 0)
		close(h->tick_fd);
	if (h->sig_fd >= 0)
		close(h->sig_fd);
	if (h->store_open)
		store_close(&h->store);
	pss_log("exit %d", code);
	return code;
}
