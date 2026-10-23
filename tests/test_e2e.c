/*
 * End-to-end on the lab machine: real hub + real camd (with the
 * fake-rpicam-still.sh stand-in for the camera), and this test playing the
 * part of pird by sending PIR edge messages over the hub socket. The live
 * demo uses the real pird and rpicam-still on the Pi; this checks the
 * pipeline and storage around them.
 */
#include "t.h"

#include "common/ipc.h"
#include "common/time.h"
#include "store/evlog.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct seen {
	int boots, events;
	uint64_t max_boot;
	struct pss_rec_event last;
};

static int seen_visitor(uint32_t seg, uint16_t type, uint16_t ver, const void *p, uint32_t len, void *arg)
{
	struct seen *s = arg;

	(void)seg;
	(void)ver;
	if (type == PSS_REC_EVENT && len == sizeof s->last) {
		memcpy(&s->last, p, sizeof s->last);
		s->events++;
	} else if (type == PSS_REC_BOOT && len == sizeof(struct pss_rec_boot)) {
		struct pss_rec_boot b;

		memcpy(&b, p, sizeof b);
		if (b.boot_no > s->max_boot)
			s->max_boot = b.boot_no;
		s->boots++;
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
		CHECK(store_scan(data, seen_visitor, &before) == 0);

	hp = spawn((char *[]){ hub, "--quiet-ms=300", "--burst-frames=3", "--burst-interval-ms=20", NULL });
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
	send_edge(fd, 1);
	sleep_ms(50);
	send_edge(fd, 0);

	/* Event closes after the burst returns and 300 ms of quiet (1 s tick). */
	for (int i = 0; i < 100; i++) {
		memset(out, 0, sizeof *out);
		CHECK(store_scan(data, seen_visitor, out) == 0);
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

	/* Session 1: camera works. */
	unsetenv("FAKE_RPICAM_FAIL");
	run_session(data, &s);
	first_id = s.last.event_id;
	CHECK(s.boots == 1 && s.max_boot == 1);
	CHECK(s.last.kind == PSS_EV_MOTION_ONLY);
	CHECK(s.last.n_images == 3);
	CHECK(!(s.last.flags & PSS_EVF_NO_IMAGES));
	CHECK(s.last.flags & PSS_EVF_AUDIO_OFFLINE); /* no micd running */
	CHECK(s.last.pir_edges == 1);
	CHECK(s.last.end_mono_ns >= s.last.start_mono_ns);
	snprintf(path, sizeof path, "%s/clips/%" PRIu64 ".dir", data, first_id);
	CHECK(count_entries(path, ".jpg") == 3);

	/* Session 2: same data dir, camera fails. Ids continue, boot count grows. */
	setenv("FAKE_RPICAM_FAIL", "1", 1);
	run_session(data, &s);
	CHECK(s.boots == 2 && s.max_boot == 2);
	CHECK(s.last.event_id == first_id + 1);
	CHECK(s.last.flags & PSS_EVF_NO_IMAGES);
	CHECK(s.last.n_images == 0);

	snprintf(path, sizeof path, "%s/clips", data);
	CHECK(count_entries(path, ".tmp") == 0);
	unsetenv("FAKE_RPICAM_FAIL");
	t_rm_rf(root);
}

int main(void)
{
	if (getenv("PSS_BIN_DIR") == NULL || getenv("PSS_TEST_DIR") == NULL) {
		fprintf(stderr, "run via `make test` (needs PSS_BIN_DIR and PSS_TEST_DIR)\n");
		return 2;
	}
	T_RUN(test_pir_to_storage);
	T_END();
}
