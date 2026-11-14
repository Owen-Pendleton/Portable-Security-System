#ifndef PSS_ANALYSIS_ARM_H
#define PSS_ANALYSIS_ARM_H

/*
 * Arm state machine (DESIGN.md Section 4). Pure logic, no I/O, so it is
 * testable off the Pi; the hub feeds it button presses, confirmed entries
 * and clock ticks, and persists every change as a PSS_REC_ARM record.
 *
 *   DISARMED   --button-->         EXIT_DELAY (LED blinks)
 *   EXIT_DELAY --button-->         DISARMED (cancelled)
 *   EXIT_DELAY --exit delay over-> ARMED (LED off)
 *   ARMED      --button-->         DISARMED
 *   ARMED      --ENTRY-->          GRACE
 *   GRACE      --button-->         DISARMED, entry labelled owner
 *   GRACE      --grace over-->     ARMED, entry labelled reported
 *
 * After a restart (arm_restore): DISARMED and ARMED come back as they
 * were, EXIT_DELAY restarts the full delay, and GRACE comes back ARMED
 * with the entry reported, so pulling the plug can't hide an entry.
 *
 * The state, cause and label values are stored on disk: never renumber.
 */

#include <stdint.h>

enum arm_state {
	ARM_DISARMED = 1,
	ARM_EXIT_DELAY = 2,
	ARM_ARMED = 3,
	ARM_GRACE = 4,
};

enum arm_cause {
	ARM_CAUSE_BUTTON = 1,
	ARM_CAUSE_EXIT_DELAY_OVER = 2,
	ARM_CAUSE_ENTRY = 3,
	ARM_CAUSE_GRACE_OVER = 4,
	ARM_CAUSE_RESTORE = 5,	/* restart: state reloaded from the store */
};

enum arm_label {
	ARM_LABEL_NONE = 0,
	ARM_LABEL_OWNER = 1,	/* button pressed during GRACE */
	ARM_LABEL_REPORTED = 2,	/* GRACE ran out (or was cut short by a restart) */
};

enum arm_led { ARM_LED_OFF = 0, ARM_LED_BLINK = 1 };

struct arm {
	int64_t exit_delay_ns, grace_ns;
	enum arm_state state;
	int64_t deadline_ns;	/* EXIT_DELAY and GRACE only */
	uint64_t grace_event_id;	/* GRACE only */
};

/* One transition, for the hub to log and persist. */
struct arm_change {
	enum arm_state from, to;
	enum arm_cause cause;
	enum arm_label label;
	uint64_t event_id;	/* the entry a GRACE transition is about, else 0 */
};

void arm_init(struct arm *a, int64_t exit_delay_ns, int64_t grace_ns);
/* Reloads the last persisted state (0 if none: DISARMED, no change). Returns
 * 1 and fills *c if the hub must persist a change, else 0. */
int arm_restore(struct arm *a, uint32_t saved_state, uint64_t saved_event_id, int64_t now_ns,
		struct arm_change *c);
/* Each returns 1 and fills *c on a transition, else 0. */
int arm_button(struct arm *a, int64_t now_ns, struct arm_change *c);
int arm_entry(struct arm *a, uint64_t event_id, int64_t now_ns, struct arm_change *c);
int arm_tick(struct arm *a, int64_t now_ns, struct arm_change *c);

/* ARMED or GRACE: new events may be recorded and the camera may fire. */
int arm_watching(const struct arm *a);
enum arm_led arm_led(const struct arm *a);
const char *arm_state_name(uint32_t state);
const char *arm_label_name(uint32_t label);

#endif
