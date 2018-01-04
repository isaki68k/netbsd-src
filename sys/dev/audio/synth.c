#define _USE_MATH_DEFINES
#include <math.h>
#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include "auring.h"

void __noreturn
synth_error(char *msg)
{
	exit(1);
}

/*
 基準音 O4A
 */

double O4A_freq = 440.0;

/* 12平均律 */
double
temperament_EQ12(double key)
{
	return pow(2, key / 12);
}

typedef double (*temperment_func)(double key);

temperment_func temperament = temperament_EQ12;

static
void
gen_sin(audio_ring_t *dst)
{
	if (dst->fmt.channels != 1) synth_error("ch != 1");
	if (!is_SIGNED(&dst->fmt)) synth_error("!SLINEAR");

	if (dst->fmt.stride == 8) {
		int8_t *dptr = dst->sample;
		for (int i = 0; i < dst->capacity; i++) {
			*dptr++ = (int8_t)((sin(2 * M_PI * i / dst->capacity) + 1) * 256 / 2 - 128);
		}
	} else if (dst->fmt.stride == 16) {
		int16_t *dptr = dst->sample;
		for (int i = 0; i < dst->capacity; i++) {
			*dptr++ = (int16_t)((sin(2 * M_PI * i / dst->capacity) + 1) * 65536 / 2 - 32768);
		}
	} else if (dst->fmt.stride == 32) {
		int32_t *dptr = dst->sample;
		for (int i = 0; i < dst->capacity; i++) {
			double t = sin(2 * M_PI * i / dst->capacity);
//			*dptr++ = (int32_t)((t + 1) * 4294967296.0 / 2 - 2147483648.0);
			*dptr++ = (int32_t)(t * 2147483647);
		}
	} else {
		synth_error("NotImpl");
	}
	dst->top = 0;
	dst->count = dst->capacity;
}

int32_t
tone_read(audio_ring_t *src, int start, int end)
{
#if 0
	int32_t rv = 0;
	int64_t absmax = 0;

	int32_t *sptr = RING_PTR(int32_t, src, start);
	int32_t *ecap = RING_END_PTR(int32_t, src);
	int step = src->fmt->channels;

	for (int i = start; i < end; i++) {
		int64_t s = (int64_t)*sptr;
		if (s < 0) s = -s;
		if (absmax < s) {
			absmax = s;
			rv = *sptr;
		}
		sptr += step;
		if (sptr > ecap) {
			sptr -= src->capacity * step;
		}
	}
	return rv;
#else
	return *RING_PTR(int32_t, src, (start + end) / 2);
#endif
}

void
play_note(audio_ring_t *dst, int count, audio_ring_t *tone, double factor, int volume)
{
	if (factor < 1) synth_error("factor < 1");

	if (dst->capacity - dst->count < count) synth_error("buffer overflow");

	double v0 = (double)volume / 256;
	double v = 0;
	double t = 0;
	int slice = 0;
	for (int remain = count; remain > 0; remain -= slice) {
		slice = min(remain, audio_ring_unround_free_count(dst));
		if (dst->fmt.stride == 16) {
			int16_t *dptr = RING_BOT(int16_t, dst);
			for (int i = 0; i < slice; i++) {
				int32_t x = (int32_t)(tone_read(tone, (int)t, (int)(t + factor)) * v);
				*dptr++ = (int16_t)(x >> 16);
				t += factor;
				if (t >= tone->count) t -= tone->count;

				if (i < 4410) {
					v = v0 * i / 4410;
				} else if (i < slice - 4410) {
					v = v0;
				} else {
					v = v0 * (slice - i) / 4410;
				}
			}
		} else if (dst->fmt.stride == 32) {
			int32_t *dptr = RING_BOT(int32_t, dst);
			for (int i = 0; i < slice; i++) {
				int32_t x = (int32_t)(tone_read(tone, (int)t, (int)(t + factor)) * v);
				*dptr++ = x;
				t += factor;
				if (t >= tone->count) t -= tone->count;
				v = v;// *0.9999;
			}
		}
		audio_ring_appended(dst, slice);
	}
}

