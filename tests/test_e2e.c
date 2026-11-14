/*
 * End-to-end on the lab machine: real hub + real camd (with the
 * fake-rpicam-still.sh stand-in for the camera), and this test playing the
 * part of pird by sending PIR edge messages over the hub socket. The live
 * demo uses the real pird and rpicam-still on the Pi; this checks the
 * pipeline and storage around them.
 *
 * test_sound_to_storage plays micd the same way: steady fan-like frames that
 * must not trigger, then one clap frame that must open a SOUND_ONLY event.
 *
 * Fusion (DESIGN.md Section 4): motion alone and sound alone are logged but
 * take no images. test_entry_to_storage plays both pird and micd: a PIR rise
 * and a clap within T_co make an ENTRY, the only kind that fires the camera.
 *
 * Arm state (DESIGN.md Section 4): a fresh hub starts DISARMED and records
 * nothing, so every session first arms it with a button press (as pird)
 * and short --exit-delay-ms. test_arm_decisions checks the arm rules.
 */
#include "t.h"

#include "analysis/arm.h"
#include "common/ipc.h"
#include "common/time.h"
#include "store/evlog.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct seen {
	int boots, events, opens;
	uint64_t max_boot;
	struct pss_rec_event last;
	struct pss_rec_event_open last_open;
	int arms;		/* ARM records; the first 64 are kept */
	struct pss_rec_arm arm[64];
	struct pss_rec_arm last_arm;
};

static int seen_visitor(uint32_t seg, uint16_t type, uint16_t ver, const void *p, uint32_t len, void *arg)
{
	struct seen *s = arg;

	(void)seg;
	(void)ver;
	if (type == PSS_REC_EVENT && len == sizeof s->last) {
		memcpy(&s->last, p, sizeof s->last);
		s->events++;
	} else if (type == PSS_REC_EVENT_OPEN && len == sizeof s->last_open) {
		memcpy(&s->last_open, p, sizeof s->last_open);
		s->opens++;
	} else if (type == PSS_REC_BOOT && len == sizeof(struct pss_rec_boot)) {
		struct pss_rec_boot b;

		memcpy(&b, p, sizeof b);
		if (b.boot_no > s->max_boot)
			s->max_boot = b.boot_no;
		s->boots++;
	} else if (type == PSS_REC_ARM && len == sizeof s->last_arm) {
		memcpy(&s->last_arm, p, sizeof s->last_arm);
		if (s->arms < 64)
			s->arm[s->arms] = s->last_arm;
		s->arms++;
	}
	return 0;
}

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

static pid_t spawn(char *const argv[])
{
	pid_t pid = fork();

	if (pid == 0) {
		execv(argv[0], argv);
		perror(argv[0]);
		_exit(127);
	}
	return pid;
}

