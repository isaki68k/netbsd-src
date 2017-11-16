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

/*
 * -N7
 * audio_write {
 *  uioからバッファを作成
 *  audiostartp {
 *   start/trigger ..1ブロック出力/trigger開始
 *  }
 * }
 * audio_pint {
 *  (必要なら)(フィルタ内に留まってるデータから)バッファを作成
 *  start_output ..1ブロック出力
 * }
 *
 * ---
 *
 * audio_write {
 *  audio_trackmixer_play {
 *   audio_trackmixer_mixall {
 *    audio_track_play .. uioからoutbufまでの変換
 *    audio_mixer_play_mix_track ..合成
 *   }
 *   audio_mixer_play_period ..HW変換
 *   if (ready)
 *    audio_trackmixer_output ..1ブロック出力
 *  }
 * }
 * audio2_pintr {
 *  audio_trackmixer_intr {
 *   audio_trackmixer_output ..1ブロック出力
 *   audio_trackmixer_mixall {
 *    audio_track_play .. uioからoutbufまでの変換
 *    audio_mixer_play_mix_track ..合成
 *   }
 *   audio_mixer_play_period
 * }
 */

#define AUDIO_TRACE
#ifdef AUDIO_TRACE
#define TRACE0(fmt, ...)	audio_trace0(__func__, fmt, ## __VA_ARGS__)
#define TRACE(t, fmt, ...)	audio_trace(__func__, t, fmt, ## __VA_ARGS__)
#else
#define TRACE0(fmt, ...)	/**/
#define TRACE(t, fmt, ...)	/**/
#endif

#if defined(_KERNEL)
#define lock(x)					/*とりあえず*/
#define unlock(x)				/*とりあえず*/
#endif

void audio_trace0(const char *funcname, const char *fmt, ...);
void audio_trace(const char *funcname, audio_track_t *track, const char *fmt, ...);
void *audio_realloc(void *memblock, size_t bytes);
void audio_free(void *memblock);
int16_t audio_volume_to_inner(uint8_t v);
uint8_t audio_volume_to_outer(int16_t v);
void audio_track_lock(audio_track_t *track);
void audio_track_unlock(audio_track_t *track);
void audio_trackmixer_output(audio_trackmixer_t *mixer);

#if !defined(_KERNEL)
static int audio_waitio(struct audio_softc *sc, audio_track_t *track);
#endif // !_KERNEL


void
audio_trace0(const char *funcname, const char *fmt, ...)
{
	struct timeval tv;
	va_list ap;

	getmicrotime(&tv);
	printf("%d.%06d ", (int)tv.tv_sec%60, (int)tv.tv_usec);
	printf("%s ", funcname);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

void
audio_trace(const char *funcname, audio_track_t *track, const char *fmt, ...)
{
	struct timeval tv;
	va_list ap;

	getmicrotime(&tv);
	printf("%d.%06d ", (int)tv.tv_sec%60, (int)tv.tv_usec);
	printf("%s #%d ", funcname, track->id);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

/* メモリアロケーションの STUB */

void *
audio_realloc(void *memblock, size_t bytes)
{
	if (memblock != NULL) {
		if (bytes != 0) {
			return kern_realloc(memblock, bytes, M_NOWAIT);
		} else {
			kern_free(memblock);
			return NULL;
		}
	} else {
		if (bytes != 0) {
			return kern_malloc(bytes, M_NOWAIT);
		} else {
			return NULL;
		}
	}
}

void
audio_free(void *memblock)
{
	if (memblock != NULL) {
		kern_free(memblock);
	}
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
	cv_init(&track->outchan, mode == AUMODE_PLAY ? "audiowr" : "audiord");

	// 固定初期値
	track->volume = 256;
	for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
		track->ch_volume[i] = 256;
	}

	// デフォルトフォーマットでセット
	audio_track_set_format(track, &default_format);
}

// track 内のすべてのリソースを解放します。
// track 自身は解放しません。(file 内のメンバとして確保されているため)
void
audio_track_destroy(audio_track_t *track)
{
	audio_free(track->codec.srcbuf.sample);
	audio_free(track->chvol.srcbuf.sample);
	audio_free(track->chmix.srcbuf.sample);
	audio_free(track->freq.srcbuf.sample);
	audio_free(track->outputbuf.sample);
	cv_destroy(&track->outchan);
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
		audio_free(track->codec.srcbuf.sample);
		track->codec.srcbuf.sample = NULL;
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
		track->codec.srcbuf.capacity = frame_per_block_roundup(&track->codec.srcbuf.fmt);
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
		audio_free(track->chvol.srcbuf.sample);
		track->chvol.srcbuf.sample = NULL;
		return last_dst;
	} else {
		track->chvol.filter = audio_track_chvol;
		track->chvol.dst = last_dst;

		// 周波数とチャンネル数がユーザ指定値。
		track->chvol.srcbuf.fmt = *dstfmt;
		track->chvol.srcbuf.capacity = frame_per_block_roundup(&track->chvol.srcbuf.fmt);
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
		audio_free(track->chmix.srcbuf.sample);
		track->chmix.srcbuf.sample = NULL;
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
		track->chmix.srcbuf.capacity = frame_per_block_roundup(&track->chmix.srcbuf.fmt);
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
		audio_free(track->freq.srcbuf.sample);
		track->freq.srcbuf.sample = NULL;
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
		track->freq.srcbuf.capacity = frame_per_block_roundup(&track->freq.srcbuf.fmt);
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
	track->outputbuf.capacity = NBLKOUT * frame_per_block_roundup(&track->outputbuf.fmt);
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
audio_append_silence(audio_track_t *track, audio_ring_t *ring)
{
	KASSERT(track);
	KASSERT(is_internal_format(&ring->fmt));

	if (ring->count == 0) return 0;

	int fpb = frame_per_block_roundup(&ring->fmt);
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
			audio_append_silence(track, stage->dst);
		} else {
			int fpb = frame_per_block_roundup(&stage->dst->fmt);
			if (stage->dst->count < fpb) {
				// ブロックサイズたまるまで処理をしない
				return false;
			}
		}
	}
	return true;
}

