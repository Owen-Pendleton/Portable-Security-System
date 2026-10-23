#include "common/util.h"

#include "common/log.h"
#include "common/time.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

static const char *env_or(const char *name, const char *def)
{
	const char *v = getenv(name);

	return (v != NULL && v[0] != '\0') ? v : def;
}

const char *pss_run_dir(void)
{
	return env_or("PSS_RUN_DIR", "/run/pss");
}

const char *pss_data_dir(void)
{
	return env_or("PSS_DATA_DIR", "/var/lib/pss");
}

int pss_parse_u32(const char *s, uint32_t min, uint32_t max, uint32_t *out)
{
	unsigned long v;
	char *end;

	if (s == NULL || s[0] < '0' || s[0] > '9')
		return -1;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno != 0 || *end != '\0' || v < min || v > max)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

int pss_signalfd(const int *sigs, size_t n)
{
	sigset_t set;
	int fd;

	sigemptyset(&set);
	for (size_t i = 0; i < n; i++)
		sigaddset(&set, sigs[i]);
	if (sigprocmask(SIG_BLOCK, &set, NULL) != 0) {
		pss_log("sigprocmask: %s", strerror(errno));
		return -1;
	}
	fd = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
	if (fd < 0)
		pss_log("signalfd: %s", strerror(errno));
	return fd;
}

static struct timespec ns_to_ts(int64_t ns)
{
	struct timespec ts = { (time_t)(ns / PSS_NS_PER_S), (long)(ns % PSS_NS_PER_S) };

	return ts;
}

int pss_timerfd(int64_t first_ns, int64_t period_ns)
{
	struct itimerspec its;
	int fd;

	fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (fd < 0) {
		pss_log("timerfd_create: %s", strerror(errno));
		return -1;
	}
	its.it_value = ns_to_ts(first_ns);
	its.it_interval = ns_to_ts(period_ns);
	if (timerfd_settime(fd, 0, &its, NULL) != 0) {
		pss_log("timerfd_settime: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

int pss_timerfd_drain(int fd)
{
	uint64_t expirations;
	ssize_t r = read(fd, &expirations, sizeof expirations);

	if (r == (ssize_t)sizeof expirations || (r < 0 && errno == EAGAIN))
		return 0;
	pss_log("timerfd read: %s", r < 0 ? strerror(errno) : "short read");
	return -1;
}
