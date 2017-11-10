#if defined(_KERNEL)
#include <dev/audio/aumix.h>
#include <dev/audio/auring.h>
#include <dev/audio/aucodec.h>
#else
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
#endif // !_KERNEL

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

#if defined(_KERNEL)
#define x_malloc(mem)			kern_malloc(mem, M_NOWAIT)
#define x_realloc(mem, size)	kern_realloc(mem, size, M_NOWAIT)
#define x_free(mem)				kern_free(mem)
#define lock(x)					/*とりあえず*/
#define unlock(x)				/*とりあえず*/
#else
#define x_malloc(mem)			malloc(mem)
#define x_realloc(mem, size)	realloc(mem, size)
#define x_free(mem)				free(mem)
#endif

void *audio_realloc(void *memblock, size_t bytes);
void *audio_free(void *memblock);
int16_t audio_volume_to_inner(uint8_t v);
uint8_t audio_volume_to_outer(int16_t v);
void audio_track_lock(audio_track_t *track);
void audio_track_unlock(audio_track_t *track);

#if !defined(_KERNEL)
typedef void kcondvar_t;
static int audio_waitio(struct audio_softc *sc, kcondvar_t *chan, audio_track_t *track);
#endif // !_KERNEL


/* メモリアロケーションの STUB */

void *
audio_realloc(void *memblock, size_t bytes)
{
	if (memblock != NULL) {
		if (bytes != 0) {
			return x_realloc(memblock, bytes);
		} else {
			x_free(memblock);
			return NULL;
		}
	} else {
		if (bytes != 0) {
			return x_malloc(bytes);
		} else {
			return NULL;
		}
	}
}