static int
audio_track_play_input(audio_track_t *track)
{
	if (track->uio == NULL) return 0;

	/* input の空きバイト数を求める */
	int free_count = audio_ring_unround_free_count(track->input);
	int free_bytelen = free_count * track->inputfmt.channels * track->inputfmt.stride / 8 - track->subframe_buf_used;
	TRACE(track, "free=%d", free_count);

	if (free_bytelen == 0) {
		return EAGAIN;
	}

	// 今回 uiomove するバイト数 */
	int move_bytelen = min(free_bytelen, (int)track->uio->uio_resid);

	// 今回出来上がるフレーム数 */
	int framecount = (move_bytelen + track->subframe_buf_used) * 8 / (track->inputfmt.channels * track->inputfmt.stride);

	// コピー先アドレスは subframe_buf_used で調整する必要がある
	uint8_t *dptr = RING_BOT_UINT8(track->input) + track->subframe_buf_used;
	// min(bytelen, uio->uio_resid) は uiomove が保証している
	int error = uiomove(dptr, move_bytelen, track->uio);
	if (error) {
		panic("uiomove");
	}
	audio_ring_appended(track->input, framecount);
	track->inputcounter += framecount;

	// 今回 input に置いたサブフレームを次回のために求める
	track->subframe_buf_used = move_bytelen - framecount * track->inputfmt.channels * track->inputfmt.stride / 8;
	return 0;
}

/*
 * 再生時の入力データを変換してトラックバッファに投入します。
 */
void
audio_track_play(audio_track_t *track, bool isdrain)
{
	KASSERT(track);

	int track_count_0 = track->outputbuf.count;

	// 入力
	if (audio_track_play_input(track) != 0) {
		if (isdrain == false) return;
	}

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
		audio_append_silence(track, &track->outputbuf);
	}

	if (track->input == &track->outputbuf) {
		track->outputcounter = track->inputcounter;
	} else {
		track->outputcounter += track->outputbuf.count - track_count_0;
	}

#if defined(AUDIO_TRACE)
	char buf[100];
	int n = 0;
	if (track->freq.filter)
		n += snprintf(buf + n, 100 - n, " f=%d", track->freq.srcbuf.count);
	if (track->chmix.filter)
		n += snprintf(buf + n, 100 - n, " m=%d", track->chmix.srcbuf.count);
	if (track->chvol.filter)
		n += snprintf(buf + n, 100 - n, " v=%d", track->chvol.srcbuf.count);
	if (track->codec.filter)
		n += snprintf(buf + n, 100 - n, " e=%d", track->codec.srcbuf.count);
