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
#include "auintr.h"

static int audio_waitio(struct audio_softc *sc, void *kcondvar, audio_track_t *track);


#define AUDIO_TRACE
#ifdef AUDIO_TRACE
#define TRACE0(fmt, ...)	do {	\
	printf("%s ", __func__);	\
	printf(fmt, ## __VA_ARGS__);	\
	printf("\n");	\
} while (0)
#define TRACE(t, fmt, ...)	do {	\
	printf("%s #%d ", __func__, (t)->id);		\
	printf(fmt, ## __VA_ARGS__);	\
	printf("\n");	\
} while (0)
#else
#define TRACE0(fmt, ...)	/**/
#define TRACE(t, fmt, ...)	/**/
#endif

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

static void
audio_track_chvol(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);
	KASSERT(arg->context != NULL);
	KASSERT(arg->srcfmt->channels <= AUDIO_MAX_CHANNELS);

	int16_t *ch_volume = arg->context;
	const internal_t *sptr = arg->src;
	internal_t *dptr = arg->dst;

	for (int i = 0; i < arg->count; i++) {
		for (int ch = 0; ch < arg->srcfmt->channels; ch++, sptr++, dptr++) {
			*dptr = (internal_t)(((internal2_t)*sptr) * ch_volume[ch] / 256);
		}
	}
}

static void
audio_track_chmix_mixLR(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));

	const internal_t *sptr = arg->src;
	internal_t *dptr = arg->dst;

	for (int i = 0; i < arg->count; i++) {
		internal2_t s;
		s = (internal2_t)sptr[0];
		s += (internal2_t)sptr[1];
		*dptr = s / 2;
		dptr++;
		sptr += arg->srcfmt->channels;
	}
}

static void
audio_track_chmix_dupLR(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));

	const internal_t *sptr = arg->src;
	internal_t *dptr = arg->dst;

	for (int i = 0; i < arg->count; i++) {
		dptr[0] = sptr[0];
		dptr[1] = sptr[0];
		dptr += arg->dstfmt->channels;
		sptr++;
	}
	if (arg->dstfmt->channels > 2) {
		dptr = arg->dst;
		for (int i = 0; i < arg->count; i++) {
			for (int ch = 2; ch < arg->dstfmt->channels; ch++) {
				dptr[ch] = 0;
			}
			dptr += arg->dstfmt->channels;
		}
	}
}

static void
audio_track_chmix_shrink(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));

	const internal_t *sptr = arg->src;
	internal_t *dptr = arg->dst;

	for (int i = 0; i < arg->count; i++) {
		for (int ch = 0; ch < arg->dstfmt->channels; ch++) {
			*dptr++ = sptr[ch];
		}
		sptr += arg->srcfmt->channels;
	}
}

static void
audio_track_chmix_expand(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));

	const internal_t *sptr = arg->src;
	internal_t *dptr = arg->dst;

	for (int i = 0; i < arg->count; i++) {
		for (int ch = 0; ch < arg->srcfmt->channels; ch++) {
			*dptr++ = *sptr++;
		}
		for (int ch = arg->srcfmt->channels; ch < arg->dstfmt->channels; ch++) {
			*dptr++ = 0;
		}
	}
}

static void
audio_track_freq(audio_filter_arg_t *arg)
{
	audio_track_t *track = arg->context;
	audio_ring_t *src = &track->freq.srcbuf;
	audio_ring_t *dst = track->freq.dst;

	KASSERT(track);
	KASSERT(is_valid_ring(dst));
	KASSERT(is_valid_ring(src));
	KASSERT(src->count > 0);
	KASSERT(src->fmt.channels == dst->fmt.channels);

	int count = audio_ring_unround_free_count(dst);

	if (count <= 0) {
		return;
		//		panic("not impl");
	}

	// 単純法
	// XXX: 高速化が必要
	internal_t *dptr = RING_BOT(internal_t, dst);
	for (int i = 0; i < count; i++) {
		if (src->count <= 0) break;

		internal_t *sptr = RING_TOP(internal_t, src);
		for (int ch = 0; ch < dst->fmt.channels; ch++, dptr++, sptr++) {
			*dptr = *sptr;
		}

		audio_rational_add(&track->freq_current, &track->freq_step, dst->fmt.sample_rate);
		audio_ring_tookfromtop(src, track->freq_current.i);
		track->freq_current.i = 0;
		audio_ring_appended(dst, 1);
	}
}