void *
audio_free(void *memblock)
{
	if (memblock != NULL) {
		x_free(memblock);
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

// ring が空でなく 1 ブロックに満たない時、1ブロックまで無音を追加します。
static int
audio_append_slience(audio_track_t *track, audio_ring_t *ring)
{
	KASSERT(is_internal_format(&ring->fmt));

	if (ring->count == 0) return 0;

	int fpb = ring->fmt.sample_rate * AUDIO_BLK_MS / 1000;
	if (ring->count >= fpb) {
		return 0;
	}

	int n = (ring->capacity - ring->count) % fpb;
	
	TRACE(track, "Append silence %d frames", n);
	KASSERT(audio_ring_unround_free_count(ring) >= n);

	memset(RING_BOT_UINT8(ring), 0, n * ring->fmt.channels * sizeof(internal_t));
	audio_ring_appended(ring, n);
	return n;
}

// このステージで処理を中断するときは false を返します。
static bool
audio_apply_stage(audio_track_t *track, audio_stage_t *stage, bool isdrain)
{
	if (stage->filter != NULL) {
		int srccount = audio_ring_unround_count(&stage->srcbuf);
		int dstcount = audio_ring_unround_free_count(stage->dst);
		
		int count = min(srccount, dstcount);

		if (count > 0) {
			audio_filter_arg_t *arg = &stage->arg;

			arg->src = RING_TOP_UINT8(&stage->srcbuf);
			arg->dst = RING_BOT_UINT8(stage->dst);
			arg->count = count;

			stage->filter(arg);

			audio_ring_tookfromtop(&stage->srcbuf, count);
			audio_ring_appended(stage->dst, count);
		}

		/* ブロックサイズに整形 */
		if (isdrain) {
			audio_append_slience(track, stage->dst);
		} else {
			int fpb = stage->dst->fmt.sample_rate * AUDIO_BLK_MS / 1000;
			if (stage->dst->count < fpb) {
				// ブロックサイズたまるまで処理をしない
				return false;
			}
		}
	}
	return true;
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
	if (audio_apply_stage(track, &track->codec, isdrain) == false) {
		return;
	}

	/* チャンネルボリューム */
	if (audio_apply_stage(track, &track->chvol, isdrain) == false) {
		return;
	}

	/* チャンネルミキサ */
	if (audio_apply_stage(track, &track->chmix, isdrain) == false) {
		return;
	}

	/* 周波数変換 */
	// フレーム数が前後で変わるので apply 使えない
	if (track->freq.filter != NULL) {
		if (track->freq.srcbuf.count > 0) {
			track->freq.filter(&track->freq.arg);
		}
	}

	if (isdrain) {
		/* 無音をブロックサイズまで埋める */
		/* 内部フォーマットだとわかっている */
		/* 周波数変換の結果、ブロック未満の端数フレームが出ることもあるし、
		変換なしのときは入力自体が半端なときもあろう */
		audio_append_slience(track, &track->outputbuf);
	}

	if (track->input == &track->outputbuf) {
		track->outputcounter = track->inputcounter;
	} else {
		track->outputcounter += track->outputbuf.count - track_count_0;
	}

//	TRACE(track, "trackbuf=%d", track->outputbuf.count);
	// 1 トラック目の再生開始可能条件を満たしたらミキサ起動
	if (track->mixer->busy == false && track->outputbuf.count >= track->mixer->frames_per_block) {
		audio_mixer_play(track->mixer, false);
	}
}

void
audio_mixer_init(struct audio_softc *sc, audio_trackmixer_t *mixer, int mode)
{
	TRACE0("");
	memset(mixer, 0, sizeof(audio_trackmixer_t));
	mixer->sc = sc;

#if defined(_KERNEL)
	// XXX とりあえず
	if (mode == AUMODE_PLAY)
		mixer->hwbuf.fmt = sc->sc_phwfmt;
	else
		mixer->hwbuf.fmt = sc->sc_rhwfmt;

	int framelen = mixer->hwbuf.fmt.stride * mixer->hwbuf.fmt.channels;
	int capacity = (mixer->hwbuf.fmt.precision * AUDIO_BLK_MS / 1000) * 2;
	int bufsize = capacity * framelen;
	if (sc->hw_if->round_buffersize) {
		int rounded;
		rounded = sc->hw_if->round_buffersize(sc->hw_hdl, mode, bufsize);
		// 縮められても困る?
		if (rounded != bufsize) {
			aprint_error_dev(sc->dev, "buffer size not configured"
			    "%d -> %d\n", bufsize, rounded);
			return;
		}
	}

	if (sc->hw_if->allocm) {
		mixer->hwbuf.sample = sc->hw_if->allocm(sc->hw_hdl, mode,
		    bufsize);
	} else {
		mixer->hwbuf.sample = kmem_zalloc(bufsize, KM_SLEEP);
	}
#else
	mixer->hwbuf.fmt = audio_softc_get_hw_format(mixer->sc, mode);
	mixer->hwbuf.capacity = audio_softc_get_hw_capacity(mixer->sc);
	mixer->hwbuf.sample = audio_softc_allocm(mixer->sc, RING_BYTELEN(&mixer->hwbuf));
#endif

	mixer->frames_per_block = mixer->hwbuf.fmt.sample_rate * AUDIO_BLK_MS / 1000;

	mixer->track_fmt.encoding = AUDIO_ENCODING_SLINEAR_HE;
	mixer->track_fmt.channels = mixer->hwbuf.fmt.channels;
	mixer->track_fmt.sample_rate = mixer->hwbuf.fmt.sample_rate;
	mixer->track_fmt.precision = mixer->track_fmt.stride = AUDIO_INTERNAL_BITS;

	mixer->mixbuf.fmt = mixer->track_fmt;
	mixer->mixbuf.fmt.precision = mixer->mixbuf.fmt.stride = AUDIO_INTERNAL_BITS * 2;
	mixer->mixbuf.capacity = mixer->frames_per_block;
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

	// ダブルバッファを埋める
	for (int i = 0; i < 2; i++) {

		if (mixer->hwbuf.capacity - mixer->hwbuf.count >= mixer->frames_per_block) {

			audio_file_t *f;
			int mixed = 0;
			SLIST_FOREACH(f, &mixer->sc->sc_files, entry) {
				audio_track_t *track = &f->ptrack;

				if (track->outputbuf.count < mixer->frames_per_block) {
					audio_track_play(track, isdrain);
				}

				// 合成
				if (track->outputbuf.count > 0) {
					mixed += audio_mixer_play_mix_track(mixer, track);
				}
			}

			if (mixed > 0) {
				// バッファの準備ができたら転送。
				mixer->mixbuf.count = mixer->frames_per_block;
				audio_mixer_play_period(mixer);
			}
		}
	}

	if (mixer->hwseq == mixer->mixseq) {
		mixer->busy = false;
	}
}

/*
* トラックバッファから取り出し、ミキシングします。
*/
int
audio_mixer_play_mix_track(audio_trackmixer_t *mixer, audio_track_t *track)
{
	/* 1 ブロック貯まるまで待つ */
	if (track->outputbuf.count < mixer->frames_per_block) {
		TRACE0("track count too short: track_buf.count=%d", track->outputbuf.count);
		return 0;
	}

	// このトラックが処理済みならなにもしない
	if (mixer->mixseq < track->seq) return 0;

	int count = mixer->frames_per_block;

	// トラックごとなのでコピーしたローカル ring で処理をする。
	audio_ring_t mix_tmp;
	mix_tmp = mixer->mixbuf;
	mix_tmp.count = 0;

	if (mix_tmp.capacity - mix_tmp.count < count) {
		panic("mix_buf full");
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
	// mixseq はこの時点ではまだ前回の値なのでトラック側へは +1 
	track->seq = mixer->mixseq + 1;
	TRACE(track, "seq=%d, mixed+=%d", (int)track->seq, count);
	return 1;
}

/*
 * ミキシングバッファから物理デバイスバッファへ
 */
void
audio_mixer_play_period(audio_trackmixer_t *mixer /*, bool force */)
{
	/* 今回取り出すフレーム数を決定 */

	int mix_count = audio_ring_unround_count(&mixer->mixbuf);
	int hw_free_count = audio_ring_unround_free_count(&mixer->hwbuf);
	int count = min(mix_count, hw_free_count);
	if (count <= 0) {
		TRACE0("count too short: mix_count=%d hw_free=%d", mix_count, hw_free_count);
		return;
	}
	count = min(count, mixer->frames_per_block);

	mixer->mixseq++;

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
	internal_t *hptr = RING_BOT(internal_t, &mixer->hwbuf);
	for (int i = 0; i < sample_count; i++) {
		*hptr++ = *mptr++;
	}
	audio_ring_appended(&mixer->hwbuf, count);
	unlock(mixer->sc);

	/* 使用済みミキサメモリを次回のために 0 フィルする */
	memset(mptr0, 0, sample_count * sizeof(internal2_t));
	audio_ring_tookfromtop(&mixer->mixbuf, count);

	/* ハードウェアへ通知する */
	TRACE0("start count=%d mixseq=%d hwseq=%d", count, (int)mixer->mixseq, (int)mixer->hwseq);
#if defined(_KERNEL)
	audiostartp(mixer->sc);
#else
	audio_softc_play_start(mixer->sc);
#endif
	mixer->hw_output_counter += count;
}

void
audio_trackmixer_intr(audio_trackmixer_t *mixer, int count)
{
	TRACE0("");
	KASSERT(count != 0);

	mixer->hw_complete_counter += count;
	mixer->hwseq++;

#if !defined(_KERNEL)
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
			|| track->seq > track->mixer->hwseq);

		track->is_draining = false;
		printf("#%d: uio_count=%d trk_count=%d tm=%d mixhw=%d hw_complete=%d\n", track->id,
			(int)track->inputcounter, (int)track->outputcounter,
			(int)track->track_mixer_counter, (int)track->mixer_hw_counter,
			(int)track->hw_complete_counter);
	}
}

/* write の MI 側 */
int
audio_write(struct audio_softc *sc, struct uio *uio, int ioflag, audio_file_t *file)
{
	int error;
	audio_track_t *track = &file->ptrack;
	TRACE(track, "");

#if defined(_KERNEL)
	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->hw_if == NULL)
		return ENXIO;

	if (uio->uio_resid == 0) {
		sc->sc_eof++;
		return 0;
	}

#ifdef AUDIO_PM_IDLE
	if (device_is_active(&sc->dev) || sc->sc_idle)
		device_active(&sc->dev, DVA_SYSTEM);
#endif

	/*
	 * If half-duplex and currently recording, throw away data.
	 */
	// half-duplex で録音中なら、このデータは捨てる。XXX どうするか
	if (!sc->sc_full_duplex && file->rtrack.mode != 0) {
		uio->uio_offset += uio->uio_resid;
		uio->uio_resid = 0;
		DPRINTF(("audio_write: half-dpx read busy\n"));
		return 0;
	}

	// XXX playdrop と PLAY_ALL はちょっと後回し

#endif // _KERNEL

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
audio_waitio(struct audio_softc *sc, kcondvar_t *chan, audio_track_t *track)
{
#if defined(_KERNEL)
	// XXX 自分がいなくなることを想定する必要があるのかどうか
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	/* Wait for pending I/O to complete. */
	error = cv_wait_sig(chan, sc->sc_lock);

	return error;
#else
	// 本当は割り込みハンドラからトラックが消費されるんだけど
	// ここで消費をエミュレート。

	emu_intr_check();

	return 0;
#endif
}

#if !defined(_KERNEL)
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
#endif // !_KERNEL
