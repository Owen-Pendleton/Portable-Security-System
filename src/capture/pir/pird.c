/*
 * pird: HC-SR501 PIR on a Pi 5 GPIO line (mechanism B, DESIGN.md Section 3).
 *
 * Default (IRQ) mode requests the line with both-edge detection and kernel
 * CLOCK_MONOTONIC timestamps, then sleeps in poll() until the GPIO interrupt
 * delivers an edge. --poll=<ms> reads the line value on a timerfd instead;
 * it is the comparison arm for the B measurements.
 *
 * Exit codes: 0 on SIGTERM/SIGINT, 1 on error, 2 on usage, 3 when the GPIO
 * chip disappears (supervisor treats 3 as "device gone", DESIGN.md Section 3, mechanism E).
 */
#include "capture/pir/pir_logic.h"
#include "common/client.h"
#include "common/log.h"
#include "common/time.h"
#include "common/util.h"

#include <errno.h>
#include <getopt.h>
#include <gpiod.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#define EXIT_USAGE 2
#define EXIT_DEVICE_GONE 3
#define HB_PERIOD_NS (5 * PSS_NS_PER_S)
#define EVBUF_SIZE 16

struct pir_src {
	struct gpiod_chip *chip;
	struct gpiod_line_request *req;
	struct gpiod_edge_event_buffer *evbuf;
	unsigned int offset;
	uint32_t poll_ms;
	int fd;			/* line request fd (IRQ) or timerfd (poll) */
};

static void usage(void)
{
	fprintf(stderr, "usage: pird [--chip=/dev/gpiochip0] [--line=17] [--poll=MS] [--warmup=SEC]\n"
			"  --poll=0 (default) waits for GPIO edge interrupts\n");
}

static void src_close(struct pir_src *p)
{
	if (p->poll_ms > 0 && p->fd >= 0 && close(p->fd) != 0)
		pss_log("close timerfd: %s", strerror(errno));
	p->fd = -1;
	if (p->evbuf != NULL)
		gpiod_edge_event_buffer_free(p->evbuf);
	if (p->req != NULL)
		gpiod_line_request_release(p->req);
	if (p->chip != NULL)
		gpiod_chip_close(p->chip);
	p->evbuf = NULL;
	p->req = NULL;
	p->chip = NULL;
}

/* Requests the line; *level gets its current value. Returns 0, or an exit code. */
static int src_open(struct pir_src *p, const char *chip_path, int *level)
{
	struct gpiod_line_settings *ls = NULL;
	struct gpiod_line_config *lc = NULL;
	struct gpiod_request_config *rc = NULL;
	enum gpiod_line_value v;
	int ret = 1;

	p->chip = gpiod_chip_open(chip_path);
	if (p->chip == NULL) {
		pss_log("gpiod_chip_open %s: %s", chip_path, strerror(errno));
		ret = (errno == ENOENT || errno == ENODEV) ? EXIT_DEVICE_GONE : 1;
		goto out;
	}
	ls = gpiod_line_settings_new();
	lc = gpiod_line_config_new();
	rc = gpiod_request_config_new();
	if (ls == NULL || lc == NULL || rc == NULL) {
		pss_log("gpiod: out of memory");
		goto out;
	}
	if (gpiod_line_settings_set_direction(ls, GPIOD_LINE_DIRECTION_INPUT) != 0 ||
	    gpiod_line_settings_set_bias(ls, GPIOD_LINE_BIAS_PULL_UP) != 0) {
		pss_log("gpiod line settings: %s", strerror(errno));
		goto out;
	}
	if (p->poll_ms == 0 &&
	    (gpiod_line_settings_set_edge_detection(ls, GPIOD_LINE_EDGE_BOTH) != 0 ||
	     gpiod_line_settings_set_event_clock(ls, GPIOD_LINE_CLOCK_MONOTONIC) != 0)) {
		pss_log("gpiod edge settings: %s", strerror(errno));
		goto out;
	}
	if (gpiod_line_config_add_line_settings(lc, &p->offset, 1, ls) != 0) {
		pss_log("gpiod_line_config_add_line_settings: %s", strerror(errno));
		goto out;
	}
	gpiod_request_config_set_consumer(rc, "pird");
	p->req = gpiod_chip_request_lines(p->chip, rc, lc);
	if (p->req == NULL) {
		pss_log("gpiod_chip_request_lines %s line %u: %s", chip_path, p->offset, strerror(errno));
		goto out;
	}
	v = gpiod_line_request_get_value(p->req, p->offset);
	if (v == GPIOD_LINE_VALUE_ERROR) {
		pss_log("gpiod_line_request_get_value: %s", strerror(errno));
		goto out;
	}
	*level = v == GPIOD_LINE_VALUE_ACTIVE;

	if (p->poll_ms == 0) {
		p->evbuf = gpiod_edge_event_buffer_new(EVBUF_SIZE);
		if (p->evbuf == NULL) {
			pss_log("gpiod_edge_event_buffer_new: %s", strerror(errno));
			goto out;
		}
		p->fd = gpiod_line_request_get_fd(p->req);
	} else {
		int64_t period = (int64_t)p->poll_ms * PSS_NS_PER_MS;

		p->fd = pss_timerfd(period, period);
		if (p->fd < 0)
			goto out;
	}
	ret = 0;
out:
	if (rc != NULL)
		gpiod_request_config_free(rc);
	if (lc != NULL)
		gpiod_line_config_free(lc);
	if (ls != NULL)
		gpiod_line_settings_free(ls);
	if (ret != 0)
		src_close(p);
	return ret;
}