/* SIGTERM, then wait up to 5 s; returns the exit status or -1. */
static int stop(pid_t pid)
{
	int st;

	if (pid <= 0)
		return -1;
	kill(pid, SIGTERM);
	for (int i = 0; i < 500; i++) {
		pid_t r = waitpid(pid, &st, WNOHANG);

		if (r == pid)
			return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
		sleep_ms(10);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	return -1;
}

static int connect_retry(const char *path)
{
	for (int i = 0; i < 500; i++) {
		int fd = ipc_connect(path);

		if (fd >= 0)
			return fd;
		sleep_ms(10);
	}
	return -1;
}

static void send_edge(int fd, int rising)
{
	struct pss_msg m;

	pss_msg_init(&m, PSS_MSG_PIR_EDGE);
	m.u.pir.rising = (uint32_t)rising;
	CHECK(ipc_send(fd, &m) == 0);
}

static int connect_as(const char *sock, uint32_t role)
{
	struct pss_msg m;
	int fd = connect_retry(sock);

	CHECK(fd >= 0);
	pss_msg_init(&m, PSS_MSG_HELLO);
	m.u.hello.role = role;
	m.u.hello.pid = (uint32_t)getpid();
	CHECK(ipc_send(fd, &m) == 0);
	return fd;
}

/* fd must be connected as pird: the hub takes button presses only from it. */
static void press(int fd)
{
	struct pss_msg m;

	pss_msg_init(&m, PSS_MSG_BUTTON);
	m.u.button.presses = 1;
	CHECK(ipc_send(fd, &m) == 0);
}

static void scan(const char *data, struct seen *s)
{
	memset(s, 0, sizeof *s);
	CHECK(store_scan(data, PSS_LOG_EVENTS, seen_visitor, s) == 0);
}

/* Waits (up to 10 s) for at least n ARM records, the last in state st. */
static void wait_arm(const char *data, int n, uint32_t st, struct seen *s)
{
	for (int i = 0; i < 1000; i++) {
		scan(data, s);
		if (s->arms >= n && s->last_arm.state == st)
			return;
		sleep_ms(10);
	}
	fprintf(stderr, "wait_arm: wanted %d records ending %s, have %d ending %s\n", n,
		arm_state_name(st), s->arms, arm_state_name(s->last_arm.state));
	CHECK(!"wait_arm timed out");
}

/* A fresh hub starts DISARMED and records nothing: arm it, as pird would.
 * A hub restarted on the same data dir may already be ARMED. */
static void ensure_armed(const char *data, int pir_fd)
{
	struct seen s;

	scan(data, &s);
	if (s.last_arm.state == ARM_ARMED)
		return;
	press(pir_fd);
	wait_arm(data, s.arms + 1, ARM_ARMED, &s);
}

/* One hub+camd lifetime with one PIR-triggered event. Fills *out from disk. */
static void run_session(const char *data, struct seen *out)
{
	char hub[512], camd[512], sock[300];
	const char *bindir = getenv("PSS_BIN_DIR");
	struct pss_msg m;
	struct seen before = { 0 };
	pid_t hp, cp;
	int fd;

	snprintf(hub, sizeof hub, "%s/hub", bindir);
	snprintf(camd, sizeof camd, "%s/camd", bindir);
	if (access(data, F_OK) == 0)
		CHECK(store_scan(data, PSS_LOG_EVENTS, seen_visitor, &before) == 0);

	hp = spawn((char *[]){ hub, "--quiet-ms=300", "--burst-frames=3", "--burst-interval-ms=20",
			       "--exit-delay-ms=200", NULL });
	CHECK(hp > 0);
	CHECK(ipc_sock_path(sock, sizeof sock) == 0);
	fd = connect_retry(sock);
	CHECK(fd >= 0);
	cp = spawn((char *[]){ camd, NULL });
	CHECK(cp > 0);

	/* Act as pird. */
	pss_msg_init(&m, PSS_MSG_HELLO);
	m.u.hello.role = PSS_ROLE_PIR;
	m.u.hello.pid = (uint32_t)getpid();
	CHECK(ipc_send(fd, &m) == 0);
	sleep_ms(1000); /* let camd connect */
	ensure_armed(data, fd);
	send_edge(fd, 1);
	sleep_ms(50);
	send_edge(fd, 0);

	/* Event closes after the burst returns and 300 ms of quiet (1 s tick). */
	for (int i = 0; i < 100; i++) {
		memset(out, 0, sizeof *out);
		CHECK(store_scan(data, PSS_LOG_EVENTS, seen_visitor, out) == 0);
		if (out->events > before.events)
			break;
		sleep_ms(100);
	}
	CHECK(out->events == before.events + 1);
	CHECK(stop(cp) == 0);
	CHECK(stop(hp) == 0);
	close(fd);
}

static int count_entries(const char *dir, const char *suffix)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;

	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL) {
		size_t len = strlen(e->d_name), m = strlen(suffix);

		if (len >= m && strcmp(e->d_name + len - m, suffix) == 0)
			n++;
	}
	closedir(d);
	return n;
}

