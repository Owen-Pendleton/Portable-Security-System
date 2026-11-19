#include "analysis/audio.h"

#include <math.h>
#include <string.h>

#define ALPHA_MIN (1.0 / 3000)		/* 60 s time constant at 50 frames/s */
#define BAND_VAR_FLOOR (0.05 * 0.05)	/* log10 energy: 0.5 dB */
#define FLUX_VAR_FLOOR (0.05 * 0.05)

void audio_init(struct audio *a, double k_imp, double k_voice, uint32_t warmup)
{
	memset(a, 0, sizeof *a);
	a->k_imp = k_imp;
	a->k_voice = k_voice;
	a->warmup = warmup;
}

static double zscore(double x, double mu, double var, double floor)
{
	return (x - mu) / sqrt(var > floor ? var : floor);
}

static void ewma(double *mu, double *var, double x, double alpha)
{
	double d = x - *mu;

	*mu += alpha * d;
	*var = (1.0 - alpha) * (*var + alpha * d * d);
}

static void learn_frame(struct audio *a, double flux, const float band[PSS_NBANDS])
{
	double alpha;

	a->n++;
	alpha = 1.0 / (double)a->n;
	if (alpha < ALPHA_MIN)
		alpha = ALPHA_MIN;
	ewma(&a->flux_mu, &a->flux_var, flux, alpha);
	for (int b = 0; b < PSS_NBANDS; b++)
		ewma(&a->band_mu[b], &a->band_var[b], band[b], alpha);
}

/* Returns 1 if this frame is (or continues) an impulse candidate, or is
 * sudden (flux z >= k_imp) at all, so it must not be learned. */
static int impulse_step(struct audio *a, int64_t ts_ns, double zf, int broad, double level,
			struct audio_result *r)
{
	int cand = zf >= a->k_imp && broad >= AUDIO_BROAD_MIN;

	if (a->imp_pending) {
		if (cand && level > a->imp_level) {	/* still rising: this is the peak */
			a->imp_ns = ts_ns;
			a->imp_level = a->imp_min_level = level;
			a->imp_z = zf;
			a->imp_bands = broad;
			a->imp_age = 0;
			return 1;
		}
		a->imp_age++;
		if (level < a->imp_min_level)
			a->imp_min_level = level;
		r->tune_imp_z = a->imp_z;
		r->tune_imp_bands = a->imp_bands;
		r->tune_fall_db = (a->imp_level - a->imp_min_level) * 10.0;
		if (level <= a->imp_level - AUDIO_DECAY_DB / 10.0) {
			a->imp_pending = 0;
			r->flags |= AUDIO_IMPULSE;
			r->impulse_ns = a->imp_ns;
			r->impulse_z = a->imp_z;
			r->tune_imp = AUDIO_TUNE_YES;
			r->tune_fall_ms = (int)a->imp_age * 20;
			return 1;
		}
		if (a->imp_age >= AUDIO_DECAY_FRAMES) {	/* too slow to fall: not a click */
			a->imp_pending = 0;
			r->tune_imp = AUDIO_TUNE_NO_DECAY;
		}
		return 1;
	}
	if (!cand) {
		if (zf >= a->k_imp / 2) {	/* came close: say why not */
			r->tune_imp = zf < a->k_imp ? AUDIO_TUNE_NO_FLUX : AUDIO_TUNE_NO_BROAD;
			r->tune_imp_z = zf;
			r->tune_imp_bands = broad;
		}
		/* Sudden but not broadband (a beep): not an impulse, but not
		 * learned either, so it can't inflate the flux variance. */
		return zf >= a->k_imp;
	}
	a->imp_pending = 1;
	a->imp_ns = ts_ns;
	a->imp_level = a->imp_min_level = level;
	a->imp_z = zf;
	a->imp_bands = broad;
	a->imp_age = 0;
	return 1;
}

/* Tuning: a summary of each second (50 frames) whose loudest speech-band
 * score reached AUDIO_TUNE_VOICE_Z. */
static void voice_tune(struct audio *a, int voicey, double vz, double low_z, struct audio_result *r)
{
	if (a->sec_frames == 0 || vz > a->sec_vz_max) {
		a->sec_vz_max = vz;
		a->sec_low_z = low_z;
	}
	a->sec_counted += (uint32_t)voicey;
	if (++a->sec_frames < 50)
		return;
	if (a->sec_vz_max >= AUDIO_TUNE_VOICE_Z) {
		r->tune_voice = 1;
		r->tune_vz_max = a->sec_vz_max;
		r->tune_low_z = a->sec_low_z;
		r->tune_counted = a->sec_counted;
		r->tune_window = a->voice_count;
	}
	a->sec_frames = a->sec_counted = 0;
}

static void voice_step(struct audio *a, int voicey, struct audio_result *r)
{
	a->voice_count -= a->voice_win[a->voice_pos];
	a->voice_win[a->voice_pos] = (uint8_t)voicey;
	a->voice_count += (uint32_t)voicey;
	a->voice_pos = (a->voice_pos + 1) % AUDIO_VOICE_WIN;
	if (a->voice_count < AUDIO_VOICE_MIN) {
		a->voice_active = 0;
		return;
	}
	if (!a->voice_active)
		r->flags |= AUDIO_VOICE_START;
	a->voice_active = 1;
	r->flags |= AUDIO_VOICE;
}

int audio_frame(struct audio *a, int64_t ts_ns, double flux, const float band[PSS_NBANDS], int learn,
		struct audio_result *r)
{
	double zb[PSS_NBANDS], zf, level = 0, vz = 0;
	int broad = 0, transient, voicey;

	memset(r, 0, sizeof *r);
	if (!isfinite(flux))
		return -1;
	for (int b = 0; b < PSS_NBANDS; b++)
		if (!isfinite(band[b]))
			return -1;
	if (a->n < a->warmup) {
		if (learn)
			learn_frame(a, flux, band);
		return 0;
	}

	zf = zscore(flux, a->flux_mu, a->flux_var, FLUX_VAR_FLOOR);
	for (int b = 0; b < PSS_NBANDS; b++) {
		zb[b] = zscore(band[b], a->band_mu[b], a->band_var[b], BAND_VAR_FLOOR);
		if (zb[b] >= AUDIO_BAND_Z)
			broad++;
		level += band[b];
	}
	level /= PSS_NBANDS;
	for (int b = AUDIO_VOICE_LO; b <= AUDIO_VOICE_HI; b++)
		vz += zb[b];
	vz /= AUDIO_VOICE_HI - AUDIO_VOICE_LO + 1;
	r->voice_z = vz;

	transient = impulse_step(a, ts_ns, zf, broad, level, r);
	voicey = vz >= a->k_voice && zb[0] < vz;
	voice_step(a, voicey, r);
	voice_tune(a, voicey, vz, zb[0], r);

	if (learn) {
		if (zf > a->imp_zmax)
			a->imp_zmax = zf;
		if (vz > a->voice_zmax)
			a->voice_zmax = vz;
		if (!transient)
			learn_frame(a, flux, band);
	}
	return 0;
}
