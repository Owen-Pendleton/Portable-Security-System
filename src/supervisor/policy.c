#include "supervisor/policy.h"

#include "common/time.h"

#include <string.h>

void sup_policy_default(struct sup_policy *p)
{
	p->backoff_min_ns = 1 * PSS_NS_PER_S;
	p->backoff_max_ns = 60 * PSS_NS_PER_S;
	p->healthy_ns = 600 * PSS_NS_PER_S;
	p->gone_retry_ns = 30 * PSS_NS_PER_S;
	p->hb_timeout_ns = 15 * PSS_NS_PER_S;
	p->first_hb_ns = 60 * PSS_NS_PER_S;
}

void sup_track_init(const struct sup_policy *p, struct sup_track *t)
{
	memset(t, 0, sizeof *t);
	t->backoff_ns = p->backoff_min_ns;
}

void sup_track_started(struct sup_track *t, int64_t now_ns)
{
	t->started_ns = now_ns;
	t->have_hb = 0;
}

void sup_track_beat(struct sup_track *t, int64_t now_ns)
{
	t->last_hb_ns = now_ns;
	t->have_hb = 1;
}

int sup_hung(const struct sup_policy *p, const struct sup_track *t, int64_t now_ns)
{
	if (!t->have_hb)
		return now_ns - t->started_ns >= p->first_hb_ns;
	return now_ns - t->last_hb_ns >= p->hb_timeout_ns;
}

int64_t sup_restart_delay(const struct sup_policy *p, struct sup_track *t, int64_t now_ns, int exit_code)
{
	int64_t d;

	if (now_ns - t->started_ns >= p->healthy_ns)
		t->backoff_ns = p->backoff_min_ns;
	if (exit_code == SUP_EXIT_DEVICE_GONE)
		return p->gone_retry_ns;
	d = t->backoff_ns;
	t->backoff_ns = d * 2 > p->backoff_max_ns ? p->backoff_max_ns : d * 2;
	return d;
}
