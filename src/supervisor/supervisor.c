/*
 * supervisor (DESIGN.md Section 3, mechanism E): started by systemd at
 * boot (deploy/pss.service), it fork/execs every daemon listed in its
 * config file and keeps them running (rules in policy.c):
 *   - each child gets one end of an AF_UNIX socketpair in $PSS_SUP_FD and
 *     heartbeats on it every 5 s (src/common/suphb.c);
 *   - a child that exits is restarted with backoff; exit 3 ("device gone")
 *     is retried every 30 s; a child that stops heartbeating is SIGKILLed
 *     and restarted;
 *   - children get PR_SET_PDEATHSIG, so they die with the supervisor, and
 *     their pids are kept in $PSS_RUN_DIR/supervisor.pids so a restarted
 *     supervisor can clean up any that survived;
 *   - once an hour (--soak-every-s) it logs each child's pid, RSS, open
 *     fds, restarts and heartbeats: the 48 h soak evidence.
 *
 * Config: one child per line, "name program [args...]", '#' comments. A
 * program without a '/' is looked up next to the supervisor binary. The
 * order of lines is the start order (hub first).
 *
 * Exit codes: 0 on SIGTERM/SIGINT (all children stopped), 1 on error,
 * 2 on usage or a bad config.
 */
#include "common/ipc.h"
#include "common/log.h"
#include "common/time.h"
#include "common/util.h"
#include "supervisor/policy.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXIT_USAGE 2
#define MAX_CHILDREN 8
#define MAX_ARGS 16
#define LINE_MAX_LEN 512
#define TICK_NS (250 * PSS_NS_PER_MS)

enum { TAG_SIG = MAX_CHILDREN, TAG_TICK };

struct child {
	char name[32];
	char path[PATH_MAX];
	char line[LINE_MAX_LEN];	/* argv points into this */
	char *argv[MAX_ARGS + 1];
	pid_t pid;		/* 0 while not running */
	int fd;			/* our end of the socketpair, -1 if none */
	int killed;		/* SIGKILLed as hung; cleared when reaped */
	int64_t next_start_ns;
	uint32_t restarts;
	uint64_t beats;
	struct sup_track t;
};

struct sup {
	struct sup_policy pol;
	struct child kids[MAX_CHILDREN];
	int n;
	int epoll_fd, sig_fd, tick_fd;
	int stopping;
	int64_t stop_deadline_ns, stop_timeout_ns;
	int64_t start_ns, next_soak_ns, soak_every_ns;
	char exe_dir[PATH_MAX];
	char pids_path[PATH_MAX];
};

static struct sup sup;

static void usage(void)
{
	fprintf(stderr, "usage: supervisor --config=FILE [--hb-timeout-ms=15000] [--first-hb-ms=60000]\n"
			"           [--backoff-min-ms=1000] [--backoff-max-ms=60000] [--healthy-ms=600000]\n"
			"           [--gone-retry-ms=30000] [--soak-every-s=3600] [--stop-timeout-ms=10000]\n");
}

/* --- config ------------------------------------------------------------ */

static int parse_config_line(struct sup *s, char *line, const char *file, int lineno)
{
	struct child *c;
	char *save = NULL, *tok;
	int argc = 0;

	line[strcspn(line, "#\n")] = '\0';
	if (strspn(line, " \t") == strlen(line))
		return 0;
	if (s->n == MAX_CHILDREN) {
		pss_log("%s:%d: more than %d children", file, lineno, MAX_CHILDREN);
		return -1;
	}
	c = &s->kids[s->n];
	memset(c, 0, sizeof *c);
	c->fd = -1;
	if (strlen(line) >= sizeof c->line) {
		pss_log("%s:%d: line too long", file, lineno);
		return -1;
	}
	memcpy(c->line, line, strlen(line) + 1);
	tok = strtok_r(c->line, " \t", &save);
	if (tok == NULL || strlen(tok) >= sizeof c->name) {
		pss_log("%s:%d: bad name", file, lineno);
		return -1;
	}
	memcpy(c->name, tok, strlen(tok) + 1);
	while ((tok = strtok_r(NULL, " \t", &save)) != NULL) {
		if (argc == MAX_ARGS) {
			pss_log("%s:%d: more than %d arguments", file, lineno, MAX_ARGS);
			return -1;
		}
		c->argv[argc++] = tok;
	}
	if (argc == 0) {
		pss_log("%s:%d: %s has no program", file, lineno, c->name);
		return -1;
	}
	c->argv[argc] = NULL;
	if (strchr(c->argv[0], '/') != NULL)
		snprintf(c->path, sizeof c->path, "%s", c->argv[0]);
	else if (snprintf(c->path, sizeof c->path, "%s/%s", s->exe_dir, c->argv[0]) >= (int)sizeof c->path) {
		pss_log("%s:%d: path too long", file, lineno);
		return -1;
	}
	for (int i = 0; i < s->n; i++) {
		if (strcmp(s->kids[i].name, c->name) == 0) {
			pss_log("%s:%d: duplicate name %s", file, lineno, c->name);
			return -1;
		}
	}
	sup_track_init(&s->pol, &c->t);
	s->n++;
	return 0;
}

