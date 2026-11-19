/* Audio detectors: impulse (door, latch) and voice against a learned room. */
#include "t.h"

#include "analysis/audio.h"

#include <math.h>
#include <string.h>

#define MS 1000000LL
#define WARMUP 250

/* A synthetic room: each band at its own level with +-0.05 (log10) jitter,
 * flux 0.3 +- 0.1, one frame every 20 ms. */
struct room {
	struct audio a;
	uint32_t st;
	int64_t t;
	unsigned impulses, voice_starts, voice_frames;
	int64_t last_impulse_ns;
	/* tuning reports */
	unsigned tune[AUDIO_TUNE_NO_DECAY + 1], voice_ticks;
	struct audio_result tune_imp, tune_voice;	/* the latest of each */
};

static const double base[PSS_NBANDS] = { -3.0, -3.2, -3.5, -3.7, -3.9, -4.1, -4.3, -4.5 };

static double unit(uint32_t *st)	/* uniform in [-1, 1] */
{
	*st = *st * 1103515245u + 12345u;
	return ((*st >> 8) & 0xffff) / 32767.5 - 1.0;
}

/* One frame: each band at base + up[b] (+ jitter), flux as given (< 0: room
 * flux). learn as the hub passes it. */
static void frame(struct room *r, const double up[PSS_NBANDS], double flux, int learn)
{
	float band[PSS_NBANDS];
	struct audio_result res;

	for (int b = 0; b < PSS_NBANDS; b++)
		band[b] = (float)(base[b] + (up ? up[b] : 0) + 0.05 * unit(&r->st));
	if (flux < 0)
		flux = 0.3 + 0.1 * unit(&r->st);
	r->t += 20 * MS;
	CHECK(audio_frame(&r->a, r->t, flux, band, learn, &res) == 0);
	if (res.flags & AUDIO_IMPULSE) {
		r->impulses++;
		r->last_impulse_ns = res.impulse_ns;
	}
	if (res.flags & AUDIO_VOICE_START)
		r->voice_starts++;
	if (res.flags & AUDIO_VOICE)
		r->voice_frames++;
	if (res.tune_imp != AUDIO_TUNE_NONE) {
		r->tune[res.tune_imp]++;
		r->tune_imp = res;
	}
	if (res.tune_voice) {
		r->voice_ticks++;
		r->tune_voice = res;
	}
	/* Tuning reports never disagree with the decision. */
	CHECK(!!(res.flags & AUDIO_IMPULSE) == (res.tune_imp == AUDIO_TUNE_YES));
}

static void quiet(struct room *r, int n)
{
	for (int i = 0; i < n; i++)
		frame(r, NULL, -1, 1);
}

static void counts_reset(struct room *r)
{
	r->impulses = r->voice_starts = r->voice_frames = 0;
	memset(r->tune, 0, sizeof r->tune);
	r->voice_ticks = 0;
}

/* A room that has learned its background for 10 s. */
static void room_init(struct room *r)
{
	memset(r, 0, sizeof *r);
	r->st = 12345;
	r->t = 1000 * MS;
	audio_init(&r->a, 6, 3, WARMUP);
	quiet(r, 500);
	counts_reset(r);
}

static const double click[PSS_NBANDS] = { 1.5, 1.8, 2.0, 2.0, 2.0, 2.0, 2.0, 1.8 };

static void test_nothing_before_warmup(void)
{
	struct room r;

	memset(&r, 0, sizeof r);
	r.st = 1;
	audio_init(&r.a, 6, 3, WARMUP);
	quiet(&r, 100);
	frame(&r, click, 8.0, 1);
	quiet(&r, 10);
	CHECK(r.impulses == 0);
}

static void test_steady_room_is_silent(void)
{
	struct room r;

	room_init(&r);
	quiet(&r, 3000);	/* a minute of fan */
	CHECK(r.impulses == 0);
	CHECK(r.voice_starts == 0);
}

/* Door latch: one loud broadband frame, then the room again. Reported once,
 * stamped with the loud frame's time, within 140 ms. */
