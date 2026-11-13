/* Fusion: PIR rise + sound onset within +-T_co confirms an entry. */
#include "t.h"

#include "analysis/fusion.h"

#define MS 1000000LL
#define S 1000000000LL

static void test_rise_then_onset(void)
{
	struct fusion f;
	int64_t dt = 0;

	fusion_init(&f, 3 * S);
	CHECK(fusion_rise(&f, 100 * S, &dt) == 0);		/* nothing heard yet */
	CHECK(fusion_onset(&f, 100 * S + 2900 * MS, &dt) == 1);
	CHECK(dt == 2900 * MS);

	fusion_init(&f, 3 * S);
	CHECK(fusion_rise(&f, 100 * S, NULL) == 0);
	CHECK(fusion_onset(&f, 100 * S + 3100 * MS, NULL) == 0);	/* too late */
}

static void test_onset_then_rise(void)
{
	struct fusion f;
	int64_t dt = 0;

	/* The usual door: latch sound first, then the person walks into view. */
	fusion_init(&f, 3 * S);
	CHECK(fusion_onset(&f, 50 * S, &dt) == 0);
	CHECK(fusion_rise(&f, 51 * S, &dt) == 1);
	CHECK(dt == -1 * S);

	fusion_init(&f, 3 * S);
	CHECK(fusion_onset(&f, 50 * S, NULL) == 0);
	CHECK(fusion_rise(&f, 54 * S, NULL) == 0);	/* sound was 4 s before */
}

static void test_boundary_inclusive(void)
{
	struct fusion f;

	fusion_init(&f, 3 * S);
	CHECK(fusion_rise(&f, 10 * S, NULL) == 0);
	CHECK(fusion_onset(&f, 13 * S, NULL) == 1);
	fusion_init(&f, 3 * S);
	CHECK(fusion_onset(&f, 10 * S, NULL) == 0);
	CHECK(fusion_rise(&f, 13 * S, NULL) == 1);
	fusion_init(&f, 3 * S);
	CHECK(fusion_onset(&f, 10 * S, NULL) == 0);
	CHECK(fusion_rise(&f, 13 * S + 1, NULL) == 0);
}

static void test_out_of_order_arrival(void)
{
	struct fusion f;
	int64_t dt = 0;

	/* The onset message arrives after the rise message but carries an
	 * earlier timestamp; still a match, with a negative dt. */
	fusion_init(&f, 3 * S);
	CHECK(fusion_rise(&f, 20 * S, NULL) == 0);
	CHECK(fusion_onset(&f, 19 * S, &dt) == 1);
	CHECK(dt == -1 * S);
}

static void test_latest_signal_is_used(void)
{
	struct fusion f;

	/* An old rise doesn't match, but a newer one does. */
	fusion_init(&f, 3 * S);
	CHECK(fusion_rise(&f, 0, NULL) == 0);
	CHECK(fusion_rise(&f, 10 * S, NULL) == 0);
	CHECK(fusion_onset(&f, 12 * S, NULL) == 1);
}

static void test_forget(void)
{
	struct fusion f;

	fusion_init(&f, 3 * S);
	CHECK(fusion_onset(&f, 5 * S, NULL) == 0);
	fusion_forget(&f, 0, 1);	/* mic disconnected */
	CHECK(fusion_rise(&f, 6 * S, NULL) == 0);
	fusion_forget(&f, 1, 0);	/* PIR disconnected */
	CHECK(fusion_onset(&f, 7 * S, NULL) == 0);
	CHECK(fusion_rise(&f, 8 * S, NULL) == 1);	/* both live again */
}

int main(void)
{
	T_RUN(test_rise_then_onset);
	T_RUN(test_onset_then_rise);
	T_RUN(test_boundary_inclusive);
	T_RUN(test_out_of_order_arrival);
	T_RUN(test_latest_signal_is_used);
	T_RUN(test_forget);
	T_END();
}
