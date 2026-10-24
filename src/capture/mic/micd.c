/*
 * micd: USB microphone capture + feature extraction (DESIGN.md Section 2, Section 4).
 *
 * Two threads joined by an SPSC ring: the capture thread blocks in
 * snd_pcm_readi() and pushes 20 ms chunks; the main thread wakes on an
 * eventfd, computes features every 882-sample hop over a 1024-sample window,
 * and sends MIC_FRAME messages to the hub. The ring holds ~11.9 s of audio,
 * the size the 10 s pre-roll clip will need (M4).
 *
 * M3 status: written and unit-tested (features) off-device; NOT yet run on
 * the Blue Snowball, which has not arrived.
 *
 * Exit codes: 0 on SIGTERM/SIGINT, 1 on error, 2 on usage, 3 when the
 * device disappears (unplugged), per DESIGN.md Section 6.
 */
#include "capture/mic/features.h"
#include "common/client.h"
#include "common/log.h"
#include "common/time.h"
#include "common/util.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <unistd.h>

#define EXIT_USAGE 2
#define EXIT_DEVICE_GONE 3
#define RATE 44100
#define LATENCY_US 200000
#define RING_SIZE (1u << 19)	/* samples; ~11.9 s at 44.1 kHz */
#define RING_MASK (RING_SIZE - 1)
#define HB_PERIOD_NS (5 * PSS_NS_PER_S)

struct micd {
	snd_pcm_t *pcm;
	int16_t *ring;
	_Atomic uint64_t head;	/* written by capture thread */
	_Atomic uint64_t tail;	/* written by main thread */
	int wake_fd;
	atomic_int stop;
	atomic_int fatal;	/* exit code requested by the capture thread */
	atomic_ulong xruns;
	atomic_ulong overflow;	/* samples dropped because the ring was full */
};

static void usage(void)
{
	fprintf(stderr, "usage: micd [--device=default]\n");
}

static void ring_push(struct micd *m, const int16_t *src, uint64_t n)
{
	uint64_t head = atomic_load_explicit(&m->head, memory_order_relaxed);
	uint64_t tail = atomic_load_explicit(&m->tail, memory_order_acquire);
	uint64_t space = RING_SIZE - (head - tail);

	if (n > space) {
		atomic_fetch_add(&m->overflow, (unsigned long)(n - space));
		n = space;
	}
	for (uint64_t i = 0; i < n; i++)
		m->ring[(head + i) & RING_MASK] = src[i];
	atomic_store_explicit(&m->head, head + n, memory_order_release);
}

static uint64_t ring_avail(struct micd *m)
{
	return atomic_load_explicit(&m->head, memory_order_acquire) -
	       atomic_load_explicit(&m->tail, memory_order_relaxed);
}

static void ring_pop(struct micd *m, int16_t *dst, uint64_t n)
{
	uint64_t tail = atomic_load_explicit(&m->tail, memory_order_relaxed);

	for (uint64_t i = 0; i < n; i++)
		dst[i] = m->ring[(tail + i) & RING_MASK];
	atomic_store_explicit(&m->tail, tail + n, memory_order_release);
}

static void wake(struct micd *m)
{
	uint64_t one = 1;

	if (write(m->wake_fd, &one, sizeof one) < 0 && errno != EAGAIN)
		pss_log("eventfd write: %s", strerror(errno));
}

static void *capture_main(void *arg)
{
	struct micd *m = arg;
	int16_t chunk[PSS_HOP];

	while (!atomic_load(&m->stop)) {
		snd_pcm_sframes_t n = snd_pcm_readi(m->pcm, chunk, PSS_HOP);

		if (n == -EPIPE || n == -ESTRPIPE) {
			unsigned long x = atomic_fetch_add(&m->xruns, 1) + 1;
			int r = snd_pcm_prepare(m->pcm);

			if (r < 0) {
				pss_log("snd_pcm_prepare after %s: %s", n == -EPIPE ? "xrun" : "suspend", snd_strerror(r));
				atomic_store(&m->fatal, 1);
				break;
			}
			pss_log("xrun #%lu recovered", x);
			continue;
		}
		if (n == -EAGAIN || n == -EINTR)
			continue;
		if (n < 0) {
			int gone = (n == -ENODEV || n == -EIO);

			pss_log("snd_pcm_readi: %s%s", snd_strerror((int)n), gone ? "; exit 3" : "");
			atomic_store(&m->fatal, gone ? EXIT_DEVICE_GONE : 1);
			break;
		}
		ring_push(m, chunk, (uint64_t)n);
		wake(m);
	}
	wake(m); /* let the main thread see stop/fatal */
	return NULL;
}

