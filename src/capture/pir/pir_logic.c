#include "capture/pir/pir_logic.h"

#include <string.h>

void pir_filter_init(struct pir_filter *f, int64_t start_ns, int64_t warmup_ns, int initial_level)
{
	memset(f, 0, sizeof *f);
	f->warmup_until_ns = start_ns + warmup_ns;
	f->level = initial_level ? 1 : 0;
}

enum pir_verdict pir_filter_edge(struct pir_filter *f, int rising, int64_t ts_ns)
{
	int level = rising ? 1 : 0;

	if (level == f->level) {
		f->suppressed++;
		return PIR_DUPLICATE;
	}
	/* Track the level even during warm-up so the first real edge is correct. */
	f->level = level;
	if (ts_ns < f->warmup_until_ns) {
		f->suppressed++;
		return PIR_WARMUP;
	}
	f->forwarded++;
	return PIR_FORWARD;
}