#endif
	TRACE(track, "busy=%d outbuf=%d/%d%s", track->mixer->busy,
		track->outputbuf.count, track->outputbuf.capacity, buf);
}

int
audio_mixer_init(struct audio_softc *sc, audio_trackmixer_t *mixer, int mode)
{
	memset(mixer, 0, sizeof(audio_trackmixer_t));
	mixer->sc = sc;

#if defined(_KERNEL)
	// XXX とりあえず
	if (mode == AUMODE_PLAY)
		mixer->hwbuf.fmt = sc->sc_phwfmt;
	else
		mixer->hwbuf.fmt = sc->sc_rhwfmt;

	int framelen = mixer->hwbuf.fmt.channels * mixer->hwbuf.fmt.stride / NBBY;
	mixer->frames_per_block = frame_per_block_roundup(&mixer->hwbuf.fmt);
	int capacity = mixer->frames_per_block * 16;
	int bufsize = capacity * framelen;
	if (sc->hw_if->round_buffersize) {
		int rounded;
		mutex_enter(sc->sc_lock);
		rounded = sc->hw_if->round_buffersize(sc->hw_hdl, mode, bufsize);
		mutex_exit(sc->sc_lock);
		// 縮められても困る?
		if (rounded != bufsize) {
			aprint_error_dev(sc->dev, "buffer size not configured"
			    " %d -> %d\n", bufsize, rounded);
			return ENXIO;
		}
	}
	mixer->hwbuf.capacity = capacity;

	int blksize = mixer->frames_per_block * framelen;
	if (sc->hw_if->round_blocksize) {
		int rounded;
		audio_params_t p = format2_to_params(&mixer->hwbuf.fmt);
		mutex_enter(sc->sc_lock);
		rounded = sc->hw_if->round_blocksize(sc->hw_hdl, blksize, mode, &p);
		mutex_exit(sc->sc_lock);
		// 違っていても困る?
		if (rounded != blksize) {
			aprint_error_dev(sc->dev, "blksize not configured"
			    " %d -> %d\n", blksize, rounded);
			return ENXIO;
		}
	}

	if (sc->hw_if->allocm) {
		mixer->hwbuf.sample = sc->hw_if->allocm(sc->hw_hdl, mode,
		    bufsize);
	} else {
		mixer->hwbuf.sample = kern_malloc(bufsize, M_NOWAIT);
	}
#else
	TRACE0("");

	mixer->hwbuf.fmt = audio_softc_get_hw_format(mixer->sc, mode);
	mixer->hwbuf.capacity = audio_softc_get_hw_capacity(mixer->sc);
	mixer->hwbuf.sample = sc->hw_if->allocm(NULL, 0, RING_BYTELEN(&mixer->hwbuf));

	mixer->frames_per_block = frame_per_block_roundup(&mixer->hwbuf.fmt);
#endif

	mixer->track_fmt.encoding = AUDIO_ENCODING_SLINEAR_HE;
	mixer->track_fmt.channels = mixer->hwbuf.fmt.channels;
	mixer->track_fmt.sample_rate = mixer->hwbuf.fmt.sample_rate;
	mixer->track_fmt.precision = mixer->track_fmt.stride = AUDIO_INTERNAL_BITS;

	if (mode == AUMODE_PLAY) {
		// 合成バッファ
		mixer->mixfmt = mixer->track_fmt;
		mixer->mixfmt.precision *= 2;
		mixer->mixfmt.stride *= 2;
		int n = mixer->frames_per_block * mixer->mixfmt.channels * mixer->mixfmt.stride / 8;
		mixer->mixsample = audio_realloc(mixer->mixsample, n);
	} else {
		// 合成バッファは使用しない
	}

	mixer->volume = 256;

	cv_init(&mixer->intrcv, "audiodr");
	return 0;
}

void
audio_mixer_destroy(audio_trackmixer_t *mixer, int mode)
{
	struct audio_softc *sc = mixer->sc;

	if (mixer->hwbuf.sample != NULL) {
		if (sc->hw_if->freem) {
			sc->hw_if->freem(sc->hw_hdl, mixer->hwbuf.sample, mode);
		} else {
			kern_free(mixer->hwbuf.sample);
		}
		mixer->hwbuf.sample = NULL;
	}

	if (mode == AUMODE_PLAY) {
		kern_free(mixer->mixsample);
	} else {
		// 合成バッファは使用しない
	}

	// intrcv を cv_destroy() してはいけないっぽい。KASSERT で死ぬ。
}