static int load_config(struct sup *s, const char *file)
{
	char line[LINE_MAX_LEN + 2];
	FILE *f = fopen(file, "re");
	int lineno = 0, ret = 0;

	if (f == NULL) {
		pss_log("open %s: %s", file, strerror(errno));
		return -1;
	}
	while (ret == 0 && fgets(line, sizeof line, f) != NULL) {
		lineno++;
		if (strchr(line, '\n') == NULL && !feof(f)) {
			pss_log("%s:%d: line too long", file, lineno);
			ret = -1;
			break;
		}
		ret = parse_config_line(s, line, file, lineno);
	}
	if (ret == 0 && ferror(f)) {
		pss_log("read %s: %s", file, strerror(errno));
		ret = -1;
	}
	if (fclose(f) != 0)
		pss_log("close %s: %s", file, strerror(errno));
	if (ret == 0 && s->n == 0) {
		pss_log("%s: no children configured", file);
		ret = -1;
	}
	return ret;
}

static int find_exe_dir(struct sup *s)
{
	ssize_t n = readlink("/proc/self/exe", s->exe_dir, sizeof s->exe_dir - 1);
	char *slash;

	if (n < 0) {
		pss_log("readlink /proc/self/exe: %s", strerror(errno));
		return -1;
	}
	s->exe_dir[n] = '\0';
	slash = strrchr(s->exe_dir, '/');
	if (slash != NULL)
		*slash = '\0';
	return 0;
}

/* --- pid file and stale children --------------------------------------- */

/* $PSS_RUN_DIR/supervisor.pids: "pid name" per running child. The run dir
 * is tmpfs, so this needs no fsync; it only has to outlive this process. */
static void write_pids(struct sup *s)
{
	char tmp[PATH_MAX + 8];
	FILE *f;
	int bad = 0;

	snprintf(tmp, sizeof tmp, "%s.tmp", s->pids_path);
	f = fopen(tmp, "we");
	if (f == NULL) {
		pss_log("open %s: %s", tmp, strerror(errno));
		return;
	}
	for (int i = 0; i < s->n; i++)
		if (s->kids[i].pid > 0 && fprintf(f, "%d %s\n", (int)s->kids[i].pid, s->kids[i].name) < 0)
			bad = 1;
	if (fclose(f) != 0 || bad) {
		pss_log("write %s: %s", tmp, strerror(errno));
		return;
	}
	if (rename(tmp, s->pids_path) != 0)
		pss_log("rename %s: %s", tmp, strerror(errno));
}

/* 1 if pid is running one of our configured programs. */
static int is_our_program(struct sup *s, pid_t pid)
{
	char link[64], exe[PATH_MAX], want[PATH_MAX];
	ssize_t n;

	snprintf(link, sizeof link, "/proc/%d/exe", (int)pid);
	n = readlink(link, exe, sizeof exe - 1);
	if (n < 0)
		return 0;
	exe[n] = '\0';
	for (int i = 0; i < s->n; i++)
		if (realpath(s->kids[i].path, want) != NULL && strcmp(want, exe) == 0)
			return 1;
	return 0;
}

