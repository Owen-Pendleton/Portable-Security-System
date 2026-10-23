#include "capture/mic/features.h"

#include <math.h>
#include <string.h>

static unsigned hz_to_bin(double hz, unsigned rate)
{
	long b = lround(hz * PSS_FFT_N / rate);

	if (b < 1)
		b = 1;
	if (b > PSS_FFT_N / 2)
		b = PSS_FFT_N / 2;
	return (unsigned)b;
}

int feat_init(struct feat_state *s, unsigned rate)
{
	if (rate < 2 * PSS_FEAT_HI_HZ)
		return -1;
	memset(s, 0, sizeof *s);
	for (unsigned i = 0; i < PSS_FFT_N; i++)
		s->window[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / (PSS_FFT_N - 1)));
	for (unsigned k = 0; k < PSS_FFT_N / 2; k++) {
		s->cos_t[k] = (float)cos(2.0 * M_PI * k / PSS_FFT_N);
		s->sin_t[k] = (float)sin(2.0 * M_PI * k / PSS_FFT_N);
	}
	/* Edges f_b = 100 Hz * 80^(b/8): 100, 173, 299, 518, 894, 1546, 2675, 4627, 8000 Hz. */
	for (unsigned b = 0; b < PSS_NBANDS; b++) {
		double ratio = PSS_FEAT_HI_HZ / PSS_FEAT_LO_HZ;

		s->band_lo[b] = (uint16_t)hz_to_bin(PSS_FEAT_LO_HZ * pow(ratio, (double)b / PSS_NBANDS), rate);
		s->band_hi[b] = (uint16_t)hz_to_bin(PSS_FEAT_LO_HZ * pow(ratio, (double)(b + 1) / PSS_NBANDS), rate);
		if (s->band_hi[b] <= s->band_lo[b])
			s->band_hi[b] = (uint16_t)(s->band_lo[b] + 1);
	}
	return 0;
}

/* In-place iterative radix-2 FFT (forward, e^{-i 2 pi k n / N}). */
static void fft(float *re, float *im, const float *cos_t, const float *sin_t)
{
	const unsigned n = PSS_FFT_N;

	for (unsigned i = 1, j = 0; i < n; i++) {
		unsigned bit = n >> 1;

		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			float t = re[i];

			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}
	for (unsigned len = 2; len <= n; len <<= 1) {
		unsigned half = len / 2, step = n / len;

		for (unsigned i = 0; i < n; i += len) {
			for (unsigned k = 0; k < half; k++) {
				float wr = cos_t[k * step], wi = -sin_t[k * step];
				unsigned a = i + k, b = a + half;
				float xr = re[b] * wr - im[b] * wi;
				float xi = re[b] * wi + im[b] * wr;

				re[b] = re[a] - xr;
				im[b] = im[a] - xi;
				re[a] += xr;
				im[a] += xi;
			}
		}
	}
}

void feat_compute(struct feat_state *s, const int16_t frame[PSS_FFT_N], struct pss_features *out)
{
	float re[PSS_FFT_N], im[PSS_FFT_N];
	double sumsq = 0.0;

	for (unsigned i = 0; i < PSS_FFT_N; i++) {
		float x = frame[i] / 32768.0f;

		sumsq += (double)x * x;
		re[i] = x * s->window[i];
		im[i] = 0.0f;
	}
	out->rms = (float)sqrt(sumsq / PSS_FFT_N);

	fft(re, im, s->cos_t, s->sin_t);

	out->flux = 0.0f;
	for (unsigned b = 0; b < PSS_NBANDS; b++) {
		double e = 0.0;

		for (unsigned k = s->band_lo[b]; k < s->band_hi[b]; k++)
			e += (double)re[k] * re[k] + (double)im[k] * im[k];
		out->band[b] = (float)log10(e / PSS_FFT_N + 1e-10);
		if (s->have_prev && out->band[b] > s->prev[b])
			out->flux += out->band[b] - s->prev[b];
		s->prev[b] = out->band[b];
	}
	s->have_prev = 1;
}
