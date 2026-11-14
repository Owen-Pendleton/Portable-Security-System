/*
 * A stand-in daemon for test_supervisor: heartbeats to the supervisor
 * through the real src/common/suphb.c, then misbehaves as told.
 *   fake_child ok            beat every 100 ms until SIGTERM, exit 0
 *   fake_child crash MS      beat, exit 1 after MS ms
 *   fake_child gone          exit 3 at once (device gone)
 *   fake_child hang MS       beat for MS ms, then stop beating (still alive)
 *   fake_child silent        never beat
 *   fake_child stubborn      like ok, but ignore SIGTERM
 */
#include "common/log.h"
#include "common/suphb.h"
#include "common/time.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t term;

static void on_term(int sig)
{
	(void)sig;
	term = 1;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	const char *mode = argc > 1 ? argv[1] : "ok";
	int64_t limit_ns = argc > 2 ? atoll(argv[2]) * PSS_NS_PER_MS : 0;
	int64_t start = pss_mono_ns();
	int beat = strcmp(mode, "silent") != 0;
	struct timespec ts = { 0, 100 * 1000000L };

	pss_log_init("fake_child");
	suphb_init();
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = strcmp(mode, "stubborn") == 0 ? SIG_IGN : on_term;
	sigaction(SIGTERM, &sa, NULL);
	if (strcmp(mode, "gone") == 0)
		return 3;
	/* Tell the test we're past exec and our SIGTERM handling is in place. */
	if (getenv("PSS_RUN_DIR") != NULL) {
		char path[512];
		FILE *f;

		snprintf(path, sizeof path, "%s/ready.%d", getenv("PSS_RUN_DIR"), (int)getpid());
		f = fopen(path, "w");
		if (f != NULL)
			fclose(f);
	}
	while (!term) {
		int64_t ran = pss_mono_ns() - start;

		if (strcmp(mode, "crash") == 0 && ran >= limit_ns)
			return 1;
		if (strcmp(mode, "hang") == 0 && ran >= limit_ns)
			beat = 0;
		if (beat)
			suphb_beat(0, 0);
		nanosleep(&ts, NULL);
	}
	return 0;
}
