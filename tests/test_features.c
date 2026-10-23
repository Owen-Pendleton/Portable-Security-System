/* micd's feature extraction on synthetic signals (no microphone needed). */
#include "t.h"

#include "capture/mic/features.h"

#include <math.h>
#include <string.h>

#define RATE 44100

static void sine(int16_t *f, double hz, double amp)
{
	for (int i = 0; i < PSS_FFT_N; i++)
		f[i] = (int16_t)lround(amp * 32767.0 * sin(2.0 * M_PI * hz * i / RATE));
}

static int loudest_band(const struct pss_features *x)
{
	int best = 0;

	for (int b = 1; b < PSS_NBANDS; b++)
		if (x->band[b] > x->band[best])
			best = b;
	return best;
}

static void test_band_edges_cover_100hz_to_8khz(void)
{
	struct feat_state s;

	CHECK(feat_init(&s, RATE) == 0);
	CHECK(s.band_lo[0] == 2);	/* 100 Hz * 1024 / 44100 = 2.3 */
	CHECK(s.band_hi[PSS_NBANDS - 1] == 186); /* 8 kHz = 185.8 */
	for (int b = 1; b < PSS_NBANDS; b++)
		CHECK(s.band_lo[b] == s.band_hi[b - 1]);
	CHECK(feat_init(&s, 8000) == -1);
}

static void test_sine_lands_in_its_band(void)
{
	static struct feat_state s;
	struct pss_features x;
	int16_t f[PSS_FFT_N];

	CHECK(feat_init(&s, RATE) == 0);
	sine(f, 1000.0, 0.5); /* 1 kHz: band 4 = [894, 1546) Hz */
	feat_compute(&s, f, &x);
	CHECK(loudest_band(&x) == 4);
	CHECK(fabsf(x.rms - 0.3536f) < 0.01f);

	sine(f, 3000.0, 0.5); /* band 6 = [2675, 4627) Hz */
	feat_compute(&s, f, &x);
	CHECK(loudest_band(&x) == 6);

	sine(f, 130.0, 0.5); /* band 0 = [100, 173) Hz; 130 Hz sits on bin 3 */
	feat_compute(&s, f, &x);
	CHECK(loudest_band(&x) == 0);
}

static void test_flux_is_onset(void)
{
	static struct feat_state s;
	struct pss_features x;
	int16_t silence[PSS_FFT_N], loud[PSS_FFT_N];

	memset(silence, 0, sizeof silence);
	sine(loud, 1000.0, 0.8);
	CHECK(feat_init(&s, RATE) == 0);

	feat_compute(&s, silence, &x);
	CHECK(x.flux == 0.0f && x.rms == 0.0f);
	feat_compute(&s, silence, &x);
	CHECK(x.flux == 0.0f);
	feat_compute(&s, loud, &x);
	CHECK(x.flux > 5.0f);	/* onset: energy jumps by orders of magnitude */
	feat_compute(&s, loud, &x);
	CHECK(x.flux == 0.0f);	/* steady sound is not an onset */
	feat_compute(&s, silence, &x);
	CHECK(x.flux == 0.0f);	/* decay is not an onset either */
}

int main(void)
{
	T_RUN(test_band_edges_cover_100hz_to_8khz);
	T_RUN(test_sine_lands_in_its_band);
	T_RUN(test_flux_is_onset);
	T_END();
}
