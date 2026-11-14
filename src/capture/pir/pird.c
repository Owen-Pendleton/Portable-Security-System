/*
 * pird: HC-SR501 PIR and the arm button on Pi 5 GPIO lines (mechanism B,
 * DESIGN.md Section 3).
 *
 * Default (IRQ) mode requests both lines with both-edge detection and kernel
 * CLOCK_MONOTONIC timestamps, then sleeps in poll() until the GPIO interrupt
 * delivers an edge. --poll=<ms> reads the line values on a timerfd instead;
 * it is the comparison arm for the B measurements.
 *
 * The button (pressed = low, Pi pull-up) is debounced by the kernel (IRQ mode)
 * and by button_logic.c (both modes); each press goes to the hub as one
 * PSS_MSG_BUTTON. The PIR warm-up does not apply to it.
 *
 * Exit codes: 0 on SIGTERM/SIGINT, 1 on error, 2 on usage, 3 when the GPIO
 * chip disappears (supervisor treats 3 as "device gone", DESIGN.md Section 3, mechanism E).
 */
#include "capture/pir/button_logic.h"
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
	unsigned int offsets[2];	/* [0] PIR, [1] button (if nlines == 2) */
	unsigned int nlines;
	uint32_t poll_ms;
	uint32_t debounce_ms;
	int fd;			/* line request fd (IRQ) or timerfd (poll) */
};

struct pird {
	struct pss_client cl;
	struct pir_filter pir;
	struct btn_filter btn;
};