/* Children of a previous supervisor that survived it (it was SIGKILLed and
 * PR_SET_PDEATHSIG's SIGTERM didn't finish them): kill them before starting
 * new ones, or a stale hub would hold the socket. Returns how many. */
static int clean_stale(struct sup *s)
{
	FILE *f = fopen(s->pids_path, "re");
	pid_t stale[MAX_CHILDREN];
	int pid, n = 0;
	char name[64];

	if (f == NULL) {
		if (errno != ENOENT)
			pss_log("open %s: %s", s->pids_path, strerror(errno));
		return 0;
	}
	while (n < MAX_CHILDREN && fscanf(f, "%d %63s", &pid, name) == 2) {
		if (pid <= 1 || !is_our_program(s, (pid_t)pid))
			continue;
		if (kill((pid_t)pid, SIGKILL) != 0) {
			if (errno != ESRCH)
				pss_log("kill stale %s pid %d: %s", name, pid, strerror(errno));
			continue;
		}
		pss_log("killed stale %s pid %d", name, pid);
		stale[n++] = (pid_t)pid;
	}
	if (fclose(f) != 0)
		pss_log("close %s: %s", s->pids_path, strerror(errno));
	/* Not our children, so no waitpid: wait for them to vanish (<= 2 s). */
	for (int tries = 0; tries < 200; tries++) {
		int alive = 0;

		for (int i = 0; i < n; i++)
			if (kill(stale[i], 0) == 0)
				alive++;
		if (alive == 0)
			break;
		usleep(10000);
	}
	return n;
}

/* --- children ---------------------------------------------------------- */

static void drop_fd(struct sup *s, struct child *c)
{
	if (c->fd < 0)
		return;
	if (epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL) != 0)
		pss_log("epoll_ctl DEL: %s", strerror(errno));
	if (close(c->fd) != 0)
		pss_log("close %s fd: %s", c->name, strerror(errno));
	c->fd = -1;
}

/* In the forked child: never returns. */
static void exec_child(struct child *c, int fd, pid_t parent)
{
	char fdbuf[16];
	sigset_t none;

	if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent)
		_exit(1);	/* the supervisor already died */
	sigemptyset(&none);
	if (sigprocmask(SIG_SETMASK, &none, NULL) != 0)	/* the signalfd mask is inherited */
		_exit(1);
	if (fcntl(fd, F_SETFD, 0) != 0)			/* keep only this end across exec */
		_exit(1);
	snprintf(fdbuf, sizeof fdbuf, "%d", fd);
	if (setenv("PSS_SUP_FD", fdbuf, 1) != 0)
		_exit(1);
	execv(c->path, c->argv);
	pss_log("exec %s: %s", c->path, strerror(errno));
	_exit(127);
}

static void start_child(struct sup *s, int idx, int64_t now)
{
	struct child *c = &s->kids[idx];
	struct epoll_event ee = { .events = EPOLLIN, .data.u32 = (uint32_t)idx };
	pid_t parent = getpid(), pid;
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sv) != 0) {
		pss_log("socketpair for %s: %s; retry in 1s", c->name, strerror(errno));
		c->next_start_ns = now + PSS_NS_PER_S;
		return;
	}
	pid = fork();
	if (pid < 0) {
		pss_log("fork %s: %s; retry in 1s", c->name, strerror(errno));
		close(sv[0]);
		close(sv[1]);
		c->next_start_ns = now + PSS_NS_PER_S;
		return;
	}
	if (pid == 0) {
		close(sv[0]);
		exec_child(c, sv[1], parent);
	}
	if (close(sv[1]) != 0)
		pss_log("close %s child end: %s", c->name, strerror(errno));
	c->pid = pid;
	c->fd = sv[0];
	c->killed = 0;
	sup_track_started(&c->t, now);
	if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, c->fd, &ee) != 0) {
		/* Without the fd we can't hear heartbeats; it will be killed as hung. */
		pss_log("epoll_ctl ADD %s: %s", c->name, strerror(errno));
		close(c->fd);
		c->fd = -1;
	}
	pss_log("started %s pid %d%s", c->name, (int)pid, c->restarts ? "" : " (first start)");
	write_pids(s);
}

