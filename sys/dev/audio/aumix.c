#include "aumix.h"
#include <errno.h>
#include <stdlib.h>
#include <memory.h>
#include <stdbool.h>
#include "auring.h"
#include "audev.h"
#include "aucodec.h"
#include "auformat.h"
#include "uio.h"

int
gcd(int a, int b)
{
	int t;
	if (a == b) return a;
	if (a < b) {
		t = a;
		a = b;
		b = t;
	}

	while (b > 0) {
		t = b;
		b = a % b;
		a = t;
	}
	return a;
}

/* メモリアロケーションの STUB */

void *
audio_realloc(void *memblock, size_t bytes)
{
	if (memblock != NULL) {
		if (bytes != 0) {
			return realloc(memblock, bytes);
		} else {
			free(memblock);
			return NULL;
		}
	} else {
		if (bytes != 0) {
			return malloc(bytes);
		} else {
			return NULL;
		}
	}
}

void *
audio_free(void *memblock)
{
	if (memblock != NULL) {
		free(memblock);
	}
	return NULL;
}


/*
 * ***** audio_rational *****
 */

/* r = 0 */
static inline void
audio_rational_clear(audio_rational_t *r)
{
	KASSERT(r != NULL);
	r->i = 0;
	r->n = 0;
}

/* 共通分母 d の正規化された帯分数 r, a に対し、 r += a を実行し、結果の整数部を返します。 */
static inline int
audio_rational_add(audio_rational_t *r, audio_rational_t *a, int d)
{
	KASSERT(r != NULL);
	KASSERT(a != NULL);
	KASSERT(d != 0);
	KASSERT(r->n < d);
	KASSERT(a->n < d);

	r->i += a->i;
	r->n += a->n;
	if (r->n >= d) {
		r->i += 1;
		r->n -= d;
	}
	return r->i;
}

/* a > b なら + 、a == b なら 0 , a < b なら - を返します。*/
static inline int
audio_rational_cmp(audio_rational_t *a, audio_rational_t *b)
{
	int r = a->i - b->i;
	if (r == 0) {
		r = a->n - b->n;
	}
	return r;
}

/*
 * audio_volume
 */

 /* ユーザランドなどで使用される 0.255 ボリュームを、トラック内の 0..256 ボリュームに変換します。 */
int16_t
audio_volume_to_inner(uint8_t v)
{
	return v < 127 ? v : (int16_t)v + 1;
}

/* トラック内の 0..256 ボリュームを、外部で使用される 0..255 ボリュームに変換します。 */
uint8_t
audio_volume_to_outer(int16_t v)
{
	return v < 127 ? v : v - 1;
}


/*
 * ***** audio_track *****
 */

audio_format_t default_format = {
	AUDIO_ENCODING_MULAW,
	8000, /* freq */
	1, /* channels */
	8, /* precision */
	8, /* stride */
};

void
audio_track_init(audio_track_t *track, audio_trackmixer_t *mixer)
{
	memset(track, 0, sizeof(audio_track_t));
	track->mixer = mixer;

	/* userio_buf はユーザフォーマット。 */
	track->userio_fmt = default_format;
	track->userio_buf.fmt = &track->userio_fmt;

	/* enconvert_buf は内部フォーマットだが userio 周波数, userio チャンネル */
	track->enconvert_fmt = mixer->track_fmt;
	track->enconvert_buf.fmt = &track->enconvert_fmt;

	/* step2 は内部フォーマットだが userio 周波数と処理用チャンネル */
	track->chmix_fmt = mixer->track_fmt;
	track->chmix_buf.fmt = &track->chmix_fmt;

	/* track_buf は内部フォーマット */
	track->track_buf.fmt = &mixer->track_fmt;
	track->track_buf.top = 0;
	track->track_buf.count = 0;
	track->track_buf.capacity = 16 * mixer->frames_per_block;
	track->track_buf.sample = audio_realloc(track->track_buf.sample, RING_BYTELEN(&track->track_buf));

	audio_track_set_format(track, &default_format);

	track->volume = 256;
	track->mixed_count = 0;
}