static void test_pir_to_storage(void)
{
	char root[256], data[300], run[300], fake[512], path[600];
	struct seen s = { 0 };
	uint64_t first_id;

	t_mkdtemp(root);
	snprintf(data, sizeof data, "%s/data", root);
	snprintf(run, sizeof run, "%s/run", root);
	snprintf(fake, sizeof fake, "%s/fake-rpicam-still.sh", getenv("PSS_TEST_DIR"));
	CHECK(chmod(fake, 0755) == 0);
	setenv("PSS_DATA_DIR", data, 1);
	setenv("PSS_RUN_DIR", run, 1);
	setenv("PSS_RPICAM", fake, 1);

	/* Session 1: motion alone is logged as MOTION_ONLY and takes no images
	 * (the camera is for confirmed entries only; see test_entry_to_storage). */
	unsetenv("FAKE_RPICAM_FAIL");
	run_session(data, &s);
	first_id = s.last.event_id;
	CHECK(s.boots == 1 && s.max_boot == 1);
	CHECK(s.last.kind == PSS_EV_MOTION_ONLY);
	CHECK(s.last.n_images == 0);
	CHECK(!(s.last.flags & PSS_EVF_NO_IMAGES));	/* no burst was asked for, none failed */
	CHECK(s.last.flags & PSS_EVF_AUDIO_OFFLINE); /* no micd running */
	CHECK(s.last.pir_edges == 1);
	CHECK(s.last.end_mono_ns >= s.last.start_mono_ns);
	CHECK(s.opens == 0);
	snprintf(path, sizeof path, "%s/clips/%" PRIu64 ".dir", data, first_id);
	CHECK(count_entries(path, ".jpg") == -1);	/* no clip directory at all */
	snprintf(path, sizeof path, "%s/stats", data);
	CHECK(count_entries(path, ".pel") >= 1);	/* stats log lives apart from events */
	snprintf(path, sizeof path, "%s/events", data);
	CHECK(count_entries(path, ".pel") == 1);

	/* Session 2: same data dir. Ids continue, boot count grows. (The camera
	 * failure case moved to test_entry_to_storage: only an ENTRY bursts.) */
	run_session(data, &s);
	CHECK(s.boots == 2 && s.max_boot == 2);
	CHECK(s.last.event_id == first_id + 1);
	CHECK(s.last.kind == PSS_EV_MOTION_ONLY);
	CHECK(s.last.n_images == 0);

	snprintf(path, sizeof path, "%s/clips", data);
	CHECK(count_entries(path, ".tmp") == 0);
	unsetenv("FAKE_RPICAM_FAIL");
	t_rm_rf(root);
}

/* Uniform in [-1, 1), deterministic so the test is repeatable. */
static double lcg_unit(uint32_t *st)
{
	*st = *st * 1664525u + 1013904223u;
	return (double)(*st >> 8) / (double)(1u << 23) - 1.0;
}

static void send_mic_frame(int fd, double rms, double flux)
{
	struct pss_msg m;

	pss_msg_init(&m, PSS_MSG_MIC_FRAME);
	m.u.mic.rms = (float)rms;
	m.u.mic.flux = (float)flux;
	for (int b = 0; b < PSS_NBANDS; b++)
		m.u.mic.band[b] = (float)log10(rms * rms);
	for (int i = 0; i < 500; i++) {
		if (ipc_send(fd, &m) == 0)
			return;
		if (errno != EAGAIN)
			break;
		sleep_ms(1);	/* socket buffer full: let the hub drain it */
	}
	CHECK(!"send_mic_frame failed");
}

/* Fan-like noise: level rms_lvl with +-10% jitter, flux 0.3 +- 0.1. */
static void send_fan(int fd, uint32_t *st, double rms_lvl)
{
	send_mic_frame(fd, rms_lvl * (1.0 + 0.1 * lcg_unit(st)), 0.3 + 0.1 * lcg_unit(st));
}

static int count_events(const char *data, struct seen *out)
{
	memset(out, 0, sizeof *out);
	CHECK(store_scan(data, PSS_LOG_EVENTS, seen_visitor, out) == 0);
	return out->events;
}