// 全トラックを req フレーム分合成します。
// 合成が行われれば 1 以上を返す?
static int
audio_trackmixer_mixall(audio_trackmixer_t *mixer, int req, bool isdrain)
{
	audio_file_t *f;
	int mixed = 0;
	SLIST_FOREACH(f, &mixer->sc->sc_files, entry) {
		audio_track_t *track = &f->ptrack;

		if (track->outputbuf.count < req) {
			audio_track_play(track, isdrain);
		}

		// 合成
		if (track->outputbuf.count > 0) {
			mixed = audio_mixer_play_mix_track(mixer, track, req, mixed);
		}
	}
	return mixed;
}

static void
audio2_pintr(void *arg)
{
	struct audio_softc *sc;
	audio_trackmixer_t *mixer;

	sc = arg;
	KASSERT(mutex_owned(sc->sc_intr_lock));

	mixer = sc->sc_pmixer;
	TRACE0("hwbuf.count=%d", mixer->hwbuf.count);

	int count = mixer->frames_per_block;
	audio_ring_tookfromtop(&mixer->hwbuf, count);
	audio_trackmixer_intr(mixer, count);
}

static int
audio2_trigger_output(audio_trackmixer_t *mixer, void *start, void *end, int blksize)
{
	struct audio_softc *sc = mixer->sc;

	KASSERT(sc->hw_if->trigger_output != NULL);
	KASSERT(mutex_owned(sc->sc_intr_lock));

	audio_params_t params;
	// TODO: params 作る
	params = format2_to_params(&mixer->hwbuf.fmt);
	int error = sc->hw_if->trigger_output(sc->hw_hdl, start, end, blksize, audio2_pintr, sc, &params);
	return error;
}

static int
audio2_start_output(audio_trackmixer_t *mixer, void *start, int blksize)
{
	struct audio_softc *sc = mixer->sc;

	KASSERT(sc->hw_if->start_output != NULL);
	KASSERT(mutex_owned(sc->sc_intr_lock));

	int error = sc->hw_if->start_output(sc->hw_hdl, start, blksize, audio2_pintr, sc);
	return error;
}

static void
audio2_halt_output(audio_trackmixer_t *mixer)
{
	struct audio_softc *sc = mixer->sc;

	sc->hw_if->halt_output(sc->hw_hdl);

	sc->sc_pbusy = false;
	mixer->hwbuf.top = 0;
}

// トラックミキサ起動になる可能性のある再生要求
// トラックミキサが起動したら true を返します。
bool
audio_trackmixer_play(audio_trackmixer_t *mixer)
{
	struct audio_softc *sc;

	sc = mixer->sc;
	KASSERT(mutex_owned(sc->sc_lock));

	// トラックミキサが起動していたら、割り込みから駆動されるので true を返しておく
	if (sc->sc_pbusy) return true;

	TRACE0("begin mixseq=%d hwseq=%d hwbuf=%d/%d/%d",
		(int)mixer->mixseq, (int)mixer->hwseq,
		mixer->hwbuf.top, mixer->hwbuf.count, mixer->hwbuf.capacity);

	// ダブルバッファを埋める

	if (mixer->hwbuf.capacity - mixer->hwbuf.count >= mixer->frames_per_block) {
		int mixed = audio_trackmixer_mixall(mixer, 2 * mixer->frames_per_block, false);
		if (mixed == 0) {
			TRACE0("data not mixed");
			return false;
		}

		// バッファの準備ができたら転送。
		mutex_enter(sc->sc_intr_lock);
		audio_mixer_play_period(mixer);
		if (mixer->hwbuf.count >= mixer->frames_per_block * 2) {
			if (sc->sc_pbusy == false) {
				audio_trackmixer_output(mixer);
			}
		}
		mutex_exit(sc->sc_intr_lock);
	}

	TRACE0("end   mixseq=%d hwseq=%d hwbuf=%d/%d/%d",
		(int)mixer->mixseq, (int)mixer->hwseq,
		mixer->hwbuf.top, mixer->hwbuf.count, mixer->hwbuf.capacity);

	return sc->sc_pbusy;
}

