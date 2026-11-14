/* Arm state machine (DESIGN.md Section 4), including Bryan's decisions:
 * button while ARMED disarms, button during EXIT_DELAY cancels, state
 * survives a restart. */
#include "t.h"

#include "analysis/arm.h"

#define S 1000000000LL
#define EXIT (60 * S)
#define GRACE (30 * S)

static void armed_at(struct arm *a, int64_t t)
{
	struct arm_change c;

	arm_init(a, EXIT, GRACE);
	CHECK(arm_button(a, t, &c) == 1);
	CHECK(arm_tick(a, t + EXIT, &c) == 1);
	CHECK(a->state == ARM_ARMED);
}

static void test_starts_disarmed(void)
{
	struct arm a;
	struct arm_change c;

	arm_init(&a, EXIT, GRACE);
	CHECK(a.state == ARM_DISARMED);
	CHECK(!arm_watching(&a));
	CHECK(arm_led(&a) == ARM_LED_OFF);
	CHECK(arm_tick(&a, 1000 * S, &c) == 0);
	CHECK(arm_entry(&a, 7, 1000 * S, &c) == 0);	/* entries mean nothing while disarmed */
	CHECK(a.state == ARM_DISARMED);
}

static void test_exit_delay_to_armed(void)
{
	struct arm a;
	struct arm_change c;

	arm_init(&a, EXIT, GRACE);
	CHECK(arm_button(&a, 100 * S, &c) == 1);
	CHECK(c.from == ARM_DISARMED && c.to == ARM_EXIT_DELAY && c.cause == ARM_CAUSE_BUTTON);
	CHECK(arm_led(&a) == ARM_LED_BLINK);
	CHECK(!arm_watching(&a));		/* Macy walking out isn't recorded */
	CHECK(arm_entry(&a, 1, 120 * S, &c) == 0);
	CHECK(arm_tick(&a, 160 * S - 1, &c) == 0);
	CHECK(arm_tick(&a, 160 * S, &c) == 1);	/* exactly 60 s */
	CHECK(c.from == ARM_EXIT_DELAY && c.to == ARM_ARMED && c.cause == ARM_CAUSE_EXIT_DELAY_OVER);
	CHECK(arm_led(&a) == ARM_LED_OFF);
	CHECK(arm_watching(&a));
}

static void test_button_cancels_exit_delay(void)	/* decision 2 */
{
	struct arm a;
	struct arm_change c;

	arm_init(&a, EXIT, GRACE);
	CHECK(arm_button(&a, 100 * S, &c) == 1);
	CHECK(arm_button(&a, 110 * S, &c) == 1);
	CHECK(c.from == ARM_EXIT_DELAY && c.to == ARM_DISARMED && c.cause == ARM_CAUSE_BUTTON);
	CHECK(arm_led(&a) == ARM_LED_OFF);
	CHECK(arm_tick(&a, 200 * S, &c) == 0);	/* the old deadline is dead */
	CHECK(a.state == ARM_DISARMED);
	/* Pressing again gives a fresh full delay. */
	CHECK(arm_button(&a, 300 * S, &c) == 1);
	CHECK(arm_tick(&a, 359 * S, &c) == 0);
	CHECK(arm_tick(&a, 360 * S, &c) == 1 && a.state == ARM_ARMED);
}

static void test_button_while_armed_disarms(void)	/* decision 1 */
{
	struct arm a;
	struct arm_change c;

	armed_at(&a, 0);
	CHECK(arm_button(&a, 500 * S, &c) == 1);
	CHECK(c.from == ARM_ARMED && c.to == ARM_DISARMED && c.cause == ARM_CAUSE_BUTTON);
	CHECK(c.label == ARM_LABEL_NONE && c.event_id == 0);
	CHECK(!arm_watching(&a));
}

static void test_grace_button_is_owner(void)
{
	struct arm a;
	struct arm_change c;

	armed_at(&a, 0);
	CHECK(arm_entry(&a, 42, 500 * S, &c) == 1);
	CHECK(c.from == ARM_ARMED && c.to == ARM_GRACE && c.cause == ARM_CAUSE_ENTRY && c.event_id == 42);
	CHECK(arm_watching(&a));
	CHECK(arm_led(&a) == ARM_LED_OFF);
	CHECK(arm_entry(&a, 43, 501 * S, &c) == 0);	/* already in GRACE */
	CHECK(arm_tick(&a, 529 * S, &c) == 0);
	CHECK(arm_button(&a, 529 * S, &c) == 1);
	CHECK(c.from == ARM_GRACE && c.to == ARM_DISARMED);
	CHECK(c.label == ARM_LABEL_OWNER && c.event_id == 42);
	CHECK(arm_tick(&a, 600 * S, &c) == 0);	/* the grace deadline is dead */
	CHECK(a.state == ARM_DISARMED);
}