static void test_sound_to_storage(void)
{
	char root[256], data[300], run[300], fake[512], path[600], hub[512], camd[512], sock[300];
	const char *bindir = getenv("PSS_BIN_DIR");
	struct pss_msg m;
	struct seen s = { 0 };
	uint32_t st = 12345;
	pid_t hp, cp;
	int fd, pfd, i;

	t_mkdtemp(root);
	snprintf(data, sizeof data, "%s/data", root);
	snprintf(run, sizeof run, "%s/run", root);
	snprintf(fake, sizeof fake, "%s/fake-rpicam-still.sh", getenv("PSS_TEST_DIR"));
	CHECK(chmod(fake, 0755) == 0);
	setenv("PSS_DATA_DIR", data, 1);
	setenv("PSS_RUN_DIR", run, 1);
	setenv("PSS_RPICAM", fake, 1);
	unsetenv("FAKE_RPICAM_FAIL");
	snprintf(hub, sizeof hub, "%s/hub", bindir);
	snprintf(camd, sizeof camd, "%s/camd", bindir);

	hp = spawn((char *[]){ hub, "--quiet-ms=300", "--burst-frames=3", "--burst-interval-ms=20",
			       "--warmup-frames=250", "--onset-z=6", "--onset-db=10", "--exit-delay-ms=200", NULL });
	CHECK(hp > 0);
	CHECK(ipc_sock_path(sock, sizeof sock) == 0);
	fd = connect_retry(sock);
	CHECK(fd >= 0);
	cp = spawn((char *[]){ camd, NULL });
	CHECK(cp > 0);

	/* Act as micd. */
	pss_msg_init(&m, PSS_MSG_HELLO);
	m.u.hello.role = PSS_ROLE_MIC;
	m.u.hello.pid = (uint32_t)getpid();
	CHECK(ipc_send(fd, &m) == 0);
	sleep_ms(1000); /* let camd connect */
	pfd = connect_as(sock, PSS_ROLE_PIR);	/* only to press the arm button */
	ensure_armed(data, pfd);

	/* 10 s of a steady fan, then the fan slowly turned up ~9.5 dB over 30 s. */
	for (i = 0; i < 500; i++)
		send_fan(fd, &st, 0.02);
	for (i = 0; i < 1500; i++)
		send_fan(fd, &st, 0.02 * (1.0 + 2.0 * i / 1500.0));
	sleep_ms(1500); /* past a 1 s tick, so any event would have closed */
	CHECK(count_events(data, &s) == 0);

	/* A clap: broadband jump (big flux, ~+18 dB), then the fan again. */
	send_mic_frame(fd, 0.5, 5.0);
	for (i = 0; i < 100; i++)
		send_fan(fd, &st, 0.06);
	for (i = 0; i < 100 && count_events(data, &s) == 0; i++)
		sleep_ms(100);
	CHECK(s.events == 1);
	CHECK(s.last.kind == PSS_EV_SOUND_ONLY);
	CHECK(s.last.pir_edges == 0);
	CHECK(s.last.n_images == 0);	/* sound alone takes no images */
	CHECK(!(s.last.flags & PSS_EVF_NO_IMAGES));
	CHECK(!(s.last.flags & PSS_EVF_AUDIO_OFFLINE));
	CHECK(s.opens == 0);
	snprintf(path, sizeof path, "%s/clips/%" PRIu64 ".dir", data, s.last.event_id);
	CHECK(count_entries(path, ".jpg") == -1);	/* no clip directory at all */
	snprintf(path, sizeof path, "%s/clips", data);
	CHECK(count_entries(path, ".tmp") == 0);

	CHECK(stop(cp) == 0);
	CHECK(stop(hp) == 0);
	close(fd);
	close(pfd);
	CHECK(count_events(data, &s) == 1); /* nothing left open at shutdown */
	t_rm_rf(root);
}

/* Waits (up to 10 s) until the events log holds n complete events. */
static void wait_events(const char *data, int n, struct seen *s)
{
	for (int i = 0; i < 100 && count_events(data, s) < n; i++)
		sleep_ms(100);
	CHECK(s->events == n);
}

/* Checks the newest event is a confirmed ENTRY with 3 images on disk. */
static void check_entry_with_images(const char *data, const struct seen *s)
{
	char path[600];

	CHECK(s->last.kind == PSS_EV_ENTRY);
	CHECK(s->last.n_images == 3);
	CHECK(!(s->last.flags & PSS_EVF_NO_IMAGES));
	CHECK(s->last_open.event_id == s->last.event_id);	/* made durable when confirmed */
	CHECK(s->last_open.kind == PSS_EV_ENTRY);
	snprintf(path, sizeof path, "%s/clips/%" PRIu64 ".dir", data, s->last.event_id);
	CHECK(count_entries(path, ".jpg") == 3);
}

/*
 * One hub+camd lifetime with the test acting as both pird and micd.
 * T_co is 500 ms here. cam_fail: run only case (a), with the camera failing.
 */
