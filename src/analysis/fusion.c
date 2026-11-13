#include "analysis/fusion.h"

#include <string.h>

void fusion_init(struct fusion *f, int64_t t_co_ns)
{
	memset(f, 0, sizeof *f);
	f->t_co_ns = t_co_ns;
}

static int within(const struct fusion *f, int64_t onset_ns, int64_t rise_ns, int64_t *dt_ns)
{
	int64_t dt = onset_ns - rise_ns;

	if (dt < -f->t_co_ns || dt > f->t_co_ns)
		return 0;
	if (dt_ns != NULL)
		*dt_ns = dt;
	return 1;
}

int fusion_rise(struct fusion *f, int64_t ts_ns, int64_t *dt_ns)
{
	f->last_rise_ns = ts_ns;
	f->have_rise = 1;
	return f->have_onset && within(f, f->last_onset_ns, ts_ns, dt_ns);
}

int fusion_onset(struct fusion *f, int64_t ts_ns, int64_t *dt_ns)
{
	f->last_onset_ns = ts_ns;
	f->have_onset = 1;
	return f->have_rise && within(f, ts_ns, f->last_rise_ns, dt_ns);
}

void fusion_forget(struct fusion *f, int pir, int mic)
{
	if (pir)
		f->have_rise = 0;
	if (mic)
		f->have_onset = 0;
}