static void on_child_fd(struct sup *s, int idx, int64_t now)
{
	struct child *c = &s->kids[idx];
	struct pss_msg m;
	int r;

	while (c->fd >= 0) {
		r = ipc_recv(c->fd, &m);
		if (r == 1) {
			if (m.type == PSS_MSG_HEARTBEAT) {
				sup_track_beat(&c->t, now);
				c->beats++;
			}
			continue;
		}
		if (r == 0 || errno != EAGAIN)
			drop_fd(s, c);	/* the child is exiting; SIGCHLD does the rest */
		return;
	}
}

static struct child *child_by_pid(struct sup *s, pid_t pid)
{
	for (int i = 0; i < s->n; i++)
		if (s->kids[i].pid == pid)
			return &s->kids[i];
	return NULL;
}

static void reap(struct sup *s, int64_t now)
{
	for (;;) {
		struct child *c;
		int st, code;
		int64_t delay;
		pid_t pid = waitpid(-1, &st, WNOHANG);

		if (pid <= 0) {
			if (pid < 0 && errno != ECHILD)
				pss_log("waitpid: %s", strerror(errno));
			return;
		}
		c = child_by_pid(s, pid);
		if (c == NULL)
			continue;
		drop_fd(s, c);
		c->pid = 0;
		code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
		if (s->stopping) {
			pss_log("%s stopped (%s %d)", c->name, code >= 0 ? "exit" : "signal",
				code >= 0 ? code : WTERMSIG(st));
			write_pids(s);
			continue;
		}
		delay = sup_restart_delay(&s->pol, &c->t, now, code);
		c->restarts++;
		if (code == SUP_EXIT_DEVICE_GONE)
			pss_log("%s exited 3 (device gone), retry in %" PRId64 "ms", c->name, delay / PSS_NS_PER_MS);
		else if (code >= 0)
			pss_log("%s exited %d, restart #%" PRIu32 " in %" PRId64 "ms", c->name, code, c->restarts,
				delay / PSS_NS_PER_MS);
		else
			pss_log("%s killed by signal %d%s, restart #%" PRIu32 " in %" PRId64 "ms", c->name,
				WTERMSIG(st), c->killed ? " (hung)" : "", c->restarts, delay / PSS_NS_PER_MS);
		c->next_start_ns = now + delay;
		write_pids(s);
	}
}

/* --- soak line --------------------------------------------------------- */

static long rss_kb(pid_t pid)
{
	char path[64], line[128];
	FILE *f;
	long kb = -1;

	snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
	f = fopen(path, "re");
	if (f == NULL)
		return -1;
	while (fgets(line, sizeof line, f) != NULL)
		if (sscanf(line, "VmRSS: %ld kB", &kb) == 1)
			break;
	fclose(f);
	return kb;
}

static int count_fds(pid_t pid)
{
	char path[64];
	DIR *d;
	struct dirent *e;
	int n = 0;

	snprintf(path, sizeof path, "/proc/%d/fd", (int)pid);
	d = opendir(path);
	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL)
		if (e->d_name[0] != '.')
			n++;
	closedir(d);
	return n;
}

static void soak_line(struct sup *s, int64_t now)
{
	char buf[1024];
	size_t off = 0;

	off += (size_t)snprintf(buf, sizeof buf, "soak: up=%" PRId64 "s self_rss=%ldkB self_fds=%d",
				(now - s->start_ns) / PSS_NS_PER_S, rss_kb(getpid()), count_fds(getpid()));
	for (int i = 0; i < s->n && off < sizeof buf; i++) {
		struct child *c = &s->kids[i];

		if (c->pid > 0)
			off += (size_t)snprintf(buf + off, sizeof buf - off,
						" %s=pid:%d,rss:%ldkB,fds:%d,restarts:%" PRIu32 ",hb:%" PRIu64,
						c->name, (int)c->pid, rss_kb(c->pid), count_fds(c->pid), c->restarts,
						c->beats);
		else
			off += (size_t)snprintf(buf + off, sizeof buf - off, " %s=DOWN,restarts:%" PRIu32,
						c->name, c->restarts);
	}
	pss_log("%s", buf);
}

/* --- main loop --------------------------------------------------------- */

