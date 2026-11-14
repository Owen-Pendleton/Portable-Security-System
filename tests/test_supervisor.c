/*
 * The real supervisor (mechanism E) on the lab machine, with short timings.
 * Its children are tests/fake_child (told how to misbehave) and, in the last
 * test, the real hub and camd. Each run's stderr (supervisor and children)
 * goes to a log file that the checks read back.
 */
#include "t.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXK 8

struct env {
	char root[256], run[300], data[300], conf[300], log[300], pids[320];
	char fake[512], sup[512];
};

struct kid {
	int pid;
	char name[32];
};

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

static void env_init(struct env *e)
{
	const char *bin = getenv("PSS_BIN_DIR");

	t_mkdtemp(e->root);
	snprintf(e->run, sizeof e->run, "%s/run", e->root);
	snprintf(e->data, sizeof e->data, "%s/data", e->root);
	snprintf(e->conf, sizeof e->conf, "%s/pss.conf", e->root);
	snprintf(e->log, sizeof e->log, "%s/sup.log", e->root);
	snprintf(e->pids, sizeof e->pids, "%s/supervisor.pids", e->run);
	snprintf(e->fake, sizeof e->fake, "%s/tests/fake_child", bin);
	snprintf(e->sup, sizeof e->sup, "%s/supervisor", bin);
	setenv("PSS_RUN_DIR", e->run, 1);
	setenv("PSS_DATA_DIR", e->data, 1);
}

static void write_conf(const struct env *e, const char *text)
{
	FILE *f = fopen(e->conf, "w");

	CHECK(f != NULL);
	if (f == NULL)
		return;
	fputs(text, f);
	fclose(f);
}

/* Starts the supervisor with stderr appended to e->log. */
static pid_t start_sup(const struct env *e, char *const extra[])
{
	char cfg[400];
	char *argv[16];
	int n = 0;
	pid_t pid;

	snprintf(cfg, sizeof cfg, "--config=%s", e->conf);
	argv[n++] = (char *)e->sup;
	argv[n++] = cfg;
	for (int i = 0; extra[i] != NULL && n < 15; i++)
		argv[n++] = extra[i];
	argv[n] = NULL;
	pid = fork();
	if (pid == 0) {
		int fd = open(e->log, O_WRONLY | O_CREAT | O_APPEND, 0644);

		if (fd < 0 || dup2(fd, 2) < 0)
			_exit(126);
		execv(argv[0], argv);
		_exit(127);
	}
	CHECK(pid > 0);
	return pid;
}