int
char_to_basekey(char c)
{
	int ch = toupper((int)c);
	switch (ch) {
	case 'A': return 0 + 12;
	case 'B': return 2 + 12;
	case 'C': return 3;
	case 'D': return 5;
	case 'E': return 7;
	case 'F': return 8;
	case 'G': return 10;

	case 'R': return 0;
	default: return -1;
	}
}

double
half_scale(char **sptr, int base_key)
{
	double rv = base_key;
	double half = 1;
	char *c = *sptr;
	while (*c) {
		if (*c == '+' || *c == '#') {
			rv += half;
		} else if (*c == '-') {
			rv -= half;
		} else {
			break;
		}
		half /= 2;
		c++;
	}
	*sptr = c;
	return rv;
}

int tempo = 120;
int volume = 256;
int octarb = 4;
int note_len = 4;

int getnum(char **str)
{
	char *ep;
	long num = strtol(*str, &ep, 10);
	if (*str == ep) num = -1;
	*str = ep;
	return num;
}

void
ring_expand(audio_ring_t *ring, int newcapacity)
{
	ring->sample = realloc(ring->sample, newcapacity * ring->fmt.channels * ring->fmt.stride / 8);
	int newfree = newcapacity - ring->capacity;
	int unround = ring->capacity == 0 ? 0 : audio_ring_unround_count(ring);
	int round = ring->count - unround;
	int bounce = min(round, newfree);
	int move = round - bounce;

	if (bounce > 0) {
		memcpy(
			(int8_t*)ring->sample + ring->capacity * ring->fmt.channels * ring->fmt.stride / 8,
			(int8_t*)ring->sample,
			bounce * ring->fmt.channels * ring->fmt.stride / 8);
	}
	if (move > 0) {
		memmove(
			(int8_t*)ring->sample,
			(int8_t*)ring->sample + bounce * ring->fmt.channels * ring->fmt.stride / 8,
			move * ring->fmt.channels * ring->fmt.stride / 8);
	}
	ring->capacity = newcapacity;
}

void
play_mml(audio_ring_t *dst, char *mml)
{
	audio_ring_t tone0;
	audio_format2_t tone_fmt0;
	audio_ring_t *tone;

	tone_fmt0.channels = 1;
	tone_fmt0.encoding = AUDIO_ENCODING_SLINEAR_NE;
	tone_fmt0.sample_rate = 44100;
	tone_fmt0.precision = 32;
	tone_fmt0.stride = 32;

	tone = &tone0;
	tone->fmt = tone_fmt0;
	tone->capacity = tone->fmt.sample_rate * tone->fmt.channels * tone->fmt.stride / 8;
	tone->sample = malloc(RING_BYTELEN(tone));

	gen_sin(tone);

	while (*mml) {
		int c = toupper((int)*mml);
		if (c == 'T') {
			mml++;
			int t = getnum(&mml);
			if (t <= 0) synth_error("T");
			tempo = t;
		} else if (c == 'V') {
			mml++;
			int v = getnum(&mml);
			if (v < 0) synth_error("V");
			if (v > 256) v = 256;
			volume = v;
		} else if (c == 'L') {
			mml++;
			int l = getnum(&mml);
			if (l <= 0) synth_error("L");
			note_len = l;
		} else if (c == 'O') {
			mml++;
			int o = getnum(&mml);
			octarb = o;
		} else {
			int basekey = char_to_basekey(c);
			if (basekey >= 0) {
				basekey -= 12;
				basekey += octarb * 12;
				mml++;
				double key = half_scale(&mml, basekey);
				int l = getnum(&mml);
				if (l <= 0) l = note_len;
				l = dst->fmt.sample_rate * 60 * 4 / tempo / l;

				int v = volume;
				if (c == 'R') v = 0;

				if (dst->capacity - dst->count < l) {
					ring_expand(dst, dst->capacity + l * 2);
				}
				play_note(dst, l, tone, 44100.0 / O4A_freq * temperament(key), v);
			}
		}
	}
}