static void on_edge(struct pss_client *cl, struct pir_filter *f, int rising, int64_t ts_ns, int64_t now_ns, int irq)
{
	enum pir_verdict v = pir_filter_edge(f, rising, ts_ns);
	struct pss_msg m;

	/* In IRQ mode ts is the kernel's interrupt timestamp, so now - ts is the
	 * edge-to-userspace latency that B measures. In poll mode ts == now. */
	if (irq)
		pss_log("edge %s lat=%" PRId64 "us%s", rising ? "rise" : "fall", (now_ns - ts_ns) / 1000,
			v == PIR_WARMUP ? " (warm-up, ignored)" : v == PIR_DUPLICATE ? " (duplicate, ignored)" : "");
	else
		pss_log("edge %s (poll)%s", rising ? "rise" : "fall",
			v == PIR_WARMUP ? " (warm-up, ignored)" : v == PIR_DUPLICATE ? " (duplicate, ignored)" : "");
	if (v != PIR_FORWARD)
		return;
	pss_msg_init(&m, PSS_MSG_PIR_EDGE);
	m.mono_ns = ts_ns;
	m.u.pir.rising = rising ? 1 : 0;
	client_send(cl, &m);
}

/* Returns 0, or an exit code. */
static int src_readable(struct pir_src *p, struct pss_client *cl, struct pir_filter *f)
{
	int64_t now;

	if (p->poll_ms == 0) {
		int n = gpiod_line_request_read_edge_events(p->req, p->evbuf, EVBUF_SIZE);

		if (n < 0) {
			pss_log("gpiod_line_request_read_edge_events: %s", strerror(errno));
			return errno == ENODEV ? EXIT_DEVICE_GONE : 1;
		}
		now = pss_mono_ns();
		for (int i = 0; i < n; i++) {
			struct gpiod_edge_event *ev = gpiod_edge_event_buffer_get_event(p->evbuf, (unsigned long)i);

			on_edge(cl, f, gpiod_edge_event_get_event_type(ev) == GPIOD_EDGE_EVENT_RISING_EDGE,
				(int64_t)gpiod_edge_event_get_timestamp_ns(ev), now, 1);
		}
		return 0;
	}

	if (pss_timerfd_drain(p->fd) != 0)
		return 1;
	enum gpiod_line_value v = gpiod_line_request_get_value(p->req, p->offset);
	if (v == GPIOD_LINE_VALUE_ERROR) {
		pss_log("gpiod_line_request_get_value: %s", strerror(errno));
		return errno == ENODEV ? EXIT_DEVICE_GONE : 1;
	}
	now = pss_mono_ns();
	if ((v == GPIOD_LINE_VALUE_ACTIVE) != f->level)
		on_edge(cl, f, v == GPIOD_LINE_VALUE_ACTIVE, now, now, 0);
	return 0;
}

