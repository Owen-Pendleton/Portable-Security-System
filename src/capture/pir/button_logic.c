#include "capture/pir/button_logic.h"

#include <string.h>

void btn_filter_init(struct btn_filter *f, int64_t start_ns, int64_t debounce_ns, int initial_level)
{
	memset(f, 0, sizeof *f);
	f->debounce_ns = debounce_ns;
	f->last_edge_ns = start_ns;
	f->level = initial_level ? 1 : 0;
	/* Released at startup: a press right away counts. Held at startup: no
	 * press until it has been released first. */
	if (f->level) {
		f->high_known = 1;
		f->high_since_ns = start_ns - debounce_ns;
	}
}

enum btn_verdict btn_filter_edge(struct btn_filter *f, int rising, int64_t ts_ns)
{
	int level = rising ? 1 : 0;

	if (level == f->level)
		return BTN_DUPLICATE;
	f->level = level;
	f->last_edge_ns = ts_ns;
	if (rising) {
		f->high_known = 1;
		f->high_since_ns = ts_ns;
		return BTN_RELEASE;
	}
	if (f->high_known && ts_ns - f->high_since_ns >= f->debounce_ns) {
		f->presses++;
		return BTN_PRESS;
	}
	f->bounces++;
	return BTN_BOUNCE;
}