/*
* トラックバッファから取り出し、ミキシングします。
* mixed には呼び出し時点までの合成済みトラック数を渡します。
* 戻り値はこの関数終了時での合成済みトラック数)です。
* つまりこのトラックを合成すれば mixed + 1 を返します。
*/
int
audio_mixer_play_mix_track(audio_trackmixer_t *mixer, audio_track_t *track, int req, int mixed)
{
	/* req フレーム貯まるまで待つ */
	if (track->outputbuf.count < req) {
		TRACE(track, "track count(%d) < req(%d); return",
		    track->outputbuf.count, req);
		return mixed;
	}

	// このトラックが処理済みならなにもしない
	if (mixer->mixseq < track->seq) return mixed;

	int count = mixer->frames_per_block;

	KASSERT(audio_ring_unround_count(&track->outputbuf) >= count);

	internal_t *sptr = RING_TOP(internal_t, &track->outputbuf);
	internal2_t *dptr = mixer->mixsample;

	/* 整数倍精度へ変換し、トラックボリュームを適用して加算合成 */
	int sample_count = count * mixer->mixfmt.channels;
	if (mixed == 0) {
		// 最初のトラック合成は代入
		if (track->volume == 256) {
			for (int i = 0; i < sample_count; i++) {
				*dptr++ = ((internal2_t)*sptr++);
			}
		} else {
			for (int i = 0; i < sample_count; i++) {
				*dptr++ = ((internal2_t)*sptr++) * track->volume / 256;
			}
		}
	} else {
		// 2本め以降なら加算合成
		if (track->volume == 256) {
			for (int i = 0; i < sample_count; i++) {
				*dptr++ += ((internal2_t)*sptr++);
			}
		} else {
			for (int i = 0; i < sample_count; i++) {
				*dptr++ += ((internal2_t)*sptr++) * track->volume / 256;
			}
		}
	}

	audio_ring_tookfromtop(&track->outputbuf, count);

	/* トラックバッファを取り込んだことを反映 */
	// mixseq はこの時点ではまだ前回の値なのでトラック側へは +1 
	track->seq = mixer->mixseq + 1;

	// audio_write() に空きが出来たことを通知
	cv_broadcast(&track->outchan);

	TRACE(track, "broadcast; trseq=%d count=%d", (int)track->seq, count);
	return mixed + 1;
}

/*
 * ミキシングバッファから物理デバイスバッファへ
 */
void
audio_mixer_play_period(audio_trackmixer_t *mixer /*, bool force */)
{
	struct audio_softc *sc;

	sc = mixer->sc;
	KASSERT(mutex_owned(sc->sc_intr_lock));

	/* 今回取り出すフレーム数を決定 */

	int mix_count = mixer->frames_per_block;
	int hw_free_count = audio_ring_unround_free_count(&mixer->hwbuf);
	int count = min(mix_count, hw_free_count);
	if (count <= 0) {
		TRACE0("count too short: mix_count=%d hw_free=%d", mix_count, hw_free_count);
		return;
	}

	mixer->mixseq++;

	/* オーバーフロー検出 */
	internal2_t ovf_plus = AUDIO_INTERNAL_T_MAX;
	internal2_t ovf_minus = AUDIO_INTERNAL_T_MIN;

	internal2_t *mptr0 = mixer->mixsample;
	internal2_t *mptr = mptr0;

	int sample_count = count * mixer->mixfmt.channels;
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
	lock(sc);
	mptr = mptr0;
	internal_t *hptr = RING_BOT(internal_t, &mixer->hwbuf);
	for (int i = 0; i < sample_count; i++) {
		*hptr++ = *mptr++;
	}
	audio_ring_appended(&mixer->hwbuf, count);
	unlock(sc);
	TRACE0("hwbuf=%d/%d/%d",
	    mixer->hwbuf.top, mixer->hwbuf.count, mixer->hwbuf.capacity);
}

// ハードウェアバッファから 1 ブロック出力
void
audio_trackmixer_output(audio_trackmixer_t *mixer)
{
	struct audio_softc *sc;
	KASSERT(mixer->hwbuf.count >= mixer->frames_per_block);
	TRACE0("hwbuf=%d/%d/%d",
	    mixer->hwbuf.top, mixer->hwbuf.count, mixer->hwbuf.capacity);

	sc = mixer->sc;

	if (sc->hw_if->trigger_output) {
		if (!sc->sc_pbusy) {
			audio2_trigger_output(
				mixer,
				mixer->hwbuf.sample,
				RING_END_UINT8(&mixer->hwbuf),
				frametobyte(&mixer->hwbuf.fmt, mixer->frames_per_block));
		}
	} else {
		audio2_start_output(
			mixer,
			RING_TOP_UINT8(&mixer->hwbuf),
			frametobyte(&mixer->hwbuf.fmt, mixer->frames_per_block));
	}
	sc->sc_pbusy = true;
}