static inline int
framecount_roundup_byte_boundary(int framecount, int stride)
{
	/* stride が、、、 */
	if ((stride & 7) == 0) {
		/* 8 の倍数なのでそのままでいい */
		return framecount;
	} else if ((stride & 3) == 0) {
		/* 4 の倍数なので framecount が奇数なら +1 して 2 の倍数を返す */
		return framecount + (framecount & 1);
	} else if ((stride & 1) == 0) {
		/* 2 の倍数なので {0, 3, 2, 1} を足して 4 の倍数を返す */
		return framecount + ((4 - (framecount & 3)) & 3);
	} else {
		/* 8 とは互いに素なので {0,7,6,5,4,3,2,1} を足して 8 の倍数を返す */
		return framecount + ((8 - (framecount & 7)) & 7);
	}
}

/*
* トラックのユーザランド側フォーマットを設定します。
* 変換用内部バッファは一度破棄されます。
*/
void
audio_track_set_format(audio_track_t *track, audio_format_t *fmt)
{
	KASSERT(is_valid_format(fmt));

	audio_format_t *track_fmt = track->track_buf.fmt;

	track->userio_fmt = *fmt;

	/* ブロック境界がバイト境界になるように、1ブロックのフレーム数を調整する */
	track->userio_frames_per_block = framecount_roundup_byte_boundary(fmt->frequency * AUDIO_BLOCK_msec / 1000, fmt->stride);

	track->userio_buf.top = 0;
	track->userio_buf.capacity = track->userio_frames_per_block;
	track->userio_mem = audio_realloc(track->userio_mem, RING_BYTELEN(&track->userio_buf));
	track->userio_buf.sample = track->userio_mem;


	if (fmt->encoding == track_fmt->encoding
		&& fmt->precision == track_fmt->precision
		&& fmt->stride == track_fmt->stride) {
		track->enconvert_mode = AUDIO_TRACK_ENCONVERT_THRU;
		track->step1 = &track->userio_buf;
	} else {
		if (fmt->stride == track_fmt->stride) {
			track->enconvert_mode = AUDIO_TRACK_ENCONVERT_INLINE;
		} else {
			track->enconvert_mode = AUDIO_TRACK_ENCONVERT_BUFFER;
		}
		track->step1 = &track->enconvert_buf;
	}

	if (fmt->channels == track_fmt->channels) {
		track->chmix_mode = AUDIO_TRACK_CHANNEL_INLINE;
	} else {
		track->chmix_mode = AUDIO_TRACK_CHANNEL_BUFFER;
		if (fmt->channels >= 2 && track_fmt->channels == 1) {
			if (track->channelmix_all) {
				track->chmix_mode |= AUDIO_TRACK_CHANNEL_MIXALL;
			} else {
				track->chmix_mode |= AUDIO_TRACK_CHANNEL_MIXLR;
			}
		} else if (fmt->channels == 1 && track_fmt->channels >= 2) {
			if (track->channelmix_all) {
				track->chmix_mode |= AUDIO_TRACK_CHANNEL_DUPALL;
			} else {
				track->chmix_mode |= AUDIO_TRACK_CHANNEL_DUPLR;
			}
		} else if (fmt->channels > track_fmt->channels) {
			track->chmix_mode |= AUDIO_TRACK_CHANNEL_SHRINK;
		} else {
			track->chmix_mode |= AUDIO_TRACK_CHANNEL_EXPAND;
		}
	}

	for (int ch = 0; ch < fmt->channels; ch++) {
		if (track->ch_volume[ch] != 256) {
			track->chmix_mode |= AUDIO_TRACK_CHANNEL_VOLUME;
			break;
		}
	}

	track->step2 = &track->chmix_buf;
	if (track->chmix_mode == AUDIO_TRACK_CHANNEL_INLINE) {
		/* INLINE しか立って無くてほかの変換がない */
		track->chmix_mode = AUDIO_TRACK_CHANNEL_THRU;
		track->step2 = track->step1;
	}

	track->freq_tmp = NULL;
	if (fmt->frequency == track_fmt->frequency) {
		track->freq_mode = AUDIO_TRACK_FREQ_THRU;
	} else {
		if (track->enconvert_mode == AUDIO_TRACK_ENCONVERT_BUFFER
			|| (track->chmix_mode & AUDIO_TRACK_CHANNEL_BUFFER)) {
			track->freq_mode = AUDIO_TRACK_FREQ_BUFFER;
		} else {
			track->freq_mode = AUDIO_TRACK_FREQ_INLINE;
			track->freq_tmp = &track->chmix_buf;
		}
	}

	if (track->volume == 256) {
		track->volume_mode = AUDIO_TRACK_VOLUME_THRU;
	} else {
		track->volume_mode = AUDIO_TRACK_VOLUME_INLINE;
	}

	/* enconvert_buf は内部フォーマットだが userio 周波数, userio チャンネル */
	track->enconvert_fmt.frequency = fmt->frequency;
	track->enconvert_fmt.channels = fmt->channels;
	track->enconvert_buf.top = 0;
	track->enconvert_buf.count = 0;
	if (track->enconvert_mode == AUDIO_TRACK_ENCONVERT_BUFFER) {
		/* バッファメモリは必要な場合のみ */
		track->enconvert_buf.capacity = 1 * track->userio_frames_per_block;
		track->enconvert_mem = audio_realloc(track->enconvert_mem, RING_BYTELEN(&track->enconvert_buf));
	} else {
		track->enconvert_buf.capacity = 0;
		track->enconvert_mem = audio_free(track->enconvert_mem);
	}
	track->enconvert_buf.sample = track->enconvert_mem;

	/* chmix_buf は内部フォーマットだが userio 周波数 */
	track->chmix_fmt.frequency = fmt->frequency;
	track->chmix_buf.top = 0;
	track->chmix_buf.count = 0;
	if ((track->chmix_mode & AUDIO_TRACK_CHANNEL_BUFFER)
		|| (track->freq_mode == AUDIO_TRACK_FREQ_INLINE)) {
		/* チャンネル変換または周波数変換が必要なら */
		track->chmix_buf.capacity =  1 * track->userio_frames_per_block;
		track->chmix_mem = audio_realloc(track->chmix_mem, RING_BYTELEN(&track->chmix_buf));
	} else {
		track->chmix_buf.capacity = 0;
		track->chmix_mem = audio_free(track->chmix_mem);
	}
	track->chmix_buf.sample = track->chmix_mem;
	

	/* TODO: PLAY OR REC */
	track->freq_step.i = fmt->frequency / track->mixer->mix_fmt.frequency;
	track->freq_step.n = fmt->frequency % track->mixer->mix_fmt.frequency;
	audio_rational_clear(&track->freq_current);

	track->codec_arg.src_fmt = &track->userio_fmt;
	track->codec_arg.dst_fmt = &track->enconvert_fmt;

	track->codec = audio_MI_codec_filter_init(&track->codec_arg);


	if (track->enconvert_mode == AUDIO_TRACK_ENCONVERT_INLINE) {
		track->step1->capacity = track->userio_buf.capacity;
		track->step1->count = 0;
		track->step1->top = track->userio_buf.top;
		track->step1->sample = track->userio_buf.sample;
	}

	if (track->chmix_mode & AUDIO_TRACK_CHANNEL_INLINE) {
		track->step2->capacity = track->step1->capacity;
		track->step2->count = 0;
		track->step2->top = track->step2->top;
		track->step2->sample = track->step2->sample;
	}


	if (debug) {
		printf("%s: userfmt=%s\n", __func__, fmt_tostring(&track->userio_fmt));
		printf(" fmtmode=%d chmix=0x%x freqmode=%d\n",
			track->enconvert_mode,
			track->chmix_mode,
			track->freq_mode);
	}
}

