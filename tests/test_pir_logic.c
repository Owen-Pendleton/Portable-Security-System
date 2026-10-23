/* pird's edge filter: warm-up suppression and duplicate-direction suppression. */
#include "t.h"

#include "capture/pir/pir_logic.h"

#define S 1000000000LL

static void test_warmup_suppresses_then_forwards(void)
{
	struct pir_filter f;

	pir_filter_init(&f, 100 * S, 60 * S, 0);
	CHECK(pir_filter_edge(&f, 1, 110 * S) == PIR_WARMUP);
	CHECK(pir_filter_edge(&f, 0, 120 * S) == PIR_WARMUP);
	CHECK(pir_filter_edge(&f, 1, 160 * S) == PIR_FORWARD); /* boundary: warm-up over */
	CHECK(pir_filter_edge(&f, 0, 163 * S) == PIR_FORWARD);
	CHECK(f.forwarded == 2 && f.suppressed == 2);
}

static void test_level_tracked_through_warmup(void)
{
	struct pir_filter f;

	/* Rose during warm-up and is still high: the next edge must be a fall. */
	pir_filter_init(&f, 0, 60 * S, 0);
	CHECK(pir_filter_edge(&f, 1, 59 * S) == PIR_WARMUP);
	CHECK(pir_filter_edge(&f, 1, 61 * S) == PIR_DUPLICATE);
	CHECK(pir_filter_edge(&f, 0, 62 * S) == PIR_FORWARD);
}

static void test_duplicates_dropped(void)
{
	struct pir_filter f;

	pir_filter_init(&f, 0, 0, 1);
	CHECK(pir_filter_edge(&f, 1, 1 * S) == PIR_DUPLICATE);
	CHECK(pir_filter_edge(&f, 0, 2 * S) == PIR_FORWARD);
	CHECK(pir_filter_edge(&f, 0, 3 * S) == PIR_DUPLICATE);
	CHECK(pir_filter_edge(&f, 1, 4 * S) == PIR_FORWARD);
}

int main(void)
{
	T_RUN(test_warmup_suppresses_then_forwards);
	T_RUN(test_level_tracked_through_warmup);
	T_RUN(test_duplicates_dropped);
	T_END();
}