static void send_heartbeat(struct pss_client *cl, const struct pir_filter *f)
{
	struct pss_msg m;

	pss_msg_init(&m, PSS_MSG_HEARTBEAT);
	m.u.hb.dropped = cl->dropped;
	m.u.hb.count = f->forwarded;
	client_send(cl, &m);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "chip", required_argument, NULL, 'c' },
		{ "line", required_argument, NULL, 'l' },
		{ "poll", required_argument, NULL, 'p' },
		{ "warmup", required_argument, NULL, 'w' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const int sigs[] = { SIGINT, SIGTERM };
	const char *chip_path = "/dev/gpiochip0";
	struct pir_src src = { .fd = -1 };
	struct pss_client cl;
	struct pir_filter filter;
	uint32_t line = 17, warmup_s = 60;
	int sig_fd = -1, hb_fd = -1, level = 0, armed = 0, code = 1, opt;

	pss_log_init("pird");
	cl.fd = -1;
	while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (opt) {
		case 'c': chip_path = optarg; break;
		case 'l':
			if (pss_parse_u32(optarg, 0, 1023, &line) != 0) { usage(); return EXIT_USAGE; }
			break;
		case 'p':
			if (pss_parse_u32(optarg, 0, 10000, &src.poll_ms) != 0) { usage(); return EXIT_USAGE; }
			break;
		case 'w':
			if (pss_parse_u32(optarg, 0, 600, &warmup_s) != 0) { usage(); return EXIT_USAGE; }
			break;
		default: usage(); return opt == 'h' ? 0 : EXIT_USAGE;
		}
	}
	src.offset = line;

	sig_fd = pss_signalfd(sigs, 2);
	if (sig_fd < 0)
		goto out;
	if (client_init(&cl, PSS_ROLE_PIR) != 0)
		goto out;
	code = src_open(&src, chip_path, &level);
	if (code != 0)
		goto out;
	code = 1;
	hb_fd = pss_timerfd(HB_PERIOD_NS, HB_PERIOD_NS);
	if (hb_fd < 0)
		goto out;
	pir_filter_init(&filter, pss_mono_ns(), (int64_t)warmup_s * PSS_NS_PER_S, level);
	pss_log("start pid=%d chip=%s line=%" PRIu32 " mode=%s level=%d; warm-up, edges ignored until T+%" PRIu32 "s",
		(int)getpid(), chip_path, line, src.poll_ms ? "poll" : "irq", level, warmup_s);
	if (src.poll_ms)
		pss_log("polling every %" PRIu32 " ms", src.poll_ms);

	for (;;) {
		struct pollfd pfd[4];
		int64_t now = pss_mono_ns();
		int nfds = 3, timeout;

		if (client_poll_connect(&cl, now))
			send_heartbeat(&cl, &filter);
		if (!armed && now >= filter.warmup_until_ns) {
			pss_log("warm-up over, armed (line level=%d)", filter.level);
			armed = 1;
		}

		pfd[0] = (struct pollfd){ .fd = sig_fd, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = src.fd, .events = POLLIN };
		pfd[2] = (struct pollfd){ .fd = hb_fd, .events = POLLIN };
		if (cl.fd >= 0)
			pfd[nfds++] = (struct pollfd){ .fd = cl.fd, .events = POLLIN };
		timeout = cl.fd < 0 ? 250 : armed ? -1 : 1000;

		if (poll(pfd, (nfds_t)nfds, timeout) < 0) {
			if (errno == EINTR)
				continue;
			pss_log("poll: %s", strerror(errno));
			goto out;
		}
		if (pfd[0].revents & POLLIN) {
			struct signalfd_siginfo si;

			if (read(sig_fd, &si, sizeof si) != (ssize_t)sizeof si)
				pss_log("signalfd read: %s", strerror(errno));
			else
				pss_log("signal %u, exiting (forwarded=%" PRIu64 " dropped=%" PRIu64 ")",
					si.ssi_signo, filter.forwarded, cl.dropped);
			code = 0;
			goto out;
		}
		if (pfd[1].revents & (POLLIN | POLLERR | POLLHUP)) {
			int r = src_readable(&src, &cl, &filter);

			if (r != 0) {
				code = r;
				if (code == EXIT_DEVICE_GONE)
					pss_log("GPIO device gone; exit 3");
				goto out;
			}
		}
		if (pfd[2].revents & POLLIN) {
			if (pss_timerfd_drain(hb_fd) != 0)
				goto out;
			send_heartbeat(&cl, &filter);
		}
		if (nfds == 4 && pfd[3].revents) {
			struct pss_msg m;
			int r = ipc_recv(cl.fd, &m);

			if (r == 0)
				client_disconnect(&cl, now, "hub closed");
			else if (r < 0 && errno != EAGAIN)
				client_disconnect(&cl, now, strerror(errno));
		}
	}
out:
	client_close(&cl);
	src_close(&src);
	if (hb_fd >= 0)
		close(hb_fd);
	if (sig_fd >= 0)
		close(sig_fd);
	return code;
}
