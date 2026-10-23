/*
 * camd: Arducam OV5647 evidence bursts (Design.md §2). On CAM_TRIGGER from the
 * hub it runs `rpicam-still --timelapse` into clips/<id>.dir.tmp and reports
 * CAM_DONE. The hub, not camd, commits or discards the burst (single writer).
 *
 * $PSS_RPICAM overrides the rpicam-still path (tests use a stand-in script).
 * Exit codes: 0 on SIGTERM/SIGINT, 1 on error, 2 on usage.
 */
#include "common/client.h"
#include "common/log.h"
#include "common/time.h"
#include "common/util.h"
#include "store/clip.h"
#include "store/dirlist.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXIT_USAGE 2
#define HB_PERIOD_NS (5 * PSS_NS_PER_S)
#define BURST_SLACK_NS (15 * PSS_NS_PER_S)

struct burst {
	pid_t pid;		/* 0 when idle */
	uint64_t event_id;
	int64_t deadline_ns;
	int killed;
};

struct camd {
	struct pss_client cl;
	struct burst b;
	const char *rpicam;
	char clips_path[PATH_MAX];
	uint32_t width, height;
	uint64_t bursts, failures;
};

static void usage(void)
{
	fprintf(stderr, "usage: camd [--width=1296] [--height=972]\n");
}

static void reply(struct camd *c, uint64_t event_id, int32_t status, uint32_t frames)
{
	struct pss_msg m;

	pss_msg_init(&m, PSS_MSG_CAM_DONE);
	m.u.cam_done.event_id = event_id;
	m.u.cam_done.status = status;
	m.u.cam_done.frames = frames;
	if (client_send(&c->cl, &m) != 0)
		pss_log("burst %" PRIu64 ": result not delivered (hub away); hub will clean up", event_id);
}

static pid_t spawn_rpicam(const struct camd *c, const char *outpat, uint32_t frames, uint32_t interval_ms)
{
	char t_ms[16], tl_ms[16], w[16], h[16];
	char *argv[] = {
		(char *)c->rpicam, "-n", "-t", t_ms, "--timelapse", tl_ms,
		"--width", w, "--height", h, "-q", "85", "-o", (char *)outpat, NULL,
	};
	pid_t pid;

	snprintf(t_ms, sizeof t_ms, "%" PRIu32, frames * interval_ms);
	snprintf(tl_ms, sizeof tl_ms, "%" PRIu32, interval_ms);
	snprintf(w, sizeof w, "%" PRIu32, c->width);
	snprintf(h, sizeof h, "%" PRIu32, c->height);

	pid = fork();
	if (pid != 0)
		return pid; /* parent, or -1 */

	/* Child: undo the parent's blocked signals, stdin from /dev/null. */
	sigset_t none;
	int devnull;

	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	devnull = open("/dev/null", O_RDONLY);
	if (devnull >= 0 && devnull != STDIN_FILENO) {
		dup2(devnull, STDIN_FILENO);
		close(devnull);
	}
	execvp(argv[0], argv);
	dprintf(STDERR_FILENO, "camd: exec %s: %s\n", argv[0], strerror(errno));
	_exit(127);
}