static void test_door_click(void)
{
	struct room r;
	int64_t t_click;

	room_init(&r);
	frame(&r, click, 8.0, 1);
	t_click = r.t;
	quiet(&r, 1);
	CHECK(r.impulses == 1);
	CHECK(r.last_impulse_ns == t_click);
	quiet(&r, 50);
	CHECK(r.impulses == 1);
	CHECK(r.voice_starts == 0);

	/* Decays over a few frames (a door's ring): still within 140 ms. */
	frame(&r, click, 8.0, 1);
	t_click = r.t;
	for (int i = 1; i <= 6; i++) {
		double up[PSS_NBANDS];

		for (int b = 0; b < PSS_NBANDS; b++)
			up[b] = click[b] - 0.15 * i;	/* 1.5 dB per frame: 9 dB at 120 ms */
		frame(&r, up, 0.0, 1);
	}
	CHECK(r.impulses == 1);
	quiet(&r, 1);	/* 140 ms: down to the room */
	CHECK(r.impulses == 2);
	CHECK(r.last_impulse_ns == t_click);
}

/* Rising over two frames: the louder one is the impulse's time. */
static void test_click_peak_is_the_time(void)
{
	struct room r;
	double half[PSS_NBANDS];

	room_init(&r);
	for (int b = 0; b < PSS_NBANDS; b++)
		half[b] = click[b] / 2;
	frame(&r, half, 8.0, 1);
	frame(&r, click, 8.0, 1);
	quiet(&r, 2);
	CHECK(r.impulses == 1);
	CHECK(r.last_impulse_ns == r.t - 2 * 20 * MS);
}

/* A thump that doesn't die away (furniture dragged, a bang that rings on):
 * falls only 5 dB in 140 ms, so not an impulse. */
static void test_slow_decay_is_not_impulse(void)
{
	struct room r;
	double ring[PSS_NBANDS];

	room_init(&r);
	for (int b = 0; b < PSS_NBANDS; b++)
		ring[b] = click[b] - 0.5;
	frame(&r, click, 8.0, 1);
	for (int i = 0; i < 20; i++)
		frame(&r, ring, -1, 1);
	quiet(&r, 10);	/* falls later: too late */
	CHECK(r.impulses == 0);
}

/* A short beep (a microwave, a phone): sudden, and gone at once, but in 3
 * bands only (tone plus harmonics), so not broadband. Loud enough that the
 * overall level falls over 10 dB, so only the broadband rule rejects it. */
static void test_narrowband_is_not_impulse(void)
{
	struct room r;
	double up[PSS_NBANDS] = { 0 };

	room_init(&r);
	up[3] = up[4] = up[5] = 3.0;
	frame(&r, up, 8.0, 1);
	quiet(&r, 10);
	CHECK(r.impulses == 0);
}

/* A beep isn't an impulse, but it isn't room sound either: it must not be
 * learned, or its huge flux inflates the flux variance and a softer door
 * click (flux z ~12 in the plain room) right after it scores under k_imp. */
static void test_click_after_beep(void)
{
	struct room r;
	double up[PSS_NBANDS] = { 0 };
	uint64_t n;

	room_init(&r);
	up[3] = up[4] = up[5] = 3.0;
	n = r.a.n;
	frame(&r, up, 8.0, 1);
	CHECK(r.a.n == n);	/* not learned */
	quiet(&r, 50);
	frame(&r, click, 1.0, 1);
	quiet(&r, 1);
	CHECK(r.impulses == 1);
}

/* Getting louder slowly (a fan turned up over 10 s): no sudden change. */
static void test_slow_swell_is_not_impulse(void)
{
	struct room r;

	room_init(&r);
	for (int i = 0; i < 500; i++) {
		double up[PSS_NBANDS];

		for (int b = 0; b < PSS_NBANDS; b++)
			up[b] = 0.002 * i;
		frame(&r, up, -1, 1);
	}
	CHECK(r.impulses == 0);
}

/* Speech-like: syllables of 100 ms on, 40 ms off, speech bands +10 dB, a
 * little low-band energy. syllables x 5 frames sound like voice. */