static void test_grace_runs_out_is_reported(void)
{
	struct arm a;
	struct arm_change c;

	armed_at(&a, 0);
	CHECK(arm_entry(&a, 9, 500 * S, &c) == 1);
	CHECK(arm_tick(&a, 530 * S - 1, &c) == 0);
	CHECK(arm_tick(&a, 530 * S, &c) == 1);	/* exactly 30 s */
	CHECK(c.from == ARM_GRACE && c.to == ARM_ARMED && c.cause == ARM_CAUSE_GRACE_OVER);
	CHECK(c.label == ARM_LABEL_REPORTED && c.event_id == 9);
	/* Back to ARMED: a later press is a plain disarm, not an owner label. */
	CHECK(arm_button(&a, 600 * S, &c) == 1);
	CHECK(c.label == ARM_LABEL_NONE && c.event_id == 0);
	/* And a later entry gets its own GRACE. */
	armed_at(&a, 700 * S);
	CHECK(arm_entry(&a, 10, 800 * S, &c) == 1 && c.event_id == 10);
}

static void test_restore(void)	/* decision 3 */
{
	struct arm a;
	struct arm_change c;

	arm_init(&a, EXIT, GRACE);
	CHECK(arm_restore(&a, 0, 0, 5 * S, &c) == 0);	/* empty store */
	CHECK(a.state == ARM_DISARMED);

	arm_init(&a, EXIT, GRACE);
	CHECK(arm_restore(&a, ARM_DISARMED, 0, 5 * S, &c) == 0);
	CHECK(a.state == ARM_DISARMED);

	arm_init(&a, EXIT, GRACE);
	CHECK(arm_restore(&a, ARM_ARMED, 0, 5 * S, &c) == 0);
	CHECK(a.state == ARM_ARMED && arm_watching(&a));

	/* EXIT_DELAY: the full delay starts over from the restart. */
	arm_init(&a, EXIT, GRACE);
	CHECK(arm_restore(&a, ARM_EXIT_DELAY, 0, 5 * S, &c) == 1);
	CHECK(c.from == ARM_EXIT_DELAY && c.to == ARM_EXIT_DELAY && c.cause == ARM_CAUSE_RESTORE);
	CHECK(arm_led(&a) == ARM_LED_BLINK);
	CHECK(arm_tick(&a, 65 * S - 1, &c) == 0);
	CHECK(arm_tick(&a, 65 * S, &c) == 1 && a.state == ARM_ARMED);

	/* GRACE: comes back ARMED, and the entry is reported. */
	arm_init(&a, EXIT, GRACE);
	CHECK(arm_restore(&a, ARM_GRACE, 77, 5 * S, &c) == 1);
	CHECK(c.from == ARM_GRACE && c.to == ARM_ARMED && c.cause == ARM_CAUSE_RESTORE);
	CHECK(c.label == ARM_LABEL_REPORTED && c.event_id == 77);
	CHECK(a.state == ARM_ARMED);
	CHECK(arm_button(&a, 6 * S, &c) == 1 && c.label == ARM_LABEL_NONE);	/* no stale owner label */

	/* Garbage on disk: fail safe to DISARMED rather than guess. */
	arm_init(&a, EXIT, GRACE);
	CHECK(arm_restore(&a, 99, 0, 5 * S, &c) == 0);
	CHECK(a.state == ARM_DISARMED);
}

static void test_names(void)
{
	CHECK(arm_state_name(ARM_GRACE)[0] == 'G');
	CHECK(arm_state_name(0)[0] == '?');
	CHECK(arm_label_name(ARM_LABEL_OWNER)[0] == 'o');
}

int main(void)
{
	T_RUN(test_starts_disarmed);
	T_RUN(test_exit_delay_to_armed);
	T_RUN(test_button_cancels_exit_delay);
	T_RUN(test_button_while_armed_disarms);
	T_RUN(test_grace_button_is_owner);
	T_RUN(test_grace_runs_out_is_reported);
	T_RUN(test_restore);
	T_RUN(test_names);
	T_END();
}