void
audio_track_lock(audio_track_t *track)
{
	// STUB
}

void
audio_track_unlock(audio_track_t *track)
{
	// STUB
}

/*
 src のエンコーディングを変換して dst に投入します。
*/
void
audio_track_enconvert(audio_track_t *track, audio_filter_t filter, audio_ring_t *dst, audio_ring_t *src)
{
	KASSERT(track != NULL);
	KASSERT(filter != NULL);
	KASSERT(is_valid_ring(dst));
	KASSERT(is_valid_ring(src));

	if (src->count <= 0) return;
	int count = src->count;

	/* 今回変換したいフレーム数 */
	if (track->is_draining) {
		/* ドレイン中は出来る限り実行 */
		count = min(count, (dst->capacity - dst->count));
	} else {
		/* 通常時は 1 ブロック */
		count = min(count, track->userio_frames_per_block);
	}
	if (count <= 0) return;

	/* 空きがない */
	if (dst->capacity - dst->count < count) return;

	audio_filter_arg_t *arg = &track->codec_arg;

	int slice_count = 0;
	for (int remain_count = count; remain_count > 0; remain_count -= slice_count) {
		int dst_count = audio_ring_unround_free_count(dst);
		KASSERT(dst_count != 0);

		int src_count = audio_ring_unround_count(src);
		slice_count = min(remain_count, src_count);
		slice_count = min(slice_count, dst_count);
		KASSERT(slice_count > 0);

		arg->dst = RING_BOT_UINT8(dst);
		arg->src = RING_TOP_UINT8(src);
		arg->count = slice_count;
		int done_count = filter(arg);

		/* このフレーム数では変換できない */
		if (done_count == 0) {
			// ring 再構成とか。
			break;
		}
		slice_count = done_count;
		audio_ring_tookfromtop(src, done_count);
		audio_ring_appended(dst, done_count);
	}
}