static void entry_session(const char *data, int cam_fail)
{
	char hub[512], camd[512], sock[300], path[600];
	const char *bindir = getenv("PSS_BIN_DIR");
	struct seen s = { 0 };
	uint32_t st = 777;
	pid_t hp, cp;
	int pfd, mfd, n0, i;

	snprintf(hub, sizeof hub, "%s/hub", bindir);
	snprintf(camd, sizeof camd, "%s/camd", bindir);
	if (cam_fail)
		setenv("FAKE_RPICAM_FAIL", "1", 1);
	else
		unsetenv("FAKE_RPICAM_FAIL");
	n0 = access(data, F_OK) == 0 ? count_events(data, &s) : 0;

	hp = spawn((char *[]){ hub, "--quiet-ms=300", "--burst-frames=3", "--burst-interval-ms=20",
			       "--warmup-frames=50", "--t-co-ms=500", "--exit-delay-ms=200", "--grace-ms=300", NULL });
	CHECK(hp > 0);
	CHECK(ipc_sock_path(sock, sizeof sock) == 0);
	pfd = connect_as(sock, PSS_ROLE_PIR);
	mfd = connect_as(sock, PSS_ROLE_MIC);
	cp = spawn((char *[]){ camd, NULL });
	CHECK(cp > 0);
	sleep_ms(1000);	/* let camd connect */
	ensure_armed(data, pfd);
	for (i = 0; i < 100; i++)	/* learn the room background */
		send_fan(mfd, &st, 0.02);

	/* (a) Door: latch sound first, then the person walks into view. */
	send_mic_frame(mfd, 0.5, 5.0);
	sleep_ms(100);
	send_edge(pfd, 1);
	sleep_ms(50);
	send_edge(pfd, 0);
	wait_events(data, n0 + 1, &s);
	CHECK(s.last.pir_edges == 1);
	CHECK(!(s.last.flags & PSS_EVF_AUDIO_OFFLINE));
	if (cam_fail) {
		CHECK(s.last.kind == PSS_EV_ENTRY);
		CHECK(s.last.flags & PSS_EVF_NO_IMAGES);
		CHECK(s.last.n_images == 0);
		goto done;
	}
	check_entry_with_images(data, &s);

	/* (b) Motion first, then the sound 200 ms later. */
	send_edge(pfd, 1);
	sleep_ms(200);
	send_mic_frame(mfd, 0.5, 5.0);
	sleep_ms(50);
	send_edge(pfd, 0);
	wait_events(data, n0 + 2, &s);
	check_entry_with_images(data, &s);

	/* (c) Motion and sound 1 s apart (> T_co): no ENTRY, no images. The PIR
	 * stays high in between so both land in one event. */
	send_edge(pfd, 1);
	sleep_ms(1000);
	send_mic_frame(mfd, 0.5, 5.0);
	sleep_ms(50);
	send_edge(pfd, 0);
	wait_events(data, n0 + 3, &s);
	CHECK(s.last.kind == PSS_EV_MOTION_ONLY);
	CHECK(s.last.n_images == 0);
	CHECK(s.opens == 2);	/* only (a) and (b) were confirmed */
	snprintf(path, sizeof path, "%s/clips/%" PRIu64 ".dir", data, s.last.event_id);
	CHECK(count_entries(path, ".jpg") == -1);
done:
	snprintf(path, sizeof path, "%s/clips", data);
	CHECK(count_entries(path, ".tmp") == 0);
	CHECK(stop(cp) == 0);
	CHECK(stop(hp) == 0);
	close(pfd);
	close(mfd);
	unsetenv("FAKE_RPICAM_FAIL");
}

static void test_entry_to_storage(void)
{
	char root[256], data[300], run[300], fake[512];

	t_mkdtemp(root);
	snprintf(data, sizeof data, "%s/data", root);
	snprintf(run, sizeof run, "%s/run", root);
	snprintf(fake, sizeof fake, "%s/fake-rpicam-still.sh", getenv("PSS_TEST_DIR"));
	CHECK(chmod(fake, 0755) == 0);
	setenv("PSS_DATA_DIR", data, 1);
	setenv("PSS_RUN_DIR", run, 1);
	setenv("PSS_RPICAM", fake, 1);

	entry_session(data, 0);	/* (a) sound->motion, (b) motion->sound, (c) too far apart */
	entry_session(data, 1);	/* (d) camera fails: ENTRY kept, flagged no-images */
	t_rm_rf(root);
}

