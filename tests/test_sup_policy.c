/* Supervisor restart rules (DESIGN.md Section 3, mechanism E). */
#include "t.h"

#include "supervisor/policy.h"

#define S 1000000000LL

static void test_backoff_doubles_to_cap(void)
{
	struct sup_policy p;
	struct sup_track t;
	const int64_t want[] = { 1, 2, 4, 8, 16, 32, 60, 60, 60 };
	int64_t now = 0;

	sup_policy_default(&p);
	sup_track_init(&p, &t);
	for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
		int64_t d;

		sup_track_started(&t, now);
		now += 5 * S;	/* crashes after 5 s every time */
		d = sup_restart_delay(&p, &t, now, -1);
		CHECK(d == want[i] * S);
		now += d;
	}
}

static void test_healthy_run_resets_backoff(void)
{
	struct sup_policy p;
	struct sup_track t;

	sup_policy_default(&p);
	sup_track_init(&p, &t);
	sup_track_started(&t, 0);
	CHECK(sup_restart_delay(&p, &t, 1 * S, 1) == 1 * S);
	sup_track_started(&t, 2 * S);
	CHECK(sup_restart_delay(&p, &t, 3 * S, 1) == 2 * S);
	/* Ran 10 min minus 1 ns: still backing off. */
	sup_track_started(&t, 10 * S);
	CHECK(sup_restart_delay(&p, &t, 610 * S - 1, 1) == 4 * S);
	/* Ran exactly 10 min: back to 1 s. */
	sup_track_started(&t, 1000 * S);
	CHECK(sup_restart_delay(&p, &t, 1600 * S, 1) == 1 * S);
	sup_track_started(&t, 1601 * S);
	CHECK(sup_restart_delay(&p, &t, 1602 * S, 1) == 2 * S);
}

static void test_device_gone_retries_every_30s(void)
{
	struct sup_policy p;
	struct sup_track t;

	sup_policy_default(&p);
	sup_track_init(&p, &t);
	for (int i = 0; i < 5; i++) {
		sup_track_started(&t, i * 100 * S);
		CHECK(sup_restart_delay(&p, &t, i * 100 * S + 1 * S, SUP_EXIT_DEVICE_GONE) == 30 * S);
	}
	/* Unplug retries didn't grow the ordinary backoff. */
	sup_track_started(&t, 1000 * S);
	CHECK(sup_restart_delay(&p, &t, 1001 * S, 1) == 1 * S);
}

static void test_hung(void)
{
	struct sup_policy p;
	struct sup_track t;

	sup_policy_default(&p);
	sup_track_init(&p, &t);
	sup_track_started(&t, 100 * S);
	/* No heartbeat yet: 60 s allowed (PIR warm-up, slow USB). */
	CHECK(!sup_hung(&p, &t, 160 * S - 1));
	CHECK(sup_hung(&p, &t, 160 * S));
	/* Once beating: 15 s since the last beat. */
	sup_track_beat(&t, 105 * S);
	CHECK(!sup_hung(&p, &t, 120 * S - 1));
	CHECK(sup_hung(&p, &t, 120 * S));
	sup_track_beat(&t, 119 * S);
	CHECK(!sup_hung(&p, &t, 130 * S));
	/* A restart forgets the old beats. */
	sup_track_started(&t, 200 * S);
	CHECK(!sup_hung(&p, &t, 250 * S));
	CHECK(sup_hung(&p, &t, 260 * S));
}

int main(void)
{
	T_RUN(test_backoff_doubles_to_cap);
	T_RUN(test_healthy_run_resets_backoff);
	T_RUN(test_device_gone_retries_every_30s);
	T_RUN(test_hung);
	T_END();
}