void
audio_track_channel_mix(audio_track_t *track, audio_ring_t *dst, audio_ring_t *src)
{
	KASSERT(track != NULL);
	KASSERT(is_valid_ring(dst));
	KASSERT(is_valid_ring(src));

	if (src->count <= 0) return;
	int count = src->count;

	/* 今回変換したいフレーム数 */
	if (track->is_draining) {
		/* ドレイン中は出来る限り実行 */
		count = min(count, (dst->capacity - dst->count));
	} else {
		/* 通常時は 1 ブロック */
		count = min(count, track->userio_frames_per_block);
	}
	if (count <= 0) return;

	/* 空きがない */
	if (dst->capacity - dst->count < count) return;

	int slice_count = 0;
	for (int remain_count = count; remain_count > 0; remain_count -= slice_count) {
		int dst_count = audio_ring_unround_free_count(dst);
		KASSERT(dst_count != 0);

		int src_count = audio_ring_unround_count(src);
		slice_count = min(remain_count, src_count);
		slice_count = min(slice_count, dst_count);
		KASSERT(slice_count > 0);

		internal_t *sptr = RING_TOP(internal_t, src);

		if (track->chmix_mode & AUDIO_TRACK_CHANNEL_VOLUME) {
			for (int i = 0; i < slice_count; i++) {
				for (int ch = 0; ch < src->fmt->channels; ch++, sptr++) {
					*sptr = (internal_t)(((internal2_t)*sptr) * track->ch_volume[ch] / 256);
				}
			}
		}

		sptr = RING_TOP(internal_t, src);
		internal_t *dptr = RING_BOT(internal_t, dst);

		switch (track->chmix_mode & AUDIO_TRACK_CHANNEL_MIX_MASK) {
		case AUDIO_TRACK_CHANNEL_SHRINK:
			for (int i = 0; i < slice_count; i++) {
				for (int ch = 0; ch < dst->fmt->channels; ch++) {
					*dptr++ = sptr[ch];
				}
				sptr += src->fmt->channels;
			}
			break;

		case AUDIO_TRACK_CHANNEL_EXPAND:
			for (int i = 0; i < slice_count; i++) {
				for (int ch = 0; ch < src->fmt->channels; ch++) {
					*dptr++ = *sptr++;
				}
				for (int ch = src->fmt->channels; ch < dst->fmt->channels; ch++) {
					*dptr++ = 0;
				}
			}
			break;

		case AUDIO_TRACK_CHANNEL_MIXLR:
			for (int i = 0; i < slice_count; i++) {
				internal2_t s;
				s = (internal2_t)sptr[0];
				s += (internal2_t)sptr[1];
				*dptr = s / 2;
				dptr++;
				sptr += src->fmt->channels;
			}
			break;

		case AUDIO_TRACK_CHANNEL_MIXALL:
			for (int i = 0; i < slice_count; i++) {
				internal2_t s = 0;
				for (int ch = 0; ch < src->fmt->channels; ch++, sptr++) {
					s += (internal2_t)sptr[ch];
				}
				*dptr = s / src->fmt->channels;
				dptr++;
			}
			break;

		case AUDIO_TRACK_CHANNEL_DUPLR:
			for (int i = 0; i < slice_count; i++) {
				dptr[0] = sptr[0];
				dptr[1] = sptr[0];
				dptr += dst->fmt->channels;
				sptr++;
			}
			if (dst->fmt->channels > 2) {
				dptr = RING_BOT(internal_t, dst);
				for (int i = 0; i < slice_count; i++) {
					for (int ch = 2; ch < dst->fmt->channels; ch++) {
						dptr[ch] = 0;
					}
					dptr += dst->fmt->channels;
				}
			}

			break;

		case AUDIO_TRACK_CHANNEL_DUPALL:
			for (int i = 0; i < slice_count; i++) {
				for (int ch = 0; ch < dst->fmt->channels; ch++) {
					*dptr = sptr[0];
					dptr++;
				}
				sptr++;
			}
			break;

		}
		audio_ring_appended(dst, slice_count);
		audio_ring_tookfromtop(src, slice_count);
	}
}