static void talk(struct room *r, int syllables, int learn)
{
	static const double voice[PSS_NBANDS] = { 0.1, 0.4, 1.0, 1.0, 1.0, 1.0, 0.5, 0.2 };

	for (int s = 0; s < syllables; s++) {
		for (int i = 0; i < 5; i++)
			frame(r, voice, i == 0 ? 3.0 : -1, learn);
		for (int i = 0; i < 2; i++)
			frame(r, NULL, -1, learn);
	}
}

static void test_voice(void)
{
	struct room r;

	room_init(&r);
	talk(&r, 3, 1);		/* 15 frames: 300 ms of voice, not enough */
	CHECK(r.voice_starts == 0);
	talk(&r, 2, 1);		/* 25 within 2 s */
	CHECK(r.voice_starts == 1);
	CHECK(r.voice_frames > 0);
	CHECK(r.impulses == 0);	/* syllable onsets fall ~5 dB, not 10 */
	talk(&r, 20, 1);	/* keeps talking: still one start, voice every frame */
	CHECK(r.voice_starts == 1);
	counts_reset(&r);
	quiet(&r, 100);		/* 2 s of silence: voice is over... */
	CHECK(r.voice_frames < 100);
	counts_reset(&r);
	quiet(&r, 10);
	CHECK(r.voice_frames == 0);
	talk(&r, 6, 1);		/* ...so new talking starts again */
	CHECK(r.voice_starts == 1);
}

/* A cough: 100 ms of voice-band sound. */
static void test_cough_is_not_voice(void)
{
	struct room r;

	room_init(&r);
	talk(&r, 1, 1);
	quiet(&r, 100);
	CHECK(r.voice_starts == 0);
	/* Three coughs 1 s apart: 15 frames in 2 s, still not voice. */
	for (int i = 0; i < 3; i++) {
		talk(&r, 1, 1);
		quiet(&r, 43);
	}
	CHECK(r.voice_starts == 0);
}

/* HVAC: louder overall, but led by the lowest band. */
static void test_rumble_is_not_voice(void)
{
	static const double hvac[PSS_NBANDS] = { 2.0, 1.5, 1.0, 0.8, 0.6, 0.4, 0.2, 0.1 };
	struct room r;

	room_init(&r);
	for (int i = 0; i < 200; i++)
		frame(&r, hvac, -1, 0);	/* as if an event were open: never learned */
	CHECK(r.voice_starts == 0);
	CHECK(r.impulses == 0);
}

/* learn = 0 (an event is open) and impulse frames are never learned. */
static void test_learning(void)
{
	struct room r;
	uint64_t n;
	double mu;

	room_init(&r);
	n = r.a.n;
	mu = r.a.band_mu[3];
	talk(&r, 30, 0);
	CHECK(r.a.n == n);
	CHECK(r.a.band_mu[3] == mu);
	frame(&r, click, 8.0, 1);	/* candidate */
	quiet(&r, 1);			/* confirms (the decayed frame isn't learned either) */
	CHECK(r.impulses == 1);
	CHECK(r.a.n == n);
	quiet(&r, 1);
	CHECK(r.a.n == n + 1);
}

/* --tune reports: each sound that came close gets one verdict with the
 * reason, and a steady room reports nothing. */
