#ifndef PSS_CAPTURE_FEATURES_H
#define PSS_CAPTURE_FEATURES_H

/*
 * Per-frame audio features (DESIGN.md Section 4): 1024-point Hann window every
 * 20 ms (882 samples at 44.1 kHz); RMS, log energy in 8 log-spaced bands
 * from 100 Hz to 8 kHz, and spectral flux (sum of positive band increases).
 * Our own radix-2 FFT; no libraries.
 */

#include "common/ipc.h"

#include <stdint.h>

#define PSS_FFT_N 1024
#define PSS_HOP 882
#define PSS_FEAT_LO_HZ 100.0
#define PSS_FEAT_HI_HZ 8000.0

struct pss_features {
	float rms;		/* of the raw frame, full scale = 1.0 */
	float flux;		/* 0 on the first frame */
	float band[PSS_NBANDS];	/* log10 energy */
};

struct feat_state {
	float window[PSS_FFT_N];
	float cos_t[PSS_FFT_N / 2];
	float sin_t[PSS_FFT_N / 2];
	uint16_t band_lo[PSS_NBANDS];	/* FFT bins [lo, hi) */
	uint16_t band_hi[PSS_NBANDS];
	float prev[PSS_NBANDS];
	int have_prev;
};

/* Returns 0, or -1 if rate is too low for the 8 kHz top band. */
int feat_init(struct feat_state *s, unsigned rate);
void feat_compute(struct feat_state *s, const int16_t frame[PSS_FFT_N], struct pss_features *out);

#endif