void
audio_track_freq(audio_track_t *track, audio_ring_t *dst, audio_ring_t *src)
{
	KASSERT(track);
	KASSERT(is_valid_ring(dst));
	KASSERT(is_valid_ring(src));

	if (src->count <= 0) return;

	/* 通常時は 1 ブロック */
	int count = track->mixer->frames_per_block;
	count = min(count, (dst->capacity - dst->count));

	if (track->is_draining) {
		/* ドレイン中は出来る限り実行 */
		count = dst->capacity - dst->count;
	}
	if (count <= 0) return;

	/* 空きがない */
	if (dst->capacity - dst->count < count) return;

	audio_rational_t one = { 1, 0 };
	int cmp1 = audio_rational_cmp(&track->freq_step, &one);

	if (cmp1 == 0) {
		// 周波数変更なし
		/* 通常、来ない */
		count = min(count, src->count);
		audio_ring_concat(dst, src, count);
		return;
	}

	int slice_count = 0;
	for (int remain_count = count; remain_count > 0; remain_count -= slice_count) {
		/* 今回処理するフレーム数を決定します。 */
		int src_count = audio_ring_unround_count(src);
		if (src_count == 0) break;
		int dst_count = audio_ring_unround_free_count(dst);
		KASSERT(dst_count != 0);
		slice_count = min(remain_count, dst_count);

#if AUDIO_FREQ_ALGORITHM == AUDIO_FREQ_ALGORITHM_SIMPLE
		// 単純法
		if (src_count <= track->freq_current.i) {
			audio_ring_tookfromtop(src, src_count);
			track->freq_current.i -= src_count;
			slice_count = 0;
		} else {
			internal_t *dptr = RING_BOT(internal_t, dst);
			for (int i = 0; i < slice_count; i++) {
				if (track->freq_current.i >= src_count) {
					slice_count = i;
					break;
				}
				// XXX: 高速化が必要
				internal_t *sptr = RING_PTR(internal_t, src, track->freq_current.i);
				for (int ch = 0; ch < dst->fmt->channels; ch++, dptr++, sptr++) {
					*dptr = *sptr;
				}

				audio_rational_add(&track->freq_current, &track->freq_step, dst->fmt->frequency);
			}
			audio_ring_tookfromtop(src, track->freq_current.i);
			audio_ring_appended(dst, slice_count);
			track->freq_current.i = 0;
		}

#else

		if (cmp1 < 0) {
			/* 周波数を上げる */
			// TODO: 補間法とか
		} else {
			/* 周波数を下げる */
			// TODO: 平均法とか
		}
#endif

	}
}

/*
 * 再生時の入力データを変換してトラックバッファに投入します。
 */
void
audio_track_play(audio_track_t *track)
{
	KASSERT(track);

	/* エンコーディング変換 */
	if (track->enconvert_mode == AUDIO_TRACK_ENCONVERT_THRU) {
		/* nop */
	} else {
		audio_track_enconvert(track, track->codec, track->step1, &track->userio_buf);
	}

	/* チャンネルミキサとチャンネルボリューム */
	if (track->chmix_mode == AUDIO_TRACK_CHANNEL_THRU) {
		/* nop */
	} else {
		audio_track_channel_mix(track, track->step2, track->step1);
	}

	/* 周波数変換 */
	if (track->freq_mode == AUDIO_TRACK_FREQ_THRU) {
		/* track_buf へ投入 */
		audio_ring_concat(&track->track_buf, track->step2, track->mixer->frames_per_block);

	} else if (track->freq_mode == AUDIO_TRACK_FREQ_INLINE) {
		/* userio_buf がソースのときは、userio_buf を返さないといけないので、
		バッファリングを必要に応じて実行 */
		/* ここで前段出力である track->step2 は userio_buf を指しているはず */
		if (track->freq_tmp->count == 0) {
			audio_track_freq(track, &track->track_buf, track->step2);
			if (track->freq_current.n != 0) {
				/* 剰余がある時はバッファへ */
				audio_ring_concat(track->freq_tmp, track->step2, track->step2->count);
			}
		} else {
			audio_ring_concat(track->freq_tmp, track->step2, INT_MAX);
			audio_track_freq(track, &track->track_buf, track->freq_tmp);
		}
	} else if (track->freq_mode == AUDIO_TRACK_FREQ_BUFFER) {
		audio_track_freq(track, &track->track_buf, track->step2);
	} else {
		panic("freq_mode");
	}
}