static void test_tune_impulse_reasons(void)
{
	double up[PSS_NBANDS] = { 0 }, ring[PSS_NBANDS];
	struct room r;

	room_init(&r);
	quiet(&r, 3000);	/* a minute of room: nothing comes close */
	CHECK(r.tune[AUDIO_TUNE_YES] + r.tune[AUDIO_TUNE_NO_FLUX] + r.tune[AUDIO_TUNE_NO_BROAD] +
	      r.tune[AUDIO_TUNE_NO_DECAY] == 0);
	CHECK(r.voice_ticks == 0);

	frame(&r, click, 8.0, 1);	/* a door click */
	quiet(&r, 1);
	CHECK(r.tune[AUDIO_TUNE_YES] == 1);
	CHECK(r.tune_imp.tune_imp_bands == PSS_NBANDS);
	CHECK(r.tune_imp.tune_imp_z > 6);
	CHECK(r.tune_imp.tune_fall_db >= 10);
	CHECK(r.tune_imp.tune_fall_ms == 20);
	quiet(&r, 50);

	/* Broadband, but flux z only ~4: room flux is 0.3 +- ~0.06. */
	frame(&r, click, 0.55, 1);
	quiet(&r, 1);
	CHECK(r.tune[AUDIO_TUNE_NO_FLUX] == 1);
	CHECK(r.tune_imp.tune_imp_z >= 3 && r.tune_imp.tune_imp_z < 6);
	quiet(&r, 50);

	up[3] = up[4] = up[5] = 3.0;	/* a beep: 3 bands */
	frame(&r, up, 8.0, 1);
	quiet(&r, 1);
	CHECK(r.tune[AUDIO_TUNE_NO_BROAD] == 1);
	CHECK(r.tune_imp.tune_imp_bands == 3);
	quiet(&r, 50);

	for (int b = 0; b < PSS_NBANDS; b++)	/* a thump that rings on: falls ~5 dB */
		ring[b] = click[b] - 0.5;
	frame(&r, click, 8.0, 1);
	for (int i = 0; i < 10; i++)
		frame(&r, ring, -1, 1);
	CHECK(r.tune[AUDIO_TUNE_NO_DECAY] == 1);
	CHECK(r.tune_imp.tune_fall_db > 3 && r.tune_imp.tune_fall_db < 10);
	CHECK(r.tune[AUDIO_TUNE_YES] == 1 && r.impulses == 1);	/* only the door counted */
}

static void test_tune_voice_summary(void)
{
	static const double hvac[PSS_NBANDS] = { 2.0, 1.5, 1.0, 0.8, 0.6, 0.4, 0.2, 0.1 };
	struct room r;

	room_init(&r);
	talk(&r, 15, 0);	/* 105 frames: two full seconds of talking */
	CHECK(r.voice_ticks >= 2);
	CHECK(r.tune_voice.tune_vz_max > 15);
	CHECK(r.tune_voice.tune_low_z < r.tune_voice.tune_vz_max);
	CHECK(r.tune_voice.tune_counted >= 20);
	CHECK(r.tune_voice.tune_window >= AUDIO_VOICE_MIN);
	quiet(&r, 200);

	counts_reset(&r);
	for (int i = 0; i < 100; i++)	/* rumble: loud, but nothing counts */
		frame(&r, hvac, -1, 0);
	CHECK(r.voice_ticks == 2);
	CHECK(r.tune_voice.tune_low_z >= r.tune_voice.tune_vz_max);
	CHECK(r.tune_voice.tune_counted == 0);
	CHECK(r.voice_starts == 0);
}

static void test_non_finite_frame(void)
{
	struct room r;
	float band[PSS_NBANDS] = { 0 };
	struct audio_result res;
	uint64_t n;

	room_init(&r);
	n = r.a.n;
	CHECK(audio_frame(&r.a, r.t, NAN, band, 1, &res) == -1);
	band[2] = INFINITY;
	CHECK(audio_frame(&r.a, r.t, 0.3, band, 1, &res) == -1);
	CHECK(r.a.n == n);
}

int main(void)
{
	T_RUN(test_nothing_before_warmup);
	T_RUN(test_steady_room_is_silent);
	T_RUN(test_door_click);
	T_RUN(test_click_peak_is_the_time);
	T_RUN(test_slow_decay_is_not_impulse);
	T_RUN(test_narrowband_is_not_impulse);
	T_RUN(test_click_after_beep);
	T_RUN(test_slow_swell_is_not_impulse);
	T_RUN(test_voice);
	T_RUN(test_cough_is_not_voice);
	T_RUN(test_rumble_is_not_voice);
	T_RUN(test_learning);
	T_RUN(test_tune_impulse_reasons);
	T_RUN(test_tune_voice_summary);
	T_RUN(test_non_finite_frame);
	T_END();
}
