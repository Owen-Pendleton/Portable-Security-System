#include "common/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static const char *log_tag = "pss";

void pss_log_init(const char *tag)
{
	log_tag = tag;
}

void pss_log(const char *fmt, ...)
{
	char line[1024];
	char ts[32] = "(no clock)";
	struct timespec now = { 0, 0 };
	struct tm tm;
	va_list ap;
	size_t len;
	int n;

	if (clock_gettime(CLOCK_REALTIME, &now) == 0 && localtime_r(&now.tv_sec, &tm) != NULL)
		strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

	n = snprintf(line, sizeof line, "%s.%03ld %s: ", ts, now.tv_nsec / 1000000, log_tag);
	if (n < 0)
		return;
	len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;

	va_start(ap, fmt);
	n = vsnprintf(line + len, sizeof line - len, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	len += (size_t)n;
	if (len > sizeof line - 2)
		len = sizeof line - 2;
	line[len++] = '\n';

	/* One write() per line so lines from concurrent daemons don't interleave. */
	if (write(STDERR_FILENO, line, len) < 0) {
		/* stderr is gone; there is nowhere left to report this. */
	}
}