static void usage(void)
{
	fprintf(stderr, "usage: pird [--chip=/dev/gpiochip0] [--line=17] [--poll=MS] [--warmup=SEC]\n"
			"            [--button=27|none] [--button-debounce-ms=20]\n"
			"  --poll=0 (default) waits for GPIO edge interrupts\n"
			"  --button-debounce-ms=0 turns debouncing off (to measure raw bounce)\n");
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

/* Input, pull-up, and (IRQ mode) both edges with kernel timestamps. 0 or -1, logged. */
static int input_settings(struct gpiod_line_settings *ls, int irq)
{
	if (gpiod_line_settings_set_direction(ls, GPIOD_LINE_DIRECTION_INPUT) != 0 ||
	    gpiod_line_settings_set_bias(ls, GPIOD_LINE_BIAS_PULL_UP) != 0) {
		pss_log("gpiod line settings: %s", strerror(errno));
		return -1;
	}
	if (irq && (gpiod_line_settings_set_edge_detection(ls, GPIOD_LINE_EDGE_BOTH) != 0 ||
		    gpiod_line_settings_set_event_clock(ls, GPIOD_LINE_CLOCK_MONOTONIC) != 0)) {
		pss_log("gpiod edge settings: %s", strerror(errno));
		return -1;
	}
	return 0;
}

/* Reads every requested line into levels[] (1 = high). 0, or an exit code. */
static int src_get_levels(struct pir_src *p, int levels[2])
{
	enum gpiod_line_value v[2];

	if (gpiod_line_request_get_values_subset(p->req, p->nlines, p->offsets, v) != 0) {
		pss_log("gpiod_line_request_get_values_subset: %s", strerror(errno));
		return errno == ENODEV ? EXIT_DEVICE_GONE : 1;
	}
	for (unsigned int i = 0; i < p->nlines; i++)
		levels[i] = v[i] == GPIOD_LINE_VALUE_ACTIVE;
	return 0;
}

/* Requests the lines; levels[] gets their current values. Returns 0, or an exit code. */
static int src_open(struct pir_src *p, const char *chip_path, int levels[2])
{
	struct gpiod_line_settings *ls = NULL, *lsb = NULL;
	struct gpiod_line_config *lc = NULL;
	struct gpiod_request_config *rc = NULL;
	int irq = p->poll_ms == 0;
	int ret = 1;

	p->chip = gpiod_chip_open(chip_path);
	if (p->chip == NULL) {
		pss_log("gpiod_chip_open %s: %s", chip_path, strerror(errno));
		ret = (errno == ENOENT || errno == ENODEV) ? EXIT_DEVICE_GONE : 1;
		goto out;
	}
	ls = gpiod_line_settings_new();
	lsb = gpiod_line_settings_new();
	lc = gpiod_line_config_new();
	rc = gpiod_request_config_new();
	if (ls == NULL || lsb == NULL || lc == NULL || rc == NULL) {
		pss_log("gpiod: out of memory");
		goto out;
	}
	if (input_settings(ls, irq) != 0 || input_settings(lsb, irq) != 0)
		goto out;
	/* Kernel debounce on the button only; the PIR output is clean. */
	if (irq)
		gpiod_line_settings_set_debounce_period_us(lsb, (unsigned long)p->debounce_ms * 1000UL);
	if (gpiod_line_config_add_line_settings(lc, &p->offsets[0], 1, ls) != 0 ||
	    (p->nlines == 2 && gpiod_line_config_add_line_settings(lc, &p->offsets[1], 1, lsb) != 0)) {
		pss_log("gpiod_line_config_add_line_settings: %s", strerror(errno));
		goto out;
	}
	gpiod_request_config_set_consumer(rc, "pird");
	p->req = gpiod_chip_request_lines(p->chip, rc, lc);
	if (p->req == NULL) {
		pss_log("gpiod_chip_request_lines %s lines %u%s: %s", chip_path, p->offsets[0],
			p->nlines == 2 ? "+button" : "", strerror(errno));
		goto out;
	}
	ret = src_get_levels(p, levels);
	if (ret != 0)
		goto out;
	ret = 1;

	if (irq) {
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
	if (lsb != NULL)
		gpiod_line_settings_free(lsb);
	if (ls != NULL)
		gpiod_line_settings_free(ls);
	if (ret != 0)
		src_close(p);
	return ret;
}

static void on_edge(struct pird *d, int rising, int64_t ts_ns, int64_t now_ns, int irq)
{
	enum pir_verdict v = pir_filter_edge(&d->pir, rising, ts_ns);
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
	client_send(&d->cl, &m);
}

static void on_button_edge(struct pird *d, int rising, int64_t ts_ns, int64_t now_ns, int irq)
{
	int64_t gap_us = (ts_ns - d->btn.last_edge_ns) / 1000;
	enum btn_verdict v = btn_filter_edge(&d->btn, rising, ts_ns);
	char lat[32] = "(poll)";
	struct pss_msg m;

	if (irq)
		snprintf(lat, sizeof lat, "lat=%" PRId64 "us", (now_ns - ts_ns) / 1000);
	/* +gap is the time since the previous button edge: with debounce off it
	 * shows the bounce train, which is how the debounce period is chosen. */
	if (v == BTN_PRESS)
		pss_log("button %s %s +%" PRId64 "us press #%" PRIu64, rising ? "rise" : "fall", lat, gap_us,
			d->btn.presses);
	else
		pss_log("button %s %s +%" PRId64 "us%s", rising ? "rise" : "fall", lat, gap_us,
			v == BTN_RELEASE ? " (release)" : v == BTN_BOUNCE ? " (bounce, ignored)" : " (duplicate, ignored)");
	if (v != BTN_PRESS)
		return;
	pss_msg_init(&m, PSS_MSG_BUTTON);
	m.mono_ns = ts_ns;
	m.u.button.presses = (uint32_t)d->btn.presses;
	client_send(&d->cl, &m);
}

/* Returns 0, or an exit code. */
static int src_readable(struct pir_src *p, struct pird *d)
{
	int64_t now;
	int levels[2];
	int r;

	if (p->poll_ms == 0) {
		int n = gpiod_line_request_read_edge_events(p->req, p->evbuf, EVBUF_SIZE);

		if (n < 0) {
			pss_log("gpiod_line_request_read_edge_events: %s", strerror(errno));
			return errno == ENODEV ? EXIT_DEVICE_GONE : 1;
		}
		now = pss_mono_ns();
		for (int i = 0; i < n; i++) {
			struct gpiod_edge_event *ev = gpiod_edge_event_buffer_get_event(p->evbuf, (unsigned long)i);
			int rising = gpiod_edge_event_get_event_type(ev) == GPIOD_EDGE_EVENT_RISING_EDGE;
			int64_t ts = (int64_t)gpiod_edge_event_get_timestamp_ns(ev);

			if (gpiod_edge_event_get_line_offset(ev) == p->offsets[0])
				on_edge(d, rising, ts, now, 1);
			else
				on_button_edge(d, rising, ts, now, 1);
		}
		return 0;
	}

	if (pss_timerfd_drain(p->fd) != 0)
		return 1;
	r = src_get_levels(p, levels);
	if (r != 0)
		return r;
	now = pss_mono_ns();
	if (levels[0] != d->pir.level)
		on_edge(d, levels[0], now, now, 0);
	if (p->nlines == 2 && levels[1] != d->btn.level)
		on_button_edge(d, levels[1], now, now, 0);
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
		{ "button", required_argument, NULL, 'b' },
		{ "button-debounce-ms", required_argument, NULL, 'd' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const int sigs[] = { SIGINT, SIGTERM };
	const char *chip_path = "/dev/gpiochip0";
	struct pir_src src = { .fd = -1, .debounce_ms = 20 };
	struct pird d;
	uint32_t line = 17, button = 27, warmup_s = 60;
	int sig_fd = -1, hb_fd = -1, levels[2] = { 0, 1 }, use_button = 1, armed = 0, code = 1, opt;
	int64_t start;

	pss_log_init("pird");
	d.cl.fd = -1;
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
		case 'b':
			if (strcmp(optarg, "none") == 0)
				use_button = 0;
			else if (pss_parse_u32(optarg, 0, 1023, &button) != 0) { usage(); return EXIT_USAGE; }
			else
				use_button = 1;
			break;
		case 'd':
			if (pss_parse_u32(optarg, 0, 1000, &src.debounce_ms) != 0) { usage(); return EXIT_USAGE; }
			break;
		default: usage(); return opt == 'h' ? 0 : EXIT_USAGE;
		}
	}
	if (use_button && button == line) {
		fprintf(stderr, "pird: --button and --line must differ\n");
		return EXIT_USAGE;
	}
	src.offsets[0] = line;
	src.offsets[1] = button;
	src.nlines = use_button ? 2 : 1;

	sig_fd = pss_signalfd(sigs, 2);
	if (sig_fd < 0)
		goto out;
	if (client_init(&d.cl, PSS_ROLE_PIR) != 0)
		goto out;
	code = src_open(&src, chip_path, levels);
	if (code != 0)
		goto out;
	code = 1;
	hb_fd = pss_timerfd(HB_PERIOD_NS, HB_PERIOD_NS);
	if (hb_fd < 0)
		goto out;
	start = pss_mono_ns();
	pir_filter_init(&d.pir, start, (int64_t)warmup_s * PSS_NS_PER_S, levels[0]);
	btn_filter_init(&d.btn, start, (int64_t)src.debounce_ms * PSS_NS_PER_MS, levels[1]);
	pss_log("start pid=%d chip=%s line=%" PRIu32 " mode=%s level=%d; warm-up, edges ignored until T+%" PRIu32 "s",
		(int)getpid(), chip_path, line, src.poll_ms ? "poll" : "irq", levels[0], warmup_s);
	if (use_button)
		pss_log("button line=%" PRIu32 " level=%d%s debounce=%" PRIu32 "ms%s, no warm-up", button, levels[1],
			levels[1] ? "" : " (held)", src.debounce_ms, src.poll_ms ? " (software only)" : " (kernel+software)");
	else
		pss_log("button disabled");
	if (src.poll_ms)
		pss_log("polling every %" PRIu32 " ms", src.poll_ms);

	for (;;) {
		struct pollfd pfd[4];
		int64_t now = pss_mono_ns();
		int nfds = 3, timeout;

		if (client_poll_connect(&d.cl, now))
			send_heartbeat(&d.cl, &d.pir);
		if (!armed && now >= d.pir.warmup_until_ns) {
			pss_log("warm-up over, armed (line level=%d)", d.pir.level);
			armed = 1;
		}

		pfd[0] = (struct pollfd){ .fd = sig_fd, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = src.fd, .events = POLLIN };
		pfd[2] = (struct pollfd){ .fd = hb_fd, .events = POLLIN };
		if (d.cl.fd >= 0)
			pfd[nfds++] = (struct pollfd){ .fd = d.cl.fd, .events = POLLIN };
		timeout = d.cl.fd < 0 ? 250 : armed ? -1 : 1000;

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
				pss_log("signal %u, exiting (forwarded=%" PRIu64 " presses=%" PRIu64 " bounces=%" PRIu64
					" dropped=%" PRIu64 ")",
					si.ssi_signo, d.pir.forwarded, d.btn.presses, d.btn.bounces, d.cl.dropped);
			code = 0;
			goto out;
		}
		if (pfd[1].revents & (POLLIN | POLLERR | POLLHUP)) {
			int r = src_readable(&src, &d);

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
			send_heartbeat(&d.cl, &d.pir);
		}
		if (nfds == 4 && pfd[3].revents) {
			struct pss_msg m;
			int r = ipc_recv(d.cl.fd, &m);

			if (r == 0)
				client_disconnect(&d.cl, now, "hub closed");
			else if (r < 0 && errno != EAGAIN)
				client_disconnect(&d.cl, now, strerror(errno));
		}
	}
out:
	client_close(&d.cl);
	src_close(&src);
	if (hb_fd >= 0)
		close(hb_fd);
	if (sig_fd >= 0)
		close(sig_fd);
	return code;
}