static pid_t spawn_arm_hub(const char *hub, const char *exit_arg)
{
	pid_t hp = spawn((char *[]){ (char *)hub, "--quiet-ms=300", "--burst-frames=3", "--burst-interval-ms=20",
				     "--warmup-frames=50", "--t-co-ms=500", (char *)exit_arg, "--grace-ms=2000",
				     NULL });

	CHECK(hp > 0);
	return hp;
}

/* SIGKILL, like a power cut: no shutdown path runs. */
static void kill_hard(pid_t pid)
{
	int st;

	CHECK(kill(pid, SIGKILL) == 0);
	CHECK(waitpid(pid, &st, 0) == pid);
}

/* Door latch, then the person walks into view (an ENTRY if armed). */
static void door(int pfd, int mfd)
{
	send_mic_frame(mfd, 0.5, 5.0);
	sleep_ms(100);
	send_edge(pfd, 1);
	sleep_ms(50);
	send_edge(pfd, 0);
}

/*
 * Bryan's arm decisions (2026-11-13; DESIGN.md Section 4):
 *  4a/4b: while not armed nothing is recorded and the camera never fires.
 *  2:     a press during EXIT_DELAY cancels back to DISARMED.
 *  1:     a press while ARMED (no entry) disarms.
 *  GRACE: a press labels the entry `owner`; no press, it is `reported`. An
 *         entry already open when disarmed finishes and keeps its images.
 *  3:     the state survives a hard kill. Killed in GRACE: back ARMED with
 *         the entry reported. Killed in EXIT_DELAY: the delay starts over.
 */