static void start_burst(struct camd *c, const struct pss_msg *m, int64_t now)
{
	uint64_t id = m->u.cam_trigger.event_id;
	uint32_t frames = m->u.cam_trigger.frames, interval = m->u.cam_trigger.interval_ms;
	char tmp[48], outpat[PATH_MAX + 64];
	int clips_fd, r;
	pid_t pid;

	if (c->b.pid > 0) {
		pss_log("burst %" PRIu64 " refused: burst %" PRIu64 " still running", id, c->b.event_id);
		reply(c, id, -EBUSY, 0);
		return;
	}
	if (frames == 0 || frames > 1000 || interval == 0 || interval > 10000) {
		pss_log("burst %" PRIu64 " refused: bad parameters %" PRIu32 "x%" PRIu32 "ms", id, frames, interval);
		reply(c, id, -EINVAL, 0);
		return;
	}
	if (clip_tmp_name(tmp, sizeof tmp, id) != 0) {
		reply(c, id, -ENAMETOOLONG, 0);
		return;
	}
	r = snprintf(outpat, sizeof outpat, "%s/%s/img%%04d.jpg", c->clips_path, tmp);
	if (r < 0 || (size_t)r >= sizeof outpat) {
		pss_log("burst %" PRIu64 ": output path too long", id);
		reply(c, id, -ENAMETOOLONG, 0);
		return;
	}
	clips_fd = open(c->clips_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (clips_fd < 0) {
		r = errno;
		pss_log("burst %" PRIu64 ": open %s: %s", id, c->clips_path, strerror(r));
		reply(c, id, -r, 0);
		return;
	}
	if (mkdirat(clips_fd, tmp, 0755) != 0) {
		r = errno;
		pss_log("burst %" PRIu64 ": mkdir %s: %s", id, tmp, strerror(r));
		close(clips_fd);
		reply(c, id, -r, 0);
		return;
	}
	if (close(clips_fd) != 0)
		pss_log("close clips/: %s", strerror(errno));

	pid = spawn_rpicam(c, outpat, frames, interval);
	if (pid < 0) {
		r = errno;
		pss_log("burst %" PRIu64 ": fork: %s", id, strerror(r));
		reply(c, id, -r, 0);
		return;
	}
	c->b = (struct burst){
		.pid = pid,
		.event_id = id,
		.deadline_ns = now + (int64_t)frames * interval * PSS_NS_PER_MS + BURST_SLACK_NS,
	};
	c->bursts++;
	pss_log("burst %" PRIu64 " started: %" PRIu32 " frames every %" PRIu32 " ms (%s pid %d)",
		id, frames, interval, c->rpicam, (int)pid);
}

static uint32_t count_jpgs(const struct camd *c, uint64_t id)
{
	char tmp[48], path[PATH_MAX + 64];
	struct dirlist dl;
	uint32_t n = 0;
	int fd;

	if (clip_tmp_name(tmp, sizeof tmp, id) != 0)
		return 0;
	snprintf(path, sizeof path, "%s/%s", c->clips_path, tmp);
	fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		pss_log("burst %" PRIu64 ": open %s: %s", id, path, strerror(errno));
		return 0;
	}
	if (dirlist_read(fd, &dl) == 0) {
		for (size_t i = 0; i < dl.n; i++) {
			size_t len = strlen(dl.names[i]);

			if (len > 4 && strcmp(dl.names[i] + len - 4, ".jpg") == 0)
				n++;
		}
		dirlist_free(&dl);
	}
	if (close(fd) != 0)
		pss_log("close %s: %s", path, strerror(errno));
	return n;
}

static void reap_children(struct camd *c)
{
	for (;;) {
		int st;
		pid_t pid = waitpid(-1, &st, WNOHANG);

		if (pid == 0)
			return;
		if (pid < 0) {
			if (errno != ECHILD)
				pss_log("waitpid: %s", strerror(errno));
			return;
		}
		if (pid != c->b.pid)
			continue;

		int32_t status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
		uint32_t frames = count_jpgs(c, c->b.event_id);

		if (status == 0 && frames > 0) {
			pss_log("burst %" PRIu64 " done: %" PRIu32 " frames", c->b.event_id, frames);
		} else {
			c->failures++;
			pss_log("%s exit %" PRId32 "; burst %" PRIu64 " skipped (%" PRIu32 " frames)",
				c->rpicam, status, c->b.event_id, frames);
			if (status == 0)
				status = -ENODATA;
		}
		reply(c, c->b.event_id, status, frames);
		c->b = (struct burst){ 0 };
	}
}

static void send_heartbeat(struct camd *c)
{
	struct pss_msg m;

	pss_msg_init(&m, PSS_MSG_HEARTBEAT);
	m.u.hb.dropped = c->cl.dropped;
	m.u.hb.count = c->bursts;
	client_send(&c->cl, &m);
}