void
audio_mixer_init(audio_trackmixer_t *mixer, audio_softc_t *sc, int mode)
{
	memset(mixer, 0, sizeof(audio_trackmixer_t));
	mixer->sc = sc;

	mixer->hw_fmt = audio_softc_get_hw_format(mixer->sc, mode);
	mixer->hw_buf.fmt = &mixer->hw_fmt;
	mixer->hw_buf.capacity = audio_softc_get_hw_capacity(mixer->sc);
	mixer->hw_buf.sample = audio_softc_allocm(mixer->sc, RING_BYTELEN(&mixer->hw_buf));

	mixer->frames_per_block = mixer->hw_fmt.frequency * AUDIO_BLOCK_msec / 1000;

	mixer->track_fmt.encoding = AUDIO_ENCODING_SLINEAR_HE;
	mixer->track_fmt.channels = mixer->hw_fmt.channels;
	mixer->track_fmt.frequency = mixer->hw_fmt.frequency;
	mixer->track_fmt.precision = mixer->track_fmt.stride = AUDIO_INTERNAL_BITS;

	mixer->mix_fmt = mixer->track_fmt;
	mixer->mix_fmt.precision = mixer->mix_fmt.stride = AUDIO_INTERNAL_BITS * 2;

	/* 40ms double buffer */
	mixer->mix_buf.fmt = &mixer->mix_fmt;
	mixer->mix_buf.capacity = 2 * mixer->mix_fmt.frequency * AUDIO_BLOCK_msec / 1000;
	mixer->mix_buf.sample = audio_realloc(mixer->mix_buf.sample, RING_BYTELEN(&mixer->mix_buf));
	memset(mixer->mix_buf.sample, 0, RING_BYTELEN(&mixer->mix_buf));

	mixer->volume = 256;
}


/*
 * トラックバッファから 最大 1 ブロックを取り出し、
 * ミキシングして、ハードウェアに再生を通知します。
 */
void
audio_mixer_play(audio_trackmixer_t *mixer)
{
	/* 全部のトラックに聞く */

	audio_file_t *f;
	int track_count = 0;
	int track_ready = 0;
	int mixed = 0;
	SLIST_FOREACH(f, &mixer->sc->sc_files, entry) {
		track_count++;
		audio_track_t *track = &f->ptrack;
		/* 変換待ちのデータはいね～が */
		audio_track_play(track);

		audio_mixer_play_mix_track(mixer, track);

		if (track->is_draining
			|| track->mixed_count >= mixer->frames_per_block) {
			track_ready++;
		}

		if (track->mixed_count != 0) {
			if (mixed == 0) {
				mixed = track->mixed_count;
			} else {
				mixed = min(mixed, track->mixed_count);
			}
		}
	}

	mixer->mix_buf.count = mixed;

	/* 全員準備できたか、時間切れならハードウェアに転送 */
	if (track_ready == track_count
	|| audio_softc_play_busy(mixer->sc) == false) {
		audio_mixer_play_period(mixer);
	}
}