/*
 * snd_pcm_set_params() sets start_threshold to the whole buffer (8820
 * frames). The kernel only auto-starts capture when one read asks for at
 * least that many frames, and we read one 882-frame hop at a time, so the
 * stream never started and each read timed out with -EIO (seen on the Pi
 * with strace). Start on the first frame, like arecord. Returns 0 or 1.
 */
static int set_start_threshold(snd_pcm_t *pcm, const char *device)
{
	snd_pcm_sw_params_t *sw = NULL;
	int r, code = 1;

	r = snd_pcm_sw_params_malloc(&sw);
	if (r < 0) {
		pss_log("snd_pcm_sw_params_malloc: %s", snd_strerror(r));
		goto out;
	}
	r = snd_pcm_sw_params_current(pcm, sw);
	if (r < 0) {
		pss_log("snd_pcm_sw_params_current %s: %s", device, snd_strerror(r));
		goto out;
	}
	r = snd_pcm_sw_params_set_start_threshold(pcm, sw, 1);
	if (r < 0) {
		pss_log("snd_pcm_sw_params_set_start_threshold %s: %s", device, snd_strerror(r));
		goto out;
	}
	r = snd_pcm_sw_params(pcm, sw);
	if (r < 0) {
		pss_log("snd_pcm_sw_params %s: %s", device, snd_strerror(r));
		goto out;
	}
	code = 0;
out:
	snd_pcm_sw_params_free(sw);
	return code;
}

/* Returns 0, or an exit code. */
static int open_pcm(struct micd *m, const char *device)
{
	int r = snd_pcm_open(&m->pcm, device, SND_PCM_STREAM_CAPTURE, 0);

	if (r < 0) {
		pss_log("snd_pcm_open %s: %s", device, snd_strerror(r));
		m->pcm = NULL;
		return (r == -ENODEV || r == -ENOENT) ? EXIT_DEVICE_GONE : 1;
	}
	/* s16 mono 44.1 kHz; allow ALSA's plug layer to convert if the device can't. */
	r = snd_pcm_set_params(m->pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
			       1, RATE, 1, LATENCY_US);
	if (r < 0) {
		pss_log("snd_pcm_set_params %s (s16 mono %d Hz): %s", device, RATE, snd_strerror(r));
		return 1;
	}
	return set_start_threshold(m->pcm, device);
}

