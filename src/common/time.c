#include "common/time.h"

#include "common/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t clock_ns(clockid_t id, const char *name)
{
	struct timespec ts;

	if (clock_gettime(id, &ts) != 0) {
		/* Only fails for a bad clock id or pointer: a programming error. */
		pss_log("clock_gettime(%s): %s", name, strerror(errno));
		abort();
	}
	return (int64_t)ts.tv_sec * PSS_NS_PER_S + ts.tv_nsec;
}

int64_t pss_mono_ns(void)
{
	return clock_ns(CLOCK_MONOTONIC, "CLOCK_MONOTONIC");
}

int64_t pss_wall_ns(void)
{
	return clock_ns(CLOCK_REALTIME, "CLOCK_REALTIME");
}
