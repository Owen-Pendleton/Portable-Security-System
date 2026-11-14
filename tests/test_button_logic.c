/* pird's arm-button filter: one press per physical press, bounces dropped, no warm-up. */
#include "t.h"

#include "capture/pir/button_logic.h"

#define MS 1000000LL
#define US 1000LL
#define S 1000000000LL

static void test_clean_press(void)
{
	struct btn_filter f;

	btn_filter_init(&f, 0, 20 * MS, 1);
	CHECK(btn_filter_edge(&f, 0, 5 * S) == BTN_PRESS);
	CHECK(btn_filter_edge(&f, 1, 5 * S + 200 * MS) == BTN_RELEASE);
	CHECK(f.presses == 1 && f.bounces == 0);
}

static void test_bounce_on_press_and_release(void)
{
	struct btn_filter f;
	int64_t t = 10 * S;

	btn_filter_init(&f, 0, 20 * MS, 1);
	/* Press: fall, then chatter for ~3 ms. */
	CHECK(btn_filter_edge(&f, 0, t) == BTN_PRESS);
	CHECK(btn_filter_edge(&f, 1, t + 300 * US) == BTN_RELEASE);
	CHECK(btn_filter_edge(&f, 0, t + 700 * US) == BTN_BOUNCE);
	CHECK(btn_filter_edge(&f, 1, t + 1500 * US) == BTN_RELEASE);
	CHECK(btn_filter_edge(&f, 0, t + 3 * MS) == BTN_BOUNCE);
	/* Release 150 ms later, chatter for ~5 ms. */
	t += 150 * MS;
	CHECK(btn_filter_edge(&f, 1, t) == BTN_RELEASE);
	CHECK(btn_filter_edge(&f, 0, t + 1 * MS) == BTN_BOUNCE);
	CHECK(btn_filter_edge(&f, 1, t + 2 * MS) == BTN_RELEASE);
	CHECK(btn_filter_edge(&f, 0, t + 4 * MS) == BTN_BOUNCE);
	CHECK(btn_filter_edge(&f, 1, t + 5 * MS) == BTN_RELEASE);
	CHECK(f.presses == 1 && f.bounces == 4);
}

static void test_two_presses(void)
{
	struct btn_filter f;

	btn_filter_init(&f, 0, 20 * MS, 1);
	CHECK(btn_filter_edge(&f, 0, 1 * S) == BTN_PRESS);
	CHECK(btn_filter_edge(&f, 1, 1 * S + 80 * MS) == BTN_RELEASE);
	/* High for exactly the debounce time: counts (boundary). */
	CHECK(btn_filter_edge(&f, 0, 1 * S + 100 * MS) == BTN_PRESS);
	CHECK(btn_filter_edge(&f, 1, 1 * S + 180 * MS) == BTN_RELEASE);
	/* High for just under the debounce time: bounce. */
	CHECK(btn_filter_edge(&f, 0, 1 * S + 200 * MS - 1) == BTN_BOUNCE);
	CHECK(f.presses == 2 && f.bounces == 1);
}

static void test_no_warmup(void)
{
	struct btn_filter f;

	/* A press at the very start counts; the PIR's 60 s warm-up is not here. */
	btn_filter_init(&f, 100 * S, 20 * MS, 1);
	CHECK(btn_filter_edge(&f, 0, 100 * S) == BTN_PRESS);
	CHECK(f.presses == 1);
}

static void test_held_at_startup(void)
{
	struct btn_filter f;

	btn_filter_init(&f, 0, 20 * MS, 0);
	CHECK(btn_filter_edge(&f, 0, 1 * S) == BTN_DUPLICATE);
	CHECK(btn_filter_edge(&f, 1, 2 * S) == BTN_RELEASE);
	CHECK(btn_filter_edge(&f, 0, 3 * S) == BTN_PRESS);
	CHECK(f.presses == 1 && f.bounces == 0);
}

static void test_duplicates_no_phantom_press(void)
{
	struct btn_filter f;

	btn_filter_init(&f, 0, 20 * MS, 1);
	CHECK(btn_filter_edge(&f, 1, 1 * S) == BTN_DUPLICATE);
	CHECK(btn_filter_edge(&f, 0, 2 * S) == BTN_PRESS);
	CHECK(btn_filter_edge(&f, 0, 3 * S) == BTN_DUPLICATE);
	CHECK(f.presses == 1);
}

static void test_debounce_zero_counts_every_fall(void)
{
	struct btn_filter f;

	btn_filter_init(&f, 0, 0, 1);
	CHECK(btn_filter_edge(&f, 0, 1 * S) == BTN_PRESS);
	CHECK(btn_filter_edge(&f, 1, 1 * S + 1 * US) == BTN_RELEASE);
	CHECK(btn_filter_edge(&f, 0, 1 * S + 2 * US) == BTN_PRESS);
	CHECK(f.presses == 2 && f.bounces == 0);
}

int main(void)
{
	T_RUN(test_clean_press);
	T_RUN(test_bounce_on_press_and_release);
	T_RUN(test_two_presses);
	T_RUN(test_no_warmup);
	T_RUN(test_held_at_startup);
	T_RUN(test_duplicates_no_phantom_press);
	T_RUN(test_debounce_zero_counts_every_fall);
	T_END();
}