static void test_arm_decisions(void)
{
	char root[256], data[300], run[300], fake[512], path[600], hub[512], camd[512], sock[300];
	const char *bindir = getenv("PSS_BIN_DIR");
	struct seen s;
	uint32_t st = 4242;
	uint64_t ev;
	pid_t hp, cp;
	int pfd, mfd, n, i;

	t_mkdtemp(root);
	snprintf(data, sizeof data, "%s/data", root);
	snprintf(run, sizeof run, "%s/run", root);
	snprintf(fake, sizeof fake, "%s/fake-rpicam-still.sh", getenv("PSS_TEST_DIR"));
	CHECK(chmod(fake, 0755) == 0);
	setenv("PSS_DATA_DIR", data, 1);
	setenv("PSS_RUN_DIR", run, 1);
	setenv("PSS_RPICAM", fake, 1);
	unsetenv("FAKE_RPICAM_FAIL");
	snprintf(hub, sizeof hub, "%s/hub", bindir);
	snprintf(camd, sizeof camd, "%s/camd", bindir);
	snprintf(path, sizeof path, "%s/clips", data);

	hp = spawn_arm_hub(hub, "--exit-delay-ms=300");
	CHECK(ipc_sock_path(sock, sizeof sock) == 0);
	pfd = connect_as(sock, PSS_ROLE_PIR);
	mfd = connect_as(sock, PSS_ROLE_MIC);
	cp = spawn((char *[]){ camd, NULL });
	CHECK(cp > 0);
	sleep_ms(1000);	/* let camd connect */
	for (i = 0; i < 100; i++)	/* learn the room background */
		send_fan(mfd, &st, 0.02);

	/* 4a/4b: a fresh hub is DISARMED; a full entry leaves no trace. */
	door(pfd, mfd);
	sleep_ms(1500);	/* past a 1 s tick and the 300 ms quiet time */
	scan(data, &s);
	CHECK(s.arms == 0 && s.events == 0 && s.opens == 0);
	CHECK(count_entries(path, ".dir") == 0 && count_entries(path, ".tmp") == 0);

	/* 2: press, press again during the exit delay: cancelled. */
	press(pfd);
	wait_arm(data, 1, ARM_EXIT_DELAY, &s);
	press(pfd);
	wait_arm(data, 2, ARM_DISARMED, &s);
	CHECK(s.last_arm.cause == ARM_CAUSE_BUTTON);
	sleep_ms(1500);	/* the old deadline must not arm it */
	scan(data, &s);
	CHECK(s.arms == 2 && s.last_arm.state == ARM_DISARMED);

	/* 1: armed, nobody came in, press: disarmed. */
	press(pfd);
	wait_arm(data, 4, ARM_ARMED, &s);
	CHECK(s.arm[2].state == ARM_EXIT_DELAY && s.last_arm.cause == ARM_CAUSE_EXIT_DELAY_OVER);
	press(pfd);
	wait_arm(data, 5, ARM_DISARMED, &s);
	CHECK(s.last_arm.cause == ARM_CAUSE_BUTTON && s.last_arm.label == ARM_LABEL_NONE);

	/* GRACE, then a press: the entry is Macy's (owner). It still finishes
	 * normally, with the images taken while armed. */
	ensure_armed(data, pfd);
	door(pfd, mfd);
	wait_arm(data, 8, ARM_GRACE, &s);
	ev = s.last_arm.event_id;
	CHECK(ev != 0 && s.last_arm.cause == ARM_CAUSE_ENTRY);
	press(pfd);
	wait_arm(data, 9, ARM_DISARMED, &s);
	CHECK(s.last_arm.label == ARM_LABEL_OWNER && s.last_arm.event_id == ev);
	wait_events(data, 1, &s);
	CHECK(s.last.event_id == ev);
	check_entry_with_images(data, &s);

	/* GRACE runs out (2 s): reported. */
	ensure_armed(data, pfd);
	door(pfd, mfd);
	wait_arm(data, 12, ARM_GRACE, &s);
	ev = s.last_arm.event_id;
	wait_arm(data, 13, ARM_ARMED, &s);
	CHECK(s.last_arm.cause == ARM_CAUSE_GRACE_OVER);
	CHECK(s.last_arm.label == ARM_LABEL_REPORTED && s.last_arm.event_id == ev);
	wait_events(data, 2, &s);
	check_entry_with_images(data, &s);
	CHECK(stop(cp) == 0);	/* no camera for the restarts below */

	/* 3: pull the plug during GRACE. */
	door(pfd, mfd);
	wait_arm(data, 14, ARM_GRACE, &s);
	ev = s.last_arm.event_id;
	kill_hard(hp);
	close(pfd);
	close(mfd);
	hp = spawn_arm_hub(hub, "--exit-delay-ms=3000");
	pfd = connect_as(sock, PSS_ROLE_PIR);
	wait_arm(data, 15, ARM_ARMED, &s);
	CHECK(s.last_arm.cause == ARM_CAUSE_RESTORE);
	CHECK(s.last_arm.label == ARM_LABEL_REPORTED && s.last_arm.event_id == ev);
	CHECK(s.last.event_id == ev && (s.last.flags & PSS_EVF_INTERRUPTED));	/* recovery closed it */

	/* 3: pull the plug during EXIT_DELAY (3 s here): it starts over. */
	press(pfd);
	wait_arm(data, 16, ARM_DISARMED, &s);
	press(pfd);
	wait_arm(data, 17, ARM_EXIT_DELAY, &s);
	kill_hard(hp);
	close(pfd);
	hp = spawn_arm_hub(hub, "--exit-delay-ms=3000");
	wait_arm(data, 18, ARM_EXIT_DELAY, &s);
	CHECK(s.last_arm.cause == ARM_CAUSE_RESTORE);
	n = s.arms;
	wait_arm(data, n + 1, ARM_ARMED, &s);
	CHECK(s.last_arm.cause == ARM_CAUSE_EXIT_DELAY_OVER);
	CHECK(stop(hp) == 0);

	/* A clean restart keeps ARMED without writing anything new. */
	hp = spawn_arm_hub(hub, "--exit-delay-ms=3000");
	pfd = connect_as(sock, PSS_ROLE_PIR);	/* connected = the hub finished its restore */
	sleep_ms(200);
	scan(data, &s);
	CHECK(s.arms == n + 1 && s.last_arm.state == ARM_ARMED);
	CHECK(stop(hp) == 0);
	close(pfd);
	t_rm_rf(root);
}

int main(void)
{
	if (getenv("PSS_BIN_DIR") == NULL || getenv("PSS_TEST_DIR") == NULL) {
		fprintf(stderr, "run via `make test` (needs PSS_BIN_DIR and PSS_TEST_DIR)\n");
		return 2;
	}
	T_RUN(test_pir_to_storage);
	T_RUN(test_sound_to_storage);
	T_RUN(test_entry_to_storage);
	T_RUN(test_arm_decisions);
	T_END();
}