static void tick(struct sup *s, int64_t now)
{
	for (int i = 0; i < s->n; i++) {
		struct child *c = &s->kids[i];

		if (s->stopping) {
			if (c->pid > 0 && now >= s->stop_deadline_ns && !c->killed) {
				pss_log("%s did not stop in time, SIGKILL", c->name);
				if (kill(c->pid, SIGKILL) != 0 && errno != ESRCH)
					pss_log("kill %s: %s", c->name, strerror(errno));
				c->killed = 1;
			}
			continue;
		}
		if (c->pid == 0 && now >= c->next_start_ns) {
			start_child(s, i, now);
		} else if (c->pid > 0 && !c->killed && sup_hung(&s->pol, &c->t, now)) {
			if (c->t.have_hb)
				pss_log("%s heartbeat missed %" PRId64 "ms, SIGKILL", c->name,
					(now - c->t.last_hb_ns) / PSS_NS_PER_MS);
			else
				pss_log("%s no heartbeat %" PRId64 "ms after start, SIGKILL", c->name,
					(now - c->t.started_ns) / PSS_NS_PER_MS);
			if (kill(c->pid, SIGKILL) != 0 && errno != ESRCH)
				pss_log("kill %s: %s", c->name, strerror(errno));
			c->killed = 1;
		}
	}
	if (!s->stopping && now >= s->next_soak_ns) {
		soak_line(s, now);
		s->next_soak_ns = now + s->soak_every_ns;
	}
}

static void begin_stop(struct sup *s, int64_t now)
{
	s->stopping = 1;
	s->stop_deadline_ns = now + s->stop_timeout_ns;
	for (int i = 0; i < s->n; i++)
		if (s->kids[i].pid > 0 && kill(s->kids[i].pid, SIGTERM) != 0 && errno != ESRCH)
			pss_log("kill %s: %s", s->kids[i].name, strerror(errno));
}

static int running(const struct sup *s)
{
	for (int i = 0; i < s->n; i++)
		if (s->kids[i].pid > 0)
			return 1;
	return 0;
}