void
audio_trackmixer_intr(audio_trackmixer_t *mixer, int count)
{
	struct audio_softc *sc;

	sc = mixer->sc;
	KASSERT(mutex_owned(sc->sc_intr_lock));
	KASSERT(count != 0);

	mixer->hw_complete_counter += count;
	mixer->hwseq++;

	// まず出力待ちのシーケンスを出力
	if (mixer->hwbuf.count >= mixer->frames_per_block) {
		audio_trackmixer_output(mixer);

		// 次のバッファを用意する
		int mixed = audio_trackmixer_mixall(mixer, mixer->frames_per_block, true);
		if (mixed == 0) {
			if (sc->hw_if->trigger_output) {
//				audio_append_silence
			}
		} else {
			audio_mixer_play_period(mixer);
		}
	} else {
		audio2_halt_output(mixer);
		mixer->busy = false;
	}

	// drain 待ちしている人のために通知
	cv_broadcast(&mixer->intrcv);
	TRACE0("++hwsec=%d cmplcnt=%d hwbuf=%d/%d/%d",
		(int)mixer->hwseq,
		(int)mixer->hw_complete_counter,
		mixer->hwbuf.top, mixer->hwbuf.count, mixer->hwbuf.capacity);

}

#if !defined(_KERNEL)
void
audio_track_play_drain(audio_track_t *track, bool wait)
{
	// 割り込みエミュレートしているときはメインループに制御を戻さないといけない
	audio_trackmixer_t *mixer = track->mixer;
	struct audio_softc *sc = mixer->sc;
	mutex_enter(sc->sc_lock);
	audio_track_play_drain_core(track, wait);
	mutex_exit(sc->sc_lock);
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
	audio_trackmixer_t *mixer = track->mixer;
	struct audio_softc *sc = mixer->sc;
	int error;

	TRACE(track, "");
	KASSERT(mutex_owned(sc->sc_lock));
	//KASSERT(!mutex_owned(sc->sc_intr_lock));

	track->is_draining = true;

	// 無音挿入させる
	audio_track_play(track, true);

	/* フレームサイズ未満のため待たされていたデータを破棄 */
	track->subframe_buf_used = 0;

	if (wait) {
		while (track->seq > mixer->hwseq) {
			error = cv_wait_sig(&mixer->intrcv, sc->sc_lock);
			if (error) {
				printf("cv_wait_sig failed %d\n", error);
				return;
			}
		}

		track->is_draining = false;
		TRACE(track, "uio_count=%d trk_count=%d tm=%d mixhw=%d hw_complete=%d",
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
	TRACE(track, "resid=%u", (int)uio->uio_resid);

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

#if defined(_KERNEL)
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

	track->uio = uio;

	error = 0;
	bool wake = false;
	while (uio->uio_resid > 0) {
		if (track->input->capacity - track->input->count == 0) {
			error = audio_waitio(sc, track);
			if (error < 0) {
				error = EINTR;
			}
			if (error) {
				break;
			}
		}
		audio_track_play(track, false);

		if (wake == false) {
			wake = audio_trackmixer_play(sc->sc_pmixer);
		}


#if !defined(_KERNEL)
		emu_intr_check();
#endif
	}

	// finally
	track->uio = NULL;
	return error;
}

static int
audio_waitio(struct audio_softc *sc, audio_track_t *track)
{
	// XXX 自分がいなくなることを想定する必要があるのかどうか
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	TRACE(track, "wait");
	/* Wait for pending I/O to complete. */
	error = cv_wait_sig(&track->outchan, sc->sc_lock);

	TRACE(track, "error=%d", error);
	return error;
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

	mutex_enter(file->sc->sc_lock);
	int error = audio_write(file->sc, &uio, 0, file);
	mutex_exit(file->sc->sc_lock);
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
		audio_track_init(&file->ptrack, sc->sc_pmixer, AUMODE_PLAY);
	} else {
		audio_track_init(&file->rtrack, sc->sc_rmixer, AUMODE_RECORD);
	}

	SLIST_INSERT_HEAD(&sc->sc_files, file, entry);

	return file;
}
#endif // !_KERNEL
