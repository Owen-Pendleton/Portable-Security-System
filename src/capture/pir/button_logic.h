#ifndef PSS_CAPTURE_BUTTON_LOGIC_H
#define PSS_CAPTURE_BUTTON_LOGIC_H

/*
 * Hardware-independent arm-button filter for pird, testable off the Pi.
 * The button pulls the line low when pressed (Pi pull-up, DESIGN.md Section 4).
 * Contacts bounce, so a falling edge counts as a press only if the line had
 * been high for at least debounce_ns before it. The press is reported on that
 * edge, with no added delay. There is no warm-up: unlike the PIR, the button
 * is trusted from the start.
 */

#include <stdint.h>

enum btn_verdict {
	BTN_PRESS = 0,		/* forward to the hub */
	BTN_RELEASE = 1,	/* rising edge; not forwarded */
	BTN_BOUNCE = 2,		/* fall too soon after a rise; ignored */
	BTN_DUPLICATE = 3,	/* same direction as the last edge; ignored */
};

struct btn_filter {
	int64_t debounce_ns;
	int64_t high_since_ns;	/* valid only when high_known */
	int64_t last_edge_ns;	/* previous edge (or start), for logging bounce timing */
	int level;		/* 1 released (high), 0 pressed (low) */
	int high_known;		/* 0 when held at startup and not yet released */
	uint64_t presses;
	uint64_t bounces;
};

void btn_filter_init(struct btn_filter *f, int64_t start_ns, int64_t debounce_ns, int initial_level);
enum btn_verdict btn_filter_edge(struct btn_filter *f, int rising, int64_t ts_ns);

#endif