audio_format2_t default_format = {
	AUDIO_ENCODING_MULAW,
	8000, /* freq */
	1, /* channels */
	8, /* precision */
	8, /* stride */
};

void
audio_track_init(audio_track_t *track, audio_trackmixer_t *mixer, int mode)
{
	static int newid = 0;
	memset(track, 0, sizeof(audio_track_t));
	track->id = newid++;
	// ここだけ id が決まってから表示
	TRACE(track, "");

	track->mixer = mixer;
	track->mode = mode;

	// 固定初期値
	track->volume = 256;
	for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
		track->ch_volume[i] = 256;
	}

	// デフォルトフォーマットでセット
	audio_track_set_format(track, &default_format);
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

/* stride に応じて、バイト境界に整列するフレーム数を求めます。 */
static inline int
audio_framealign(int stride)
{
	/* stride が、、、 */
	if ((stride & 7) == 0) {
		/* 8 の倍数なのでそのままでバイト境界 */
		return 1;
	} else if ((stride & 3) == 0) {
		/* 4 の倍数なので 2 フレームでバイト境界 */
		return 2;
	} else if ((stride & 1) == 0) {
		/* 2 の倍数なので 4 フレームでバイト境界 */
		return 4;
	} else {
		/* 8 とは互いに素なので 8 フレームでバイト境界 */
		return 8;
	}
}

static audio_ring_t *
init_codec(audio_track_t *track, audio_ring_t *last_dst)
{
	audio_format2_t *srcfmt = &track->inputfmt;
	audio_format2_t *dstfmt = &last_dst->fmt;

	if (srcfmt->encoding == dstfmt->encoding
	 && srcfmt->precision == dstfmt->precision
	 && srcfmt->stride == dstfmt->stride) {
		// チャンネル数以外が等しければエンコーディング変換不要
		track->codec.filter = NULL;
		return last_dst;
	} else {
		// エンコーディングを変換する
		track->codec.dst = last_dst;

		// arg のために先にフォーマットを作る
		track->codec.srcbuf.fmt = *dstfmt;
		track->codec.srcbuf.fmt.encoding = srcfmt->encoding;
		track->codec.srcbuf.fmt.precision = srcfmt->precision;
		track->codec.srcbuf.fmt.stride = srcfmt->stride;

		track->codec.arg.srcfmt = &track->codec.srcbuf.fmt;
		track->codec.arg.dstfmt = dstfmt;
		track->codec.filter = audio_MI_codec_filter_init(&track->codec.arg);

		// TODO: codec デストラクタコール
		// XXX: インライン変換はとりあえず置いておく
		track->codec.srcbuf.top = 0;
		track->codec.srcbuf.count = 0;
		track->codec.srcbuf.capacity = track->codec.srcbuf.fmt.sample_rate * AUDIO_BLK_MS / 1000;
		track->codec.srcbuf.sample = audio_realloc(track->codec.srcbuf.sample, RING_BYTELEN(&track->codec.srcbuf));

		return &track->codec.srcbuf;
	}
}