static int parse_args(struct sup *s, int argc, char **argv, const char **config)
{
	static const struct option opts[] = {
		{ "config", required_argument, NULL, 'c' },
		{ "hb-timeout-ms", required_argument, NULL, 't' },
		{ "first-hb-ms", required_argument, NULL, 'f' },
		{ "backoff-min-ms", required_argument, NULL, 'b' },
		{ "backoff-max-ms", required_argument, NULL, 'B' },
		{ "healthy-ms", required_argument, NULL, 'H' },
		{ "gone-retry-ms", required_argument, NULL, 'g' },
		{ "soak-every-s", required_argument, NULL, 's' },
		{ "stop-timeout-ms", required_argument, NULL, 'S' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int64_t ms = PSS_NS_PER_MS;
	uint32_t v;
	int opt;

	sup_policy_default(&s->pol);
	s->soak_every_ns = 3600 * PSS_NS_PER_S;
	s->stop_timeout_ns = 10 * PSS_NS_PER_S;
	*config = NULL;
	while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		if (opt == 'c') {
			*config = optarg;
			continue;
		}
		if (opt == 'h' || opt == '?' || pss_parse_u32(optarg, 1, 86400000, &v) != 0)
			return -1;
		switch (opt) {
		case 't': s->pol.hb_timeout_ns = v * ms; break;
		case 'f': s->pol.first_hb_ns = v * ms; break;
		case 'b': s->pol.backoff_min_ns = v * ms; break;
		case 'B': s->pol.backoff_max_ns = v * ms; break;
		case 'H': s->pol.healthy_ns = v * ms; break;
		case 'g': s->pol.gone_retry_ns = v * ms; break;
		case 's': s->soak_every_ns = v * PSS_NS_PER_S; break;
		case 'S': s->stop_timeout_ns = v * ms; break;
		default: return -1;
		}
	}
	if (*config == NULL || optind != argc || s->pol.backoff_min_ns > s->pol.backoff_max_ns)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	const int sigs[] = { SIGCHLD, SIGINT, SIGTERM };
	struct sup *s = &sup;
	const char *config;
	int code = 1, stale;

	pss_log_init("sup");
	s->epoll_fd = s->sig_fd = s->tick_fd = -1;
	/* Whatever started us (a shell, a terminal) may have leaked fds into us;
	 * don't pass them on to every daemon. systemd passes only 0-2. */
	if (close_range(3, ~0u, 0) != 0)
		pss_log("close_range: %s (inherited fds may reach children)", strerror(errno));
	if (parse_args(s, argc, argv, &config) != 0) {
		usage();
		return EXIT_USAGE;
	}
	if (find_exe_dir(s) != 0)
		return 1;
	if (load_config(s, config) != 0)
		return EXIT_USAGE;

	s->sig_fd = pss_signalfd(sigs, 3);
	if (s->sig_fd < 0)
		goto out;
	if (mkdir(pss_run_dir(), 0755) != 0 && errno != EEXIST) {
		pss_log("mkdir %s: %s", pss_run_dir(), strerror(errno));
		goto out;
	}
	if (snprintf(s->pids_path, sizeof s->pids_path, "%s/supervisor.pids", pss_run_dir()) >=
	    (int)sizeof s->pids_path) {
		pss_log("run dir path too long");
		goto out;
	}
	stale = clean_stale(s);
	s->tick_fd = pss_timerfd(TICK_NS, TICK_NS);
	if (s->tick_fd < 0)
		goto out;
	s->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (s->epoll_fd < 0) {
		pss_log("epoll_create1: %s", strerror(errno));
		goto out;
	}
	{
		struct epoll_event a = { .events = EPOLLIN, .data.u32 = TAG_SIG };
		struct epoll_event b = { .events = EPOLLIN, .data.u32 = TAG_TICK };

		if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->sig_fd, &a) != 0 ||
		    epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->tick_fd, &b) != 0) {
			pss_log("epoll_ctl ADD: %s", strerror(errno));
			goto out;
		}
	}

	s->start_ns = pss_mono_ns();
	s->next_soak_ns = s->start_ns + s->soak_every_ns;
	pss_log("start pid=%d config=%s children=%d, cleaned %d stale children", (int)getpid(), config, s->n, stale);
	for (int i = 0; i < s->n; i++)
		start_child(s, i, s->start_ns);

	for (;;) {
		struct epoll_event evs[MAX_CHILDREN + 2];
		int n;
		int64_t now;

		if (s->stopping && !running(s)) {
			code = 0;
			break;
		}
		n = epoll_wait(s->epoll_fd, evs, MAX_CHILDREN + 2, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			pss_log("epoll_wait: %s", strerror(errno));
			break;
		}
		now = pss_mono_ns();
		for (int i = 0; i < n; i++) {
			uint32_t tag = evs[i].data.u32;

			if (tag == TAG_SIG) {
				struct signalfd_siginfo si;

				while (read(s->sig_fd, &si, sizeof si) == (ssize_t)sizeof si) {
					if (si.ssi_signo == SIGCHLD)
						continue;
					if (!s->stopping) {
						pss_log("signal %u, stopping children", si.ssi_signo);
						begin_stop(s, now);
					}
				}
				reap(s, now);
			} else if (tag == TAG_TICK) {
				if (pss_timerfd_drain(s->tick_fd) != 0)
					goto out;
				tick(s, now);
			} else if (tag < MAX_CHILDREN) {
				on_child_fd(s, (int)tag, now);
			}
		}
	}
out:
	/* Error exit: tell children to stop (PR_SET_PDEATHSIG would too). The
	 * pid file stays, so the next supervisor can clean up stragglers. */
	if (code != 0 && running(s))
		begin_stop(s, pss_mono_ns());
	else if (s->pids_path[0] != '\0' && unlink(s->pids_path) != 0 && errno != ENOENT)
		pss_log("unlink %s: %s", s->pids_path, strerror(errno));
	for (int i = 0; i < s->n; i++)
		if (s->kids[i].fd >= 0)
			close(s->kids[i].fd);
	if (s->epoll_fd >= 0)
		close(s->epoll_fd);
	if (s->tick_fd >= 0)
		close(s->tick_fd);
	if (s->sig_fd >= 0)
		close(s->sig_fd);
	pss_log("exit %d", code);
	return code;
}
