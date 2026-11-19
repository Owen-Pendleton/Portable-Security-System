#ifndef PSS_ANALYSIS_AUDIO_H
#define PSS_ANALYSIS_AUDIO_H

/*
 * Audio detectors (DESIGN.md Section 4): a per-room background model and
 * two detectors on top of it, fed one micd frame (20 ms) at a time.
 *
 * Background: per band (8 log-spaced bands, log10 energy) and for spectral
 * flux, an exponentially weighted mean and variance with a ~60 s time
 * constant. Frames are scored as z-scores against it. The caller decides
 * when it learns (only while no event is open), and frames that are part of
 * an impulse, or sudden at all (flux z >= k_imp, e.g. a beep), are never
 * learned.
 *
 * Impulse (latch, door): flux z >= k_imp, broadband (at least
 * AUDIO_BROAD_MIN of the 8 bands at z >= AUDIO_BAND_Z), and the mean band
 * level falls at least AUDIO_DECAY_DB within AUDIO_DECAY_FRAMES (140 ms).
 * Reported once the decay is seen, stamped with the loud frame's time.
 *
 * Voice: a frame "sounds like voice" when the mean z of the speech bands
 * (2-5, 299-2675 Hz) is >= k_voice and the lowest band (0, 100-173 Hz;
 * micd measures nothing below 100 Hz) rose less than the speech bands, so
 * a low rumble (HVAC) doesn't count. Voice is detected while at least
 * AUDIO_VOICE_MIN such frames (400 ms) lie within the last AUDIO_VOICE_WIN
 * frames (2 s).
 *
 * Pure logic, no I/O, so it is testable off the Pi.
 */

#include <stdint.h>

#include "common/ipc.h"	/* PSS_NBANDS */

#define AUDIO_BAND_Z 3.0
#define AUDIO_BROAD_MIN 5
#define AUDIO_DECAY_DB 10.0
#define AUDIO_DECAY_FRAMES 7
#define AUDIO_VOICE_LO 2
#define AUDIO_VOICE_HI 5	/* inclusive */
#define AUDIO_VOICE_WIN 100
#define AUDIO_VOICE_MIN 20

enum {
	AUDIO_IMPULSE = 1,	/* an impulse was confirmed (impulse_ns, impulse_z) */
	AUDIO_VOICE_START = 2,	/* voice just became detected */
	AUDIO_VOICE = 4,	/* voice is detected (every frame while it is) */
};

/*
 * Tuning (hub --tune logs these; they never change a decision). A sound
 * "comes close" when its flux z reaches k_imp / 2; a second of sound comes
 * close when its largest speech-band z reaches AUDIO_TUNE_VOICE_Z (above
 * the jitter of a steady room, so a quiet room logs nothing).
 */
#define AUDIO_TUNE_VOICE_Z 2.5
enum audio_tune_imp {
	AUDIO_TUNE_NONE = 0,
	AUDIO_TUNE_YES,		/* confirmed impulse */
	AUDIO_TUNE_NO_FLUX,	/* flux z below k_imp */
	AUDIO_TUNE_NO_BROAD,	/* fewer than AUDIO_BROAD_MIN bands jumped */
	AUDIO_TUNE_NO_DECAY,	/* didn't fall AUDIO_DECAY_DB within 140 ms */
};

struct audio_result {
	unsigned flags;
	int64_t impulse_ns;	/* time of the loud frame */
	double impulse_z;	/* its flux z */
	double voice_z;		/* this frame's speech-band mean z */
	/* impulse verdict on a sound that came close (else AUDIO_TUNE_NONE) */
	enum audio_tune_imp tune_imp;
	double tune_imp_z;	/* flux z of the loud frame */
	int tune_imp_bands;	/* bands at z >= AUDIO_BAND_Z */
	double tune_fall_db;	/* YES: fall when confirmed; NO_DECAY: most it fell */
	int tune_fall_ms;	/* YES: how soon it fell */
	/* once per second (50 frames) of sound that came close */
	int tune_voice;
	double tune_vz_max;	/* largest speech-band mean z in that second */
	double tune_low_z;	/* band 0's z in that same frame */
	uint32_t tune_counted;	/* frames that counted as voice in that second */
	uint32_t tune_window;	/* frames counted in the last 2 s (needs AUDIO_VOICE_MIN) */
};

struct audio {
	double k_imp, k_voice;
	uint32_t warmup;	/* frames learned before anything is reported */
	uint64_t n;		/* frames learned */
	double band_mu[PSS_NBANDS], band_var[PSS_NBANDS];
	double flux_mu, flux_var;
	/* impulse waiting to see its decay */
	int imp_pending;
	uint32_t imp_age;
	int64_t imp_ns;
	double imp_level, imp_z, imp_min_level;
	int imp_bands;
	/* voice: 1 per frame that sounded like voice, last AUDIO_VOICE_WIN frames */
	uint8_t voice_win[AUDIO_VOICE_WIN];
	uint32_t voice_pos, voice_count;
	int voice_active;
	/* tuning: the current second */
	uint32_t sec_frames, sec_counted;
	double sec_vz_max, sec_low_z;
	/* largest scores seen while learning, for tuning; reset by the caller */
	double imp_zmax, voice_zmax;
};

void audio_init(struct audio *a, double k_imp, double k_voice, uint32_t warmup);
/*
 * One frame (band[] is log10 energy, as micd sends). learn: 1 if the
 * background may learn from it (no event open). Fills *r. Returns 0, or -1
 * (frame ignored, nothing changed) if any value is not finite.
 */
int audio_frame(struct audio *a, int64_t ts_ns, double flux, const float band[PSS_NBANDS], int learn,
		struct audio_result *r);

#endif