/*
* トラックバッファから取り出し、ミキシングします。
*/
void
audio_mixer_play_mix_track(audio_trackmixer_t *mixer, audio_track_t *track)
{
	if (track->track_buf.count <= 0) return;
	int count = track->track_buf.count;

	audio_ring_t track_mix;
	track_mix = mixer->mix_buf;
	track_mix.count = track->mixed_count;

	/* 今回ミキシングしたいフレーム数 */
	if (track->is_draining) {
		/* ドレイン中は出来る限り実行 */
		count = min(count, (track_mix.capacity - track_mix.count));
	} else {
		/* 通常時は 1 ブロック貯まるまで待つ */
		if (count < mixer->frames_per_block) return;
		count = mixer->frames_per_block;
	}
	if (count <= 0) return;

	if (track_mix.capacity - track_mix.count < count) {
		return;
	}

	int remain_count = count;
	for (; remain_count > 0; ) {
		int track_count = audio_ring_unround_count(&track->track_buf);
		int mix_count = audio_ring_unround_free_count(&track_mix);
		int slice_count = min(track_count, mix_count);
		slice_count = min(remain_count, slice_count);
		KASSERT(slice_count > 0);

		internal_t *sptr = RING_TOP(internal_t, &track->track_buf);
		internal2_t *dptr = RING_BOT(internal2_t, &track_mix);

		/* 整数倍精度へ変換し、トラックボリュームを適用して加算合成 */
		int slice_sample = slice_count * mixer->mix_fmt.channels;
		if (track->volume == AUDIO_TRACK_VOLUME_THRU) {
			for (int i = 0; i < slice_sample; i++) {
				*dptr++ += ((internal2_t)*sptr++);
			}
		} else {
			for (int i = 0; i < slice_sample; i++) {
				*dptr++ += ((internal2_t)*sptr++) * track->volume / 256;
			}
		}

		audio_ring_tookfromtop(&track->track_buf, slice_count);
		audio_ring_appended(&track_mix, slice_count);
		remain_count -= slice_count;
	}

	/* トラックバッファを取り込んだことを反映 */
	track->mixed_count += count;
}

/*
 * ミキシングバッファから物理デバイスバッファへ
 */
void
audio_mixer_play_period(audio_trackmixer_t *mixer /*, bool force */)
{
	/* XXX win32 は割り込みから再生 API をコール出来ないので、ポーリングする */
	if (audio_softc_play_busy(mixer->sc) == false) {
		audio_softc_play_start(mixer->sc);
	}

	/* 今回取り出すフレーム数を決定 */

	int mix_count = audio_ring_unround_count(&mixer->mix_buf);
	int hw_free_count = audio_ring_unround_free_count(&mixer->hw_buf);
	int count = min(mix_count, hw_free_count);
	if (count <= 0) {
		return;
	}
	count = min(count, mixer->frames_per_block);

	/* オーバーフロー検出も同時開催 */
	internal2_t overflow = AUDIO_INTERNAL_T_MAX;
	internal2_t ovf_minus = AUDIO_INTERNAL_T_MIN;

	internal2_t *mptr0 = RING_TOP(internal2_t, &mixer->mix_buf);
	internal2_t *mptr = mptr0;

	int sample_count = count * mixer->mix_fmt.channels;
	for (int i = 0; i < sample_count; i++) {
		if (*mptr > overflow) overflow = *mptr;
		if (*mptr < ovf_minus) ovf_minus = *mptr;

		mptr++;
	}
	if (-ovf_minus > overflow) overflow = -ovf_minus;

	/* マスタボリュームの自動制御 */
	int vol = mixer->volume;
	if (overflow * vol / 256 > AUDIO_INTERNAL_T_MAX) {
		/* オーバーフローしてたら少なくとも今回はボリュームを下げる */
		vol = (int)((internal2_t)AUDIO_INTERNAL_T_MAX * 256 / overflow);
		/* 128 までは自動でマスタボリュームを下げる */
		if (mixer->volume > 128) {
mixer->volume--;
		}
	}

	/* ここから ハードウェアチャンネル */

	/* マスタボリューム適用 */
	mptr = mptr0;
	for (int i = 0; i < sample_count; i++) {
		*mptr = *mptr * vol / 256;
		mptr++;
	}

	/* ハードウェアバッファへ転送 */
	lock(mixer->sc);
	mptr = mptr0;
	internal_t *hptr = RING_BOT(internal_t, &mixer->hw_buf);
	for (int i = 0; i < sample_count; i++) {
		*hptr++ = *mptr++;
	}
	audio_ring_appended(&mixer->hw_buf, count);
	unlock(mixer->sc);

	/* 使用済みミキサメモリを次回のために 0 フィルする */
	memset(mptr0, 0, sample_count * sizeof(internal2_t));
	audio_ring_tookfromtop(&mixer->mix_buf, count);

	/* トラックにハードウェアへ転送されたことを通知する */
	audio_file_t *f;
	SLIST_FOREACH(f, &mixer->sc->sc_files, entry) {
		audio_track_t *track = &f->ptrack;
		if (track->mixed_count <= count) {
			/* 要求転送量が全部転送されている */
			track->hw_count += track->mixed_count;
			track->mixer_hw_counter += track->mixed_count;
			track->mixed_count = 0;
		} else {
			/* のこりがある */
			track->hw_count += count;
			track->mixer_hw_counter += count;
			track->mixed_count -= count;
		}
	}

	/* ハードウェアへ通知する */
	audio_softc_play_start(mixer->sc);

}