static audio_ring_t *
init_chvol(audio_track_t *track, audio_ring_t *last_dst)
{
	audio_format2_t *srcfmt = &track->inputfmt;
	audio_format2_t *dstfmt = &last_dst->fmt;

	// チャンネルボリュームが有効かどうか
	bool use_chvol = false;
	for (int ch = 0; ch < srcfmt->channels; ch++) {
		if (track->ch_volume[ch] != 256) {
			use_chvol = true;
			break;
		}
	}

	if (use_chvol == false) {
		track->chvol.filter = NULL;
		return last_dst;
	} else {
		track->chvol.filter = audio_track_chvol;
		track->chvol.dst = last_dst;

		// 周波数とチャンネル数がユーザ指定値。
		track->chvol.srcbuf.fmt = *dstfmt;
		track->chvol.srcbuf.capacity = track->chvol.srcbuf.fmt.sample_rate * AUDIO_BLK_MS / 1000; 
		track->chvol.srcbuf.sample = audio_realloc(track->chvol.srcbuf.sample, RING_BYTELEN(&track->chvol.srcbuf));

		track->chvol.arg.count = track->chvol.srcbuf.capacity;
		track->chvol.arg.context = track->ch_volume;
		return &track->chvol.srcbuf;
	}
}


static audio_ring_t *
init_chmix(audio_track_t *track, audio_ring_t *last_dst)
{
	audio_format2_t *srcfmt = &track->inputfmt;
	audio_format2_t *dstfmt = &last_dst->fmt;

	int srcch = srcfmt->channels;
	int dstch = dstfmt->channels;

	if (srcch == dstch) {
		track->chmix.filter = NULL;
		return last_dst;
	} else {
		if (srcch == 2 && dstch == 1) {
			track->chmix.filter = audio_track_chmix_mixLR;
		} else if (srcch == 1 && dstch == 2) {
			track->chmix.filter = audio_track_chmix_dupLR;
		} else if (srcch > dstch) {
			track->chmix.filter = audio_track_chmix_shrink;
		} else {
			track->chmix.filter = audio_track_chmix_expand;
		}

		track->chmix.dst = last_dst;
		// チャンネル数を srcch にする
		track->chmix.srcbuf.fmt = *dstfmt;
		track->chmix.srcbuf.fmt.channels = srcch;
		track->chmix.srcbuf.top = 0;
		track->chmix.srcbuf.count = 0;
		// バッファサイズは計算で決められるはずだけど。とりあえず。
		track->chmix.srcbuf.capacity = track->chmix.srcbuf.fmt.sample_rate * AUDIO_BLK_MS / 1000;
		track->chmix.srcbuf.sample = audio_realloc(track->chmix.srcbuf.sample, RING_BYTELEN(&track->chmix.srcbuf));

		track->chmix.arg.srcfmt = &track->chmix.srcbuf.fmt;
		track->chmix.arg.dstfmt = dstfmt;

		return &track->chmix.srcbuf;
	}
}

static audio_ring_t*
init_freq(audio_track_t *track, audio_ring_t *last_dst)
{
	audio_format2_t *srcfmt = &track->inputfmt;
	audio_format2_t *dstfmt = &last_dst->fmt;

	uint32_t srcfreq = srcfmt->sample_rate;
	uint32_t dstfreq = dstfmt->sample_rate;

	if (srcfreq == dstfreq) {
		track->freq.filter = NULL;
		return last_dst;
	} else {
		track->freq_step.i = srcfreq / dstfreq;
		track->freq_step.n = srcfreq % dstfreq;
		audio_rational_clear(&track->freq_current);

		track->freq.arg.context = track;

		track->freq.filter = audio_track_freq;
		track->freq.dst = last_dst;
		// 周波数のみ srcfreq
		track->freq.srcbuf.fmt = *dstfmt;
		track->freq.srcbuf.fmt.sample_rate = srcfreq;
		track->freq.srcbuf.top = 0;
		track->freq.srcbuf.count = 0;
		track->freq.srcbuf.capacity = track->freq.srcbuf.fmt.sample_rate * AUDIO_BLK_MS / 1000;
		track->freq.srcbuf.sample = audio_realloc(track->freq.srcbuf.sample, RING_BYTELEN(&track->freq.srcbuf));
		return &track->freq.srcbuf;
	}
}