/* Never leave an rpicam-still behind across our own restart. */
static void kill_burst(struct camd *c)
{
	int st;

	if (c->b.pid <= 0)
		return;
	if (kill(c->b.pid, SIGKILL) != 0 && errno != ESRCH)
		pss_log("kill %d: %s", (int)c->b.pid, strerror(errno));
	while (waitpid(c->b.pid, &st, 0) < 0 && errno == EINTR)
		;
	pss_log("burst %" PRIu64 " aborted at shutdown", c->b.event_id);
	c->b = (struct burst){ 0 };
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "width", required_argument, NULL, 'W' },
		{ "height", required_argument, NULL, 'H' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const int sigs[] = { SIGINT, SIGTERM, SIGCHLD };
	static struct camd c;
	int sig_fd = -1, hb_fd = -1, code = 1, opt, r;

	pss_log_init("camd");
	c.cl.fd = -1;
	c.width = 1296;
	c.height = 972;
	while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (opt) {
		case 'W':
			if (pss_parse_u32(optarg, 64, 4608, &c.width) != 0) { usage(); return EXIT_USAGE; }
			break;
		case 'H':
			if (pss_parse_u32(optarg, 64, 3456, &c.height) != 0) { usage(); return EXIT_USAGE; }
			break;
		default: usage(); return opt == 'h' ? 0 : EXIT_USAGE;
		}
	}
	c.rpicam = getenv("PSS_RPICAM");
	if (c.rpicam == NULL || c.rpicam[0] == '\0')
		c.rpicam = "rpicam-still";
	r = snprintf(c.clips_path, sizeof c.clips_path, "%s/clips", pss_data_dir());
	if (r < 0 || (size_t)r >= sizeof c.clips_path) {
		pss_log("data dir path too long");
		return 1;
	}

	sig_fd = pss_signalfd(sigs, 3);
	if (sig_fd < 0)
		goto out;
	if (client_init(&c.cl, PSS_ROLE_CAM) != 0)
		goto out;
	hb_fd = pss_timerfd(HB_PERIOD_NS, HB_PERIOD_NS);
	if (hb_fd < 0)
		goto out;
	pss_log("start pid=%d clips=%s rpicam=%s %" PRIu32 "x%" PRIu32,
		(int)getpid(), c.clips_path, c.rpicam, c.width, c.height);

	for (;;) {
		struct pollfd pfd[3];
		int64_t now = pss_mono_ns();
		int nfds = 2, timeout = -1;

		if (client_poll_connect(&c.cl, now))
			send_heartbeat(&c);
		if (c.b.pid > 0 && !c.b.killed && now > c.b.deadline_ns) {
			pss_log("burst %" PRIu64 ": %s overran its deadline, SIGKILL", c.b.event_id, c.rpicam);
			if (kill(c.b.pid, SIGKILL) != 0 && errno != ESRCH)
				pss_log("kill %d: %s", (int)c.b.pid, strerror(errno));
			c.b.killed = 1;
		}

		pfd[0] = (struct pollfd){ .fd = sig_fd, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = hb_fd, .events = POLLIN };
		if (c.cl.fd >= 0)
			pfd[nfds++] = (struct pollfd){ .fd = c.cl.fd, .events = POLLIN };
		if (c.cl.fd < 0)
			timeout = 250;
		else if (c.b.pid > 0 && !c.b.killed)
			timeout = 1000;

		if (poll(pfd, (nfds_t)nfds, timeout) < 0) {
			if (errno == EINTR)
				continue;
			pss_log("poll: %s", strerror(errno));
			goto out;
		}
		now = pss_mono_ns();
		if (pfd[0].revents & POLLIN) {
			struct signalfd_siginfo si;
			ssize_t n;

			while ((n = read(sig_fd, &si, sizeof si)) == (ssize_t)sizeof si) {
				if (si.ssi_signo == SIGCHLD)
					continue;
				pss_log("signal %u, exiting (bursts=%" PRIu64 " failures=%" PRIu64 ")",
					si.ssi_signo, c.bursts, c.failures);
				code = 0;
				goto out;
			}
			if (n < 0 && errno != EAGAIN) {
				pss_log("signalfd read: %s", strerror(errno));
				goto out;
			}
			reap_children(&c);
		}
		if (pfd[1].revents & POLLIN) {
			if (pss_timerfd_drain(hb_fd) != 0)
				goto out;
			send_heartbeat(&c);
		}
		if (nfds == 3 && pfd[2].revents) {
			struct pss_msg m;

			while ((r = ipc_recv(c.cl.fd, &m)) == 1) {
				if (m.type == PSS_MSG_CAM_TRIGGER)
					start_burst(&c, &m, now);
				else
					pss_log("unexpected message type %u from hub", m.type);
			}
			if (r == 0)
				client_disconnect(&c.cl, now, "hub closed");
			else if (errno != EAGAIN)
				client_disconnect(&c.cl, now, strerror(errno));
		}
	}
out:
	kill_burst(&c);
	client_close(&c.cl);
	if (hb_fd >= 0)
		close(hb_fd);
	if (sig_fd >= 0)
		close(sig_fd);
	return code;
}