void
audio_trackmixer_intr(audio_trackmixer_t *mixer, int count)
{
	KASSERT(count != 0);

	/* トラックにハードウェア出力が完了したことを通知する */
	audio_file_t *f;
	SLIST_FOREACH(f, &mixer->sc->sc_files, entry) {
		audio_track_t *track = &f->ptrack;
		if (track->hw_count <= count) {
			/* 要求転送量が全部転送されている */
			track->hw_complete_counter += track->hw_count;
			track->hw_count = 0;
		} else {
			/* のこりがある */
			track->hw_complete_counter += count;
			track->hw_count -= count;
		}
	}
}


void
audio_track_play_drain(audio_track_t *track)
{
	track->is_draining = true;

	/* フレームサイズ未満のため待たされていたデータを破棄 */
	track->subframe_buf_used = 0;

	/* userio_buf は待つ必要はない */
	/* chmix_buf は待つ必要はない */

	do {
		audio_mixer_play(track->mixer);
		WAIT();
	} while (track->track_buf.count > 0
		|| track->mixed_count > 0
		|| track->hw_count > 0);

	track->is_draining = false;
}

/* write の MI 側 */
int
audio_write(audio_softc_t *sc, struct uio *uio, int ioflag, audio_file_t *file)
{
	int error;
	audio_track_t *track = &file->ptrack;

	while (uio->uio_resid > 0) {

		/* userio の空きバイト数を求める */
		int free_count = audio_ring_unround_free_count(&track->userio_buf);
		int free_bytelen = free_count * track->userio_fmt.channels * track->userio_fmt.stride / 8 - track->subframe_buf_used;

		if (free_bytelen == 0) {
			panic("たぶんここで cv wait");
			continue;
		}

		// 今回 uiomove するバイト数 */
		int move_bytelen = min(free_bytelen, (int)uio->uio_resid);

		// 今回出来上がるフレーム数 */
		int framecount = (move_bytelen + track->subframe_buf_used) * 8 / (track->userio_fmt.channels * track->userio_fmt.stride);

		// コピー先アドレスは subframe_buf_used で調整する必要がある
		uint8_t *dptr = RING_BOT_UINT8(&track->userio_buf) + track->subframe_buf_used;
		// min(bytelen, uio->uio_resid) は uiomove が保証している
		error = uiomove(dptr, move_bytelen, uio);
		if (error) {
			panic("uiomove");
		}
		audio_ring_appended(&track->userio_buf, framecount);
		
		// 今回 userio_buf に置いたサブフレームを次回のために求める
		track->subframe_buf_used = move_bytelen - framecount * track->userio_fmt.channels * track->userio_fmt.stride / 8;

		// 今回作った userio を全部トラック再生へ渡す
		while (track->userio_buf.count > 0) {
			audio_mixer_play(track->mixer);
			WAIT();
		}
	}

	return 0;
}

/*
 * ***** audio_file *****
 */
int//ssize_t
sys_write(audio_file_t *file, void* buf, size_t len)
{
	KASSERT(buf);

	if (len > INT_MAX) {
		errno = EINVAL;
		return -1;
	}

	struct uio uio = buf_to_uio(buf, len, UIO_READ);

	int error = audio_write(file->sc, &uio, 0, file);
	if (error) {
		errno = error;
		return -1;
	}
	return (int)len;
}

audio_file_t *
sys_open(audio_softc_t *sc, int mode)
{
	audio_file_t *file;

	file = calloc(1, sizeof(audio_file_t));
	file->sc = sc;

	if (mode == AUDIO_PLAY) {
		audio_track_init(&file->ptrack, &sc->sc_pmixer);
	} else {
		audio_track_init(&file->rtrack, &sc->sc_rmixer);
	}

	SLIST_INSERT_HEAD(&sc->sc_files, file, entry);

	return file;
}