static void send_heartbeat(struct pss_client *cl, struct micd *m, uint64_t frames)
{
	struct pss_msg msg;

	pss_msg_init(&msg, PSS_MSG_HEARTBEAT);
	msg.u.hb.dropped = cl->dropped + atomic_load(&m->overflow);
	msg.u.hb.count = frames;
	client_send(cl, &msg);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const int sigs[] = { SIGINT, SIGTERM };
	static struct micd m;
	static struct feat_state fs;
	struct pss_client cl;
	const char *device = "default";
	int16_t window[PSS_FFT_N];
	uint64_t frames = 0;
	pthread_t thread;
	int thread_started = 0, sig_fd = -1, hb_fd = -1, code = 1, opt, r;

	pss_log_init("micd");
	cl.fd = -1;
	m.wake_fd = -1;
	while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (opt) {
		case 'd': device = optarg; break;
		default: usage(); return opt == 'h' ? 0 : EXIT_USAGE;
		}
	}

	/* Block signals before starting the capture thread so only signalfd sees them. */
	sig_fd = pss_signalfd(sigs, 2);
	if (sig_fd < 0)
		goto out;
	if (client_init(&cl, PSS_ROLE_MIC) != 0)
		goto out;
	if (feat_init(&fs, RATE) != 0) {
		pss_log("feat_init failed");
		goto out;
	}
	m.ring = calloc(RING_SIZE, sizeof *m.ring);
	if (m.ring == NULL) {
		pss_log("out of memory for audio ring");
		goto out;
	}
	m.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (m.wake_fd < 0) {
		pss_log("eventfd: %s", strerror(errno));
		goto out;
	}
	hb_fd = pss_timerfd(HB_PERIOD_NS, HB_PERIOD_NS);
	if (hb_fd < 0)
		goto out;
	code = open_pcm(&m, device);
	if (code != 0)
		goto out;
	code = 1;
	r = pthread_create(&thread, NULL, capture_main, &m);
	if (r != 0) {
		pss_log("pthread_create: %s", strerror(r));
		goto out;
	}
	thread_started = 1;
	memset(window, 0, sizeof window);
	pss_log("start pid=%d device=%s %d Hz mono, hop %d / window %d", (int)getpid(), device, RATE,
		PSS_HOP, PSS_FFT_N);

	for (;;) {
		struct pollfd pfd[4];
		int64_t now = pss_mono_ns();
		int nfds = 3, fatal;

		if (client_poll_connect(&cl, now))
			send_heartbeat(&cl, &m, frames);
		pfd[0] = (struct pollfd){ .fd = sig_fd, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = m.wake_fd, .events = POLLIN };
		pfd[2] = (struct pollfd){ .fd = hb_fd, .events = POLLIN };
		if (cl.fd >= 0)
			pfd[nfds++] = (struct pollfd){ .fd = cl.fd, .events = POLLIN };
		if (poll(pfd, (nfds_t)nfds, cl.fd < 0 ? 250 : -1) < 0) {
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
				pss_log("signal %u, exiting (frames=%" PRIu64 " xruns=%lu overflow=%lu)",
					si.ssi_signo, frames, atomic_load(&m.xruns), atomic_load(&m.overflow));
			code = 0;
			goto out;
		}
		if (pfd[1].revents & POLLIN) {
			uint64_t count;

			if (read(m.wake_fd, &count, sizeof count) < 0 && errno != EAGAIN) {
				pss_log("eventfd read: %s", strerror(errno));
				goto out;
			}
			while (ring_avail(&m) >= PSS_HOP) {
				struct pss_features f;
				struct pss_msg msg;

				/* Slide the window by one hop: keep the last 142 samples, append 882. */
				memmove(window, window + PSS_HOP, (PSS_FFT_N - PSS_HOP) * sizeof window[0]);
				ring_pop(&m, window + (PSS_FFT_N - PSS_HOP), PSS_HOP);
				feat_compute(&fs, window, &f);
				frames++;
				pss_msg_init(&msg, PSS_MSG_MIC_FRAME);
				msg.u.mic.rms = f.rms;
				msg.u.mic.flux = f.flux;
				memcpy(msg.u.mic.band, f.band, sizeof msg.u.mic.band);
				client_send(&cl, &msg);
			}
			fatal = atomic_load(&m.fatal);
			if (fatal != 0) {
				code = fatal;
				goto out;
			}
		}
		if (pfd[2].revents & POLLIN) {
			if (pss_timerfd_drain(hb_fd) != 0)
				goto out;
			send_heartbeat(&cl, &m, frames);
		}
		if (nfds == 4 && pfd[3].revents) {
			struct pss_msg msg;

			r = ipc_recv(cl.fd, &msg);
			if (r == 0)
				client_disconnect(&cl, now, "hub closed");
			else if (r < 0 && errno != EAGAIN)
				client_disconnect(&cl, now, strerror(errno));
		}
	}
out:
	if (thread_started) {
		atomic_store(&m.stop, 1);
		r = pthread_join(thread, NULL);
		if (r != 0)
			pss_log("pthread_join: %s", strerror(r));
	}
	if (m.pcm != NULL) {
		r = snd_pcm_close(m.pcm);
		if (r < 0)
			pss_log("snd_pcm_close: %s", snd_strerror(r));
	}
	client_close(&cl);
	if (m.wake_fd >= 0)
		close(m.wake_fd);
	if (hb_fd >= 0)
		close(hb_fd);
	if (sig_fd >= 0)
		close(sig_fd);
	free(m.ring);
	return code;
}