/* SIGTERM, wait up to 10 s; the exit status, or -1. */
static int stop_sup(pid_t pid)
{
	int st;

	kill(pid, SIGTERM);
	for (int i = 0; i < 1000; i++) {
		if (waitpid(pid, &st, WNOHANG) == pid)
			return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
		sleep_ms(10);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	return -1;
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "r");
	char *buf;
	long n;

	if (f == NULL)
		return strdup("");
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n + 1);
	if (buf == NULL) {
		fclose(f);
		return strdup("");
	}
	n = (long)fread(buf, 1, (size_t)n, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

static int count(const char *hay, const char *needle)
{
	int n = 0;

	for (const char *p = hay; (p = strstr(p, needle)) != NULL; p++)
		n++;
	return n;
}

static int log_has(const struct env *e, const char *needle)
{
	char *s = slurp(e->log);
	int n = count(s, needle);

	free(s);
	return n;
}

static int read_kids(const struct env *e, struct kid k[MAXK])
{
	FILE *f = fopen(e->pids, "r");
	int n = 0;

	if (f == NULL)
		return 0;
	while (n < MAXK && fscanf(f, "%d %31s", &k[n].pid, k[n].name) == 2)
		n++;
	fclose(f);
	return n;
}

static int pid_of(const struct env *e, const char *name)
{
	struct kid k[MAXK];
	int n = read_kids(e, k);

	for (int i = 0; i < n; i++)
		if (strcmp(k[i].name, name) == 0)
			return k[i].pid;
	return 0;
}

/* Waits (up to 5 s) until `name` runs with a pid other than `not`. */
static int wait_pid(const struct env *e, const char *name, int not)
{
	for (int i = 0; i < 500; i++) {
		int p = pid_of(e, name);

		if (p > 0 && p != not)
			return p;
		sleep_ms(10);
	}
	CHECK(!"wait_pid timed out");
	return 0;
}

/* Like wait_pid, for a fake_child, and also waits (up to 10 s, ASAN is
 * slow) for its ready file: the pid file lists a child at fork, before it
 * has exec'd and installed its SIGTERM handling. */
static int wait_ready(const struct env *e, const char *name, int not)
{
	char path[400];
	int p = wait_pid(e, name, not);

	snprintf(path, sizeof path, "%s/ready.%d", e->run, p);
	for (int i = 0; i < 1000; i++) {
		if (access(path, F_OK) == 0)
			return p;
		sleep_ms(10);
	}
	CHECK(!"wait_ready timed out");
	return p;
}

static int nfds(int pid)
{
	char path[64];
	DIR *d;
	struct dirent *de;
	int n = 0;

	snprintf(path, sizeof path, "/proc/%d/fd", pid);
	d = opendir(path);
	if (d == NULL)
		return -1;
	while ((de = readdir(d)) != NULL)
		if (de->d_name[0] != '.')
			n++;
	closedir(d);
	return n;
}

static int gone_within(int pid, long ms)
{
	for (long i = 0; i < ms / 10; i++) {
		if (kill(pid, 0) != 0 && errno == ESRCH)
			return 1;
		sleep_ms(10);
	}
	return 0;
}

/* Value after `key` in the last soak line, for child `name`, or -1. */
static long soak_value(const struct env *e, const char *name, const char *key)
{
	char *s = slurp(e->log), pat[64], *p, *line = NULL;
	long v = -1;

	for (p = s; (p = strstr(p, "soak: up=")) != NULL; p++)
		line = p;
	snprintf(pat, sizeof pat, " %s=pid:", name);
	if (line != NULL && (p = strstr(line, pat)) != NULL && (p = strstr(p, key)) != NULL &&
	    (strchr(line, '\n') == NULL || p < strchr(line, '\n')))
		v = strtol(p + strlen(key), NULL, 10);
	free(s);
	return v;
}

static void test_restart_rules(void)
{
	struct env e;
	char conf[4096];
	struct kid before[MAXK];
	pid_t sp;
	int steady, steady2, fds1, nk;

	env_init(&e);
	snprintf(conf, sizeof conf,
		 "# test children\n"
		 "steady  %s ok\n"
		 "crasher %s crash 50\n"
		 "gone    %s gone\n"
		 "hanger  %s hang 300   # beats, then stops\n"
		 "silent  %s silent\n",
		 e.fake, e.fake, e.fake, e.fake, e.fake);
	write_conf(&e, conf);
	sp = start_sup(&e, (char *[]){ "--hb-timeout-ms=600", "--first-hb-ms=1000", "--backoff-min-ms=100",
				       "--backoff-max-ms=400", "--gone-retry-ms=700", "--soak-every-s=1",
				       "--stop-timeout-ms=1000", NULL });
	steady = wait_ready(&e, "steady", 0);
	sleep_ms(3000);

	/* Crash: backoff 100, 200, 400, then capped at 400 ms. */
	CHECK(log_has(&e, "crasher exited 1, restart #1 in 100ms") == 1);
	CHECK(log_has(&e, "crasher exited 1, restart #2 in 200ms") == 1);
	CHECK(log_has(&e, "crasher exited 1, restart #3 in 400ms") == 1);
	CHECK(log_has(&e, "crasher exited 1, restart #4 in 400ms") == 1);
	/* Device gone: the slow retry, never the fast backoff. */
	CHECK(log_has(&e, "gone exited 3 (device gone), retry in 700ms") >= 3);
	CHECK(log_has(&e, "gone exited 3 (device gone), retry in 700ms") <= 5);
	CHECK(log_has(&e, "gone exited 3, restart") == 0);
	/* Hung: stopped beating, or never beat. */
	CHECK(log_has(&e, "hanger heartbeat missed") >= 1);
	CHECK(log_has(&e, "hanger killed by signal 9 (hung), restart #1") == 1);
	CHECK(log_has(&e, "silent no heartbeat") >= 1);
	CHECK(log_has(&e, "silent killed by signal 9 (hung), restart #1") == 1);
	/* The healthy one was left alone, and the soak line reports it. */
	CHECK(log_has(&e, "steady exited") == 0 && log_has(&e, "steady killed") == 0);
	CHECK(log_has(&e, "steady heartbeat missed") == 0);
	CHECK(pid_of(&e, "steady") == steady);
	CHECK(soak_value(&e, "steady", ",hb:") > 10);
	CHECK(soak_value(&e, "steady", ",fds:") == nfds(steady));

	/* kill -9: restarted, with no fd leaked into the new process. */
	fds1 = nfds(steady);
	CHECK(fds1 == 4);	/* stdin, stdout, stderr, the supervisor socket */
	CHECK(kill(steady, SIGKILL) == 0);
	steady2 = wait_ready(&e, "steady", steady);
	CHECK(nfds(steady2) == fds1);
	CHECK(log_has(&e, "steady killed by signal 9, restart #1 in 100ms") == 1);

	/* Clean stop: exit 0, every child gone, pid file removed. */
	nk = read_kids(&e, before);
	CHECK(nk >= 1);
	CHECK(stop_sup(sp) == 0);
	for (int i = 0; i < nk; i++)
		CHECK(gone_within(before[i].pid, 1000));
	CHECK(access(e.pids, F_OK) != 0);
	CHECK(log_has(&e, "steady stopped (exit 0)") == 1);
	t_rm_rf(e.root);
}

static void test_no_orphans(void)
{
	struct env e;
	char conf[4096];
	char *opts[] = { "--stop-timeout-ms=500", NULL };
	pid_t sp;
	int steady, stubborn, stubborn2;

	env_init(&e);
	snprintf(conf, sizeof conf, "steady %s ok\nstubborn %s stubborn\n", e.fake, e.fake);
	write_conf(&e, conf);

	/* Supervisor SIGKILLed: PR_SET_PDEATHSIG stops its children. */
	sp = start_sup(&e, opts);
	steady = wait_ready(&e, "steady", 0);
	stubborn = wait_ready(&e, "stubborn", 0);
	CHECK(kill(sp, SIGKILL) == 0);
	waitpid(sp, NULL, 0);
	CHECK(gone_within(steady, 2000));
	/* One that ignores SIGTERM survives; the next supervisor kills it. */
	CHECK(kill(stubborn, 0) == 0);
	sp = start_sup(&e, opts);
	stubborn2 = wait_ready(&e, "stubborn", stubborn);
	CHECK(gone_within(stubborn, 1000));
	CHECK(log_has(&e, "killed stale stubborn pid") == 1);
	CHECK(log_has(&e, "cleaned 1 stale children") == 1);

	/* A child that ignores SIGTERM is SIGKILLed after --stop-timeout-ms. */
	CHECK(stop_sup(sp) == 0);
	CHECK(log_has(&e, "stubborn did not stop in time, SIGKILL") == 1);
	CHECK(gone_within(stubborn2, 1000));
	t_rm_rf(e.root);
}

static void test_real_hub_and_camd(void)
{
	struct env e;
	char fake[512];
	pid_t sp;

	env_init(&e);
	snprintf(fake, sizeof fake, "%s/fake-rpicam-still.sh", getenv("PSS_TEST_DIR"));
	CHECK(chmod(fake, 0755) == 0);
	setenv("PSS_RPICAM", fake, 1);
	/* Program names without '/' are found next to the supervisor binary. */
	write_conf(&e, "hub  hub --quiet-ms=300\ncamd camd\n");
	sp = start_sup(&e, (char *[]){ "--first-hb-ms=3000", "--soak-every-s=2", NULL });
	wait_pid(&e, "hub", 0);
	wait_pid(&e, "camd", 0);
	sleep_ms(3500);
	CHECK(log_has(&e, "started hub pid") == 1);
	CHECK(log_has(&e, "started camd pid") == 1);
	CHECK(log_has(&e, "camd: connected to hub") == 1);
	CHECK(log_has(&e, "SIGKILL") == 0);
	CHECK(log_has(&e, "restart #") == 0);
	CHECK(soak_value(&e, "hub", ",hb:") >= 1);	/* hub.c's own heartbeat */
	CHECK(soak_value(&e, "camd", ",hb:") >= 1);	/* via client.c */
	CHECK(stop_sup(sp) == 0);
	CHECK(log_has(&e, "hub stopped (exit 0)") == 1);
	CHECK(log_has(&e, "camd stopped (exit 0)") == 1);
	t_rm_rf(e.root);
}

int main(void)
{
	if (getenv("PSS_BIN_DIR") == NULL || getenv("PSS_TEST_DIR") == NULL) {
		fprintf(stderr, "run via `make test` (needs PSS_BIN_DIR and PSS_TEST_DIR)\n");
		return 2;
	}
	T_RUN(test_restart_rules);
	T_RUN(test_no_orphans);
	T_RUN(test_real_hub_and_camd);
	T_END();
}