/*
* トラックのユーザランド側フォーマットを設定します。
* 変換用内部バッファは一度破棄されます。
*/
void
audio_track_set_format(audio_track_t *track, audio_format2_t *fmt)
{
	TRACE(track, "");
	KASSERT(is_valid_format(fmt));

	// TODO: 入力値チェックをどこかでやる。

	// TODO: まず現在のバッファとかを全部破棄すると分かり易いが。

	audio_ring_t *last_dst = &track->outputbuf;
	if (track->mode == AUMODE_PLAY) {
		// 再生はトラックミキサ側から作る

		track->inputfmt = *fmt;

		track->outputbuf.fmt =  track->mixer->track_fmt;

		last_dst = init_freq(track, last_dst);
		last_dst = init_chmix(track, last_dst);
		last_dst = init_chvol(track, last_dst);
		last_dst = init_codec(track, last_dst);
	} else {
		// 録音はユーザランド側から作る

		track->inputfmt = track->mixer->track_fmt;

		track->outputbuf.fmt = *fmt;

		last_dst = init_codec(track, last_dst);
		last_dst = init_chvol(track, last_dst);
		last_dst = init_chmix(track, last_dst);
		last_dst = init_freq(track, last_dst);
	}

	// 入力バッファは先頭のステージ相当品
	track->input = last_dst;

	// 出力フォーマットに従って outputbuf を作る
	track->outputbuf.top = 0;
	track->outputbuf.count = 0;
	track->outputbuf.capacity = 16 * track->outputbuf.fmt.sample_rate * AUDIO_BLK_MS / 1000;
	track->outputbuf.sample = audio_realloc(track->outputbuf.sample, RING_BYTELEN(&track->outputbuf));

	if (debug) {
		printf("%s: userfmt=%s\n", __func__, fmt_tostring(&track->inputfmt));
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
 * 再生時の入力データを変換してトラックバッファに投入します。
 */
void
audio_track_play(audio_track_t *track, bool isdrain)
{
	KASSERT(track);

	int track_count_0 = track->outputbuf.count;

	/* エンコーディング変換 */
	if (track->codec.filter != NULL) {
		int srccount = audio_ring_unround_count(&track->codec.srcbuf);
		int dstcount = audio_ring_unround_free_count(track->codec.dst);

		int count = min(srccount, dstcount);
		
		if (count > 0) {
			audio_filter_arg_t *arg = &track->codec.arg;

			arg->dst = RING_BOT_UINT8(track->codec.dst);
			arg->src = RING_TOP_UINT8(&track->codec.srcbuf);
			arg->count = count;

			track->codec.filter(arg);

			audio_ring_tookfromtop(&track->codec.srcbuf, count);
			audio_ring_appended(track->codec.dst, count);
		}
	}

	if (track->codec.dst->count == 0) return;

	/* ブロックサイズに整形 */
	int n = track->codec.dst->capacity - track->codec.dst->count;
	if (n > 0) {
		if (isdrain) {
			/* 無音をブロックサイズまで埋める */
			/* 内部フォーマットだとわかっている */
			/* ここは入力がブロックサイズ未満の端数だった場合。次段以降がブロックサイズを想定しているので、ここでまず追加。*/
			TRACE(track, "Append silence %d frames", n);
			KASSERT(audio_ring_unround_free_count(track->codec.dst) >= n);

			memset(RING_BOT_UINT8(track->codec.dst), 0, n * track->codec.dst->fmt.channels * sizeof(internal_t));
			audio_ring_appended(track->codec.dst, n);
		} else {
			// ブロックサイズたまるまで処理をしない
			return;
		}
	}

	KASSERT(track->codec.dst->count == track->codec.dst->capacity);
	
	/* チャンネルボリューム */
	if (track->chvol.filter != NULL
	 && audio_ring_unround_count(&track->chvol.srcbuf) >= track->chvol.arg.count
	 && audio_ring_unround_free_count(track->chvol.dst) >= track->chvol.arg.count) {
		track->chvol.arg.src = RING_TOP(internal_t, &track->chvol.srcbuf);
		track->chvol.arg.dst = RING_BOT(internal_t, track->chvol.dst);
		track->chvol.filter(&track->chvol.arg);
		audio_ring_appended(track->chvol.dst, track->chvol.arg.count);
		audio_ring_tookfromtop(&track->chvol.srcbuf, track->chvol.arg.count);
	}

	/* チャンネルミキサ */
	if (track->chmix.filter != NULL
	 && audio_ring_unround_count(&track->chmix.srcbuf) >= track->chmix.arg.count
	 && audio_ring_unround_free_count(track->chmix.dst) >= track->chmix.arg.count) {
		track->chmix.arg.src = RING_TOP(internal_t, &track->chmix.srcbuf);
		track->chmix.arg.dst = RING_BOT(internal_t, track->chmix.dst);

		// XXX ほんとうは setformat での値のはず
		track->chmix.arg.count = track->chmix.srcbuf.count;

		track->chmix.filter(&track->chmix.arg);
		audio_ring_appended(track->chmix.dst, track->chmix.arg.count);
		audio_ring_tookfromtop(&track->chmix.srcbuf, track->chmix.arg.count);
	}

	/* 周波数変換 */
	if (track->freq.filter != NULL) {
		// XXX
		track->freq.filter(&track->freq.arg);
	}

	if (isdrain) {
		/* 無音をブロックサイズまで埋める */
		/* 内部フォーマットだとわかっている */
		/* 周波数変換の結果、ブロック未満の端数フレームが出る */
		int n = track->outputbuf.count % track->mixer->frames_per_block;
		if (n > 0) {
			n = track->mixer->frames_per_block - n;
			TRACE(track, "Append silence %d frames to track_buf", n);
			KASSERT(audio_ring_unround_free_count(&track->outputbuf) >= n);

			memset(RING_BOT_UINT8(&track->outputbuf), 0, n * track->outputbuf.fmt.channels * sizeof(internal_t));
			audio_ring_appended(&track->outputbuf, n);
		}
	}

	if (track->input == &track->outputbuf) {
		track->outputcounter = track->inputcounter;
	} else {
		track->outputcounter += track->outputbuf.count - track_count_0;
	}

//	TRACE(track, "trackbuf=%d", track->outputbuf.count);
	if (track->mixer->busy == false) {
		audio_mixer_play(track->mixer, false);
	}
}

void
audio_mixer_init(audio_trackmixer_t *mixer, struct audio_softc *sc, int mode)
{
	TRACE0("");
	memset(mixer, 0, sizeof(audio_trackmixer_t));
	mixer->sc = sc;

	mixer->hw_buf.fmt = audio_softc_get_hw_format(mixer->sc, mode);
	mixer->hw_buf.capacity = audio_softc_get_hw_capacity(mixer->sc);
	mixer->hw_buf.sample = audio_softc_allocm(mixer->sc, RING_BYTELEN(&mixer->hw_buf));

	mixer->frames_per_block = mixer->hw_buf.fmt.sample_rate * AUDIO_BLK_MS / 1000;

	mixer->track_fmt.encoding = AUDIO_ENCODING_SLINEAR_HE;
	mixer->track_fmt.channels = mixer->hw_buf.fmt.channels;
	mixer->track_fmt.sample_rate = mixer->hw_buf.fmt.sample_rate;
	mixer->track_fmt.precision = mixer->track_fmt.stride = AUDIO_INTERNAL_BITS;

	/* 40ms double buffer */
	mixer->mixbuf.fmt = mixer->track_fmt;
	mixer->mixbuf.fmt.precision = mixer->mixbuf.fmt.stride = AUDIO_INTERNAL_BITS * 2;
	mixer->mixbuf.capacity = 2 * mixer->frames_per_block;
	mixer->mixbuf.sample = audio_realloc(mixer->mixbuf.sample, RING_BYTELEN(&mixer->mixbuf));
	memset(mixer->mixbuf.sample, 0, RING_BYTELEN(&mixer->mixbuf));

	mixer->volume = 256;
}


/*
 * トラックバッファから 最大 1 ブロックを取り出し、
 * ミキシングして、ハードウェアに再生を通知します。
 */
void
audio_mixer_play(audio_trackmixer_t *mixer, bool isdrain)
{
	//TRACE0("");
	/* 全部のトラックに聞く */

	mixer->busy = true;

	audio_file_t *f;
	int mixed = 0;
	if (mixer->mixbuf.count < mixer->mixbuf.capacity) {
		SLIST_FOREACH(f, &mixer->sc->sc_files, entry) {
			audio_track_t *track = &f->ptrack;

			if (track->outputbuf.count < mixer->frames_per_block) {
				audio_track_play(track, isdrain);
			}

			// 合成
			if (track->outputbuf.count > 0) {
				audio_mixer_play_mix_track(mixer, track);
			}

			if (track->mixed_count != 0) {
				if (mixed == 0) {
					mixed = track->mixed_count;
				} else {
					mixed = min(mixed, track->mixed_count);
				}
			}
		}

		mixer->mixbuf.count = mixed;
	}

	// バッファの準備ができたら転送。
	if (mixer->mixbuf.count >= mixer->frames_per_block
		&& mixer->hw_buf.capacity - mixer->hw_buf.count >= mixer->frames_per_block) {
		audio_mixer_play_period(mixer);
	}

	if (mixer->mixbuf.count == 0 && mixer->hw_buf.count == 0) {
		mixer->busy = false;
	}
}

/*
* トラックバッファから取り出し、ミキシングします。
*/
void
audio_mixer_play_mix_track(audio_trackmixer_t *mixer, audio_track_t *track)
{
	/* 1 ブロック貯まるまで待つ */
	if (track->outputbuf.count < mixer->frames_per_block) {
		TRACE0("track count too short: track_buf.count=%d", track->outputbuf.count);
		return;
	}

	int count = mixer->frames_per_block;

	// mixer->mixbuf の top 位置から、このトラックの mixed_count までは前回処理済みなので、
	// コピーしたローカル ring で処理をする。
	audio_ring_t mix_tmp;
	mix_tmp = mixer->mixbuf;
	mix_tmp.count = track->mixed_count;

	if (mix_tmp.capacity - mix_tmp.count < count) {
		TRACE(track, "mix_buf full");
		return;
	}

	KASSERT(audio_ring_unround_count(&track->outputbuf) >= count);
	KASSERT(audio_ring_unround_free_count(&mix_tmp) >= count);

	internal_t *sptr = RING_TOP(internal_t, &track->outputbuf);
	internal2_t *dptr = RING_BOT(internal2_t, &mix_tmp);

	/* 整数倍精度へ変換し、トラックボリュームを適用して加算合成 */
	int sample_count = count * mixer->mixbuf.fmt.channels;
	if (track->volume == 256) {
		for (int i = 0; i < sample_count; i++) {
			*dptr++ += ((internal2_t)*sptr++);
		}
	} else {
		for (int i = 0; i < sample_count; i++) {
			*dptr++ += ((internal2_t)*sptr++) * track->volume / 256;
		}
	}

	audio_ring_tookfromtop(&track->outputbuf, count);

	/* トラックバッファを取り込んだことを反映 */
	track->mixed_count += count;
	TRACE(track, "mixed+=%d", count);
}

/*
 * ミキシングバッファから物理デバイスバッファへ
 */
void
audio_mixer_play_period(audio_trackmixer_t *mixer /*, bool force */)
{
	/* 今回取り出すフレーム数を決定 */

	int mix_count = audio_ring_unround_count(&mixer->mixbuf);
	int hw_free_count = audio_ring_unround_free_count(&mixer->hw_buf);
	int count = min(mix_count, hw_free_count);
	if (count <= 0) {
		TRACE0("count too short: mix_count=%d hw_free=%d", mix_count, hw_free_count);
		return;
	}
	count = min(count, mixer->frames_per_block);

	/* オーバーフロー検出 */
	internal2_t ovf_plus = AUDIO_INTERNAL_T_MAX;
	internal2_t ovf_minus = AUDIO_INTERNAL_T_MIN;

	internal2_t *mptr0 = RING_TOP(internal2_t, &mixer->mixbuf);
	internal2_t *mptr = mptr0;

	int sample_count = count * mixer->mixbuf.fmt.channels;
	for (int i = 0; i < sample_count; i++) {
		if (*mptr > ovf_plus) ovf_plus = *mptr;
		if (*mptr < ovf_minus) ovf_minus = *mptr;

		mptr++;
	}

	/* マスタボリュームの自動制御 */
	int vol = mixer->volume;
	if (ovf_plus * vol / 256 > AUDIO_INTERNAL_T_MAX) {
		/* オーバーフローしてたら少なくとも今回はボリュームを下げる */
		vol = (int)((internal2_t)AUDIO_INTERNAL_T_MAX * 256 / ovf_plus);
	}
	if (ovf_minus * vol / 256 < AUDIO_INTERNAL_T_MIN) {
		vol = (int)((internal2_t)AUDIO_INTERNAL_T_MIN * 256 / ovf_minus);
	}
	if (vol < mixer->volume) {
		/* 128 までは自動でマスタボリュームを下げる */
		if (mixer->volume > 128) {
			mixer->volume--;
		}
	}

	/* ここから ハードウェアチャンネル */

	/* マスタボリューム適用 */
	if (vol != 256) {
		mptr = mptr0;
		for (int i = 0; i < sample_count; i++) {
			*mptr = *mptr * vol / 256;
			mptr++;
		}
	}

	/* ハードウェアバッファへ転送 */
	// TODO: MD 側フィルタ
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
	audio_ring_tookfromtop(&mixer->mixbuf, count);

	/* トラックにハードウェアへ転送されたことを通知する */
	audio_file_t *f;
	SLIST_FOREACH(f, &mixer->sc->sc_files, entry) {
		audio_track_t *track = &f->ptrack;
		if (track->mixed_count > 0) {
			KASSERT(track->completion_blkcount < _countof(track->completion_blkID));

			track->completion_blkID[track->completion_blkcount] = mixer->hw_blkID;
			track->completion_blkcount++;

			if (track->mixed_count <= count) {
				/* 要求転送量が全部転送されている */
				track->mixer_hw_counter += track->mixed_count;
				track->mixed_count = 0;
			} else {
				/* のこりがある */
				track->mixer_hw_counter += count;
				track->mixed_count -= count;
			}
		}
	}

	/* ハードウェアへ通知する */
	TRACE0("start count=%d blkid=%d", count, mixer->hw_blkID);
	audio_softc_play_start(mixer->sc);
	mixer->hw_output_counter += count;

	// この blkID の出力は終わり。次。
	mixer->hw_blkID++;
}

void
audio_trackmixer_intr(audio_trackmixer_t *mixer, int count)
{
	TRACE0("");
	KASSERT(count != 0);

	mixer->hw_complete_counter += count;

	/* トラックにハードウェア出力が完了したことを通知する */
	audio_file_t *f;
	SLIST_FOREACH(f, &mixer->sc->sc_files, entry) {
		audio_track_t *track = &f->ptrack;
		if (track->completion_blkcount > 0) {
			if (track->completion_blkID[0] < mixer->hw_cmplID) {
				panic("missing block");
			}

			if (track->completion_blkID[0] == mixer->hw_cmplID) {
				track->hw_complete_counter += mixer->frames_per_block;
				// キューは小さいのでポインタをごそごそするより速いんではないか
				for (int i = 0; i < track->completion_blkcount - 1; i++) {
					track->completion_blkID[i] = track->completion_blkID[i + 1];
				}
				track->completion_blkcount--;
			}
		}
	}
	mixer->hw_cmplID++;

#if !false
	/* XXX win32 は割り込みから再生 API をコール出来ないので、ポーリングする */
	if (audio_softc_play_busy(mixer->sc) == false) {
		audio_softc_play_start(mixer->sc);
	}
#endif
	audio_mixer_play(mixer, true);
}

#ifdef AUDIO_INTR_EMULATED
void
audio_track_play_drain(audio_track_t *track)
{
	// 割り込みエミュレートしているときはメインループに制御を戻さないといけない
	audio_track_play_drain_core(track, false);
}
#else
void
audio_track_play_drain(audio_track_t *track)
{
	audio_track_play_drain_core(track, true);
}
#endif

void
audio_track_play_drain_core(audio_track_t *track, bool wait)
{
	TRACE(track, "");
	track->is_draining = true;

	// 無音挿入させる
	audio_track_play(track, true);

	/* フレームサイズ未満のため待たされていたデータを破棄 */
	track->subframe_buf_used = 0;

	/* userio_buf は待つ必要はない */
	/* chmix_buf は待つ必要はない */
	if (wait) {
		do {
			audio_waitio(track->mixer->sc, NULL, track);
			//audio_mixer_play(track->mixer);
		} while (track->outputbuf.count > 0
			|| track->mixed_count > 0
			|| track->completion_blkcount > 0);

		track->is_draining = false;
		printf("#%d: uio_count=%lld trk_count=%lld tm=%lld mixhw=%lld hw_complete=%lld\n", track->id,
			track->inputcounter, track->outputcounter, 
			track->track_mixer_counter, track->mixer_hw_counter,
			track->hw_complete_counter);
	}
}

/* write の MI 側 */
int
audio_write(struct audio_softc *sc, struct uio *uio, int ioflag, audio_file_t *file)
{
	int error;
	audio_track_t *track = &file->ptrack;
	TRACE(track, "");

	while (uio->uio_resid > 0) {

		/* userio の空きバイト数を求める */
		int free_count = audio_ring_unround_free_count(track->input);
		int free_bytelen = free_count * track->inputfmt.channels * track->inputfmt.stride / 8 - track->subframe_buf_used;

		if (free_bytelen == 0) {
			audio_waitio(sc, NULL, track);
		}

		// 今回 uiomove するバイト数 */
		int move_bytelen = min(free_bytelen, (int)uio->uio_resid);

		// 今回出来上がるフレーム数 */
		int framecount = (move_bytelen + track->subframe_buf_used) * 8 / (track->inputfmt.channels * track->inputfmt.stride);

		// コピー先アドレスは subframe_buf_used で調整する必要がある
		uint8_t *dptr = RING_BOT_UINT8(track->input) + track->subframe_buf_used;
		// min(bytelen, uio->uio_resid) は uiomove が保証している
		error = uiomove(dptr, move_bytelen, uio);
		if (error) {
			panic("uiomove");
		}
		audio_ring_appended(track->input, framecount);
		track->inputcounter += framecount;
		
		// 今回 userio_buf に置いたサブフレームを次回のために求める
		track->subframe_buf_used = move_bytelen - framecount * track->inputfmt.channels * track->inputfmt.stride / 8;

		// 今回作った userio を全部トラック再生へ渡す
		audio_track_play(track, false);

		// XXX: エミュレーション用に CPU 割り込み受付
		audio_waitio(sc, NULL, track);
	}

	return 0;
}

static int
audio_waitio(struct audio_softc *sc, void *kcondvar, audio_track_t *track)
{
	// 本当は割り込みハンドラからトラックが消費されるんだけど
	// ここで消費をエミュレート。

//	TRACE0("");

	emu_intr_check();

#if false
	/* 全部のトラックに聞く */

	audio_file_t *f;
	SLIST_FOREACH(f, &sc->sc_files, entry) {
		audio_track_t *ptrack = &f->ptrack;

		audio_track_play(ptrack);
	}
#endif
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
sys_open(struct audio_softc *sc, int mode)
{
	audio_file_t *file;

	file = calloc(1, sizeof(audio_file_t));
	file->sc = sc;

	if (mode == AUMODE_PLAY) {
		audio_track_init(&file->ptrack, &sc->sc_pmixer, AUMODE_PLAY);
	} else {
		audio_track_init(&file->rtrack, &sc->sc_rmixer, AUMODE_RECORD);
	}

	SLIST_INSERT_HEAD(&sc->sc_files, file, entry);

	return file;
}
