#include "analysis/arm.h"

#include <string.h>

void arm_init(struct arm *a, int64_t exit_delay_ns, int64_t grace_ns)
{
	memset(a, 0, sizeof *a);
	a->exit_delay_ns = exit_delay_ns;
	a->grace_ns = grace_ns;
	a->state = ARM_DISARMED;
}

static int go(struct arm *a, enum arm_state to, enum arm_cause cause, enum arm_label label,
	      uint64_t event_id, struct arm_change *c)
{
	c->from = a->state;
	c->to = to;
	c->cause = cause;
	c->label = label;
	c->event_id = event_id;
	a->state = to;
	if (to != ARM_GRACE)
		a->grace_event_id = 0;
	return 1;
}

int arm_restore(struct arm *a, uint32_t saved_state, uint64_t saved_event_id, int64_t now_ns,
		struct arm_change *c)
{
	switch (saved_state) {
	case ARM_ARMED:
		a->state = ARM_ARMED;
		return 0;
	case ARM_EXIT_DELAY:
		a->state = ARM_EXIT_DELAY;	/* the full delay starts over */
		a->deadline_ns = now_ns + a->exit_delay_ns;
		return go(a, ARM_EXIT_DELAY, ARM_CAUSE_RESTORE, ARM_LABEL_NONE, 0, c);
	case ARM_GRACE:
		a->state = ARM_GRACE;
		return go(a, ARM_ARMED, ARM_CAUSE_RESTORE, ARM_LABEL_REPORTED, saved_event_id, c);
	default:	/* DISARMED, nothing saved, or an unknown value */
		a->state = ARM_DISARMED;
		return 0;
	}
}

int arm_button(struct arm *a, int64_t now_ns, struct arm_change *c)
{
	switch (a->state) {
	case ARM_DISARMED:
		a->deadline_ns = now_ns + a->exit_delay_ns;
		return go(a, ARM_EXIT_DELAY, ARM_CAUSE_BUTTON, ARM_LABEL_NONE, 0, c);
	case ARM_EXIT_DELAY:
	case ARM_ARMED:
		return go(a, ARM_DISARMED, ARM_CAUSE_BUTTON, ARM_LABEL_NONE, 0, c);
	case ARM_GRACE:
		return go(a, ARM_DISARMED, ARM_CAUSE_BUTTON, ARM_LABEL_OWNER, a->grace_event_id, c);
	}
	return 0;
}

int arm_entry(struct arm *a, uint64_t event_id, int64_t now_ns, struct arm_change *c)
{
	if (a->state != ARM_ARMED)
		return 0;
	a->deadline_ns = now_ns + a->grace_ns;
	go(a, ARM_GRACE, ARM_CAUSE_ENTRY, ARM_LABEL_NONE, event_id, c);
	a->grace_event_id = event_id;
	return 1;
}

int arm_tick(struct arm *a, int64_t now_ns, struct arm_change *c)
{
	if (a->state == ARM_EXIT_DELAY && now_ns >= a->deadline_ns)
		return go(a, ARM_ARMED, ARM_CAUSE_EXIT_DELAY_OVER, ARM_LABEL_NONE, 0, c);
	if (a->state == ARM_GRACE && now_ns >= a->deadline_ns)
		return go(a, ARM_ARMED, ARM_CAUSE_GRACE_OVER, ARM_LABEL_REPORTED, a->grace_event_id, c);
	return 0;
}

int arm_watching(const struct arm *a)
{
	return a->state == ARM_ARMED || a->state == ARM_GRACE;
}

enum arm_led arm_led(const struct arm *a)
{
	return a->state == ARM_EXIT_DELAY ? ARM_LED_BLINK : ARM_LED_OFF;
}

const char *arm_state_name(uint32_t state)
{
	switch (state) {
	case ARM_DISARMED: return "DISARMED";
	case ARM_EXIT_DELAY: return "EXIT_DELAY";
	case ARM_ARMED: return "ARMED";
	case ARM_GRACE: return "GRACE";
	}
	return "?";
}

const char *arm_label_name(uint32_t label)
{
	switch (label) {
	case ARM_LABEL_NONE: return "";
	case ARM_LABEL_OWNER: return "owner";
	case ARM_LABEL_REPORTED: return "reported";
	}
	return "?";
}
