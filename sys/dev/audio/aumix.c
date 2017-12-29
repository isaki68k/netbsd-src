#if defined(_KERNEL)
#include <dev/audio/aumix.h>
#include <dev/audio/auring.h>
#include <dev/audio/aucodec.h>
#include <sys/intr.h>
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
 * audio_open {
 *  audio_pmixer_start {
 *   audio_pmixer_process.. 全トラック合成・HWバッファ作成
 *   audio_pmixer_output .. 1ブロック出力
 *  }
 * }
 * audio2_pintr {
 *  audio_pmixer_intr {
 *   audio_pmixer_output ..1ブロック出力
 *   audio_pmixer_process.. 全トラック合成・HWバッファ作成
 * }
 * audio_write {
 *  audio_track_play .. トラックバッファ作成
 * }
 */

#if defined(_KERNEL)
#define lock(x)					/*とりあえず*/
#define unlock(x)				/*とりあえず*/
#endif

#define audio_free(mem)	do {	\
	if (mem != NULL) {	\
		kern_free(mem);	\
		mem = NULL;	\
	}	\
} while (0)

void *audio_realloc(void *memblock, size_t bytes);
int16_t audio_volume_to_inner(uint8_t v);
uint8_t audio_volume_to_outer(int16_t v);
static int audio_pmixer_mixall(audio_trackmixer_t *mixer, int req, bool isintr);
void audio_pmixer_output(audio_trackmixer_t *mixer);
#if defined(AUDIO_SOFTINTR)
static void audio_pmixer_softintr(void *arg);
#endif
static int audio2_trigger_output(audio_trackmixer_t *mixer, void *start,
	void *end, int blksize);
static int audio2_start_output(audio_trackmixer_t *mixer, void *start,
	int blksize);

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

#if !defined(AUDIO_SOFTINTR)
// track_cl フラグをセットします。
// 割り込みコンテキストから呼び出すことはできません。
static inline void
audio_track_cl(struct audio_softc *sc, audio_track_t *track)
{
	mutex_enter(sc->sc_intr_lock);
	track->track_cl = 1;
	mutex_exit(sc->sc_intr_lock);
}

// track_cl フラグをリセットします。
// 割り込みコンテキストから呼び出すことはできません。
static inline void
audio_track_uncl(struct audio_softc *sc, audio_track_t *track)
{
	mutex_enter(sc->sc_intr_lock);
	track->track_cl = 0;
	mutex_exit(sc->sc_intr_lock);
}
#endif

static inline void
audio_track_enter_colock(struct audio_softc *sc, audio_track_t *track)
{
	KASSERT(track);

#if defined(AUDIO_SOFTINTR)
	mutex_enter(&track->mixer->softintrlock);
#else
	audio_track_cl(sc, track);
#endif
}

static inline void
audio_track_leave_colock(struct audio_softc *sc, audio_track_t *track)
{
	KASSERT(track);

#if defined(AUDIO_SOFTINTR)
	mutex_exit(&track->mixer->softintrlock);
#else
	audio_track_uncl(sc, track);
#endif
}

static inline kmutex_t *
audio_mixer_get_lock(audio_trackmixer_t *mixer)
{
#if defined(AUDIO_SOFTINTR)
	return &mixer->softintrlock;
#else
	return mixer->sc->sc_intr_lock;
#endif
}

static inline void
audio_mixer_enter_lock(audio_trackmixer_t *mixer)
{
	mutex_enter(audio_mixer_get_lock(mixer));
}

static inline void
audio_mixer_leave_lock(audio_trackmixer_t *mixer)
{
	mutex_exit(audio_mixer_get_lock(mixer));
}


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
#if false
		internal2_t s;
		s = (internal2_t)sptr[0];
		s += (internal2_t)sptr[1];
		*dptr = s / 2;
#else
		*dptr = sptr[0] / 2 + sptr[1] / 2;
#endif
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

// AUDIO_ASSERT なしで main.c による計測。@ amd64 (nao)
//
// src->dst	44->48	8->48	48->44	48->8	[times/msec]
// ORIG		 48.7	 60.2	 91.8	610.6
// CYCLE2	 65.9	 78.8	154.4	837.8

static void
audio_track_freq_up(audio_filter_arg_t *arg)
{
	audio_track_t *track = arg->context;
	audio_ring_t *src = &track->freq.srcbuf;
	audio_ring_t *dst = track->freq.dst;

	KASSERT(track);
	KASSERT(is_valid_ring(dst));
	KASSERT(is_valid_ring(src));
	KASSERT(src->count > 0);
	KASSERT(src->fmt.channels == dst->fmt.channels);
	KASSERT(src->top == 0);

	const internal_t *sptr = arg->src;
	internal_t *dptr = arg->dst;
#if defined(FREQ_CYCLE2)
	unsigned int t = track->freq_current;
	int step = track->freq_step;

	for (int i = 0; i < arg->count && src->count > 0; i++) {
		internal_t curr;
		internal_t next;
		internal_t a;
		internal2_t diff;

		for (int ch = 0; ch < dst->fmt.channels; ch++) {
			curr = sptr[ch];
			next = sptr[ch + src->fmt.channels];
			a = next - curr;
			diff = a * t / 65536;
			*dptr++ = curr + diff;
		}
		dst->count++;

		t += step;
		if (t >= 65536) {
			sptr += src->fmt.channels;
			src->top++;
			src->count--;
			t -= 65536;
		}
	}
	// 補正
	t += track->freq_leap;
	if (t >= 65536) {
		src->top++;
		src->count--;
		t -= 65536;
	}
	track->freq_current = t;

#elif defined(FREQ_ORIG)
	audio_rational_t tmp = track->freq_current;
	const internal_t *sptr1;

	for (int i = 0; i < arg->count; i++) {
		if (tmp.n == 0) {
			if (src->count <= 0) {
				break;
			}
			for (int ch = 0; ch < dst->fmt.channels; ch++) {
				*dptr++ = sptr[ch];
			}
		} else {
			sptr1 = sptr + src->fmt.channels;
			if (src->count <= 1) {
				break;
			}
			// 加算前の下駄 2^22 を脱ぐ
			int b = tmp.n * track->freq_coef / (1 << 22);
			int a = 256 - b;
			for (int ch = 0; ch < dst->fmt.channels; ch++) {
				// 加算後の下駄 2^8 を脱ぐ
				*dptr++ = (sptr[ch] * a + sptr1[ch] * b) / 256;
			}
		}
		dst->count++;
		audio_rational_add(&tmp, &track->freq_step, dst->fmt.sample_rate);
		if (tmp.i > 0) {
			// 周波数を上げるので、ソース側は 1 以下のステップが保証されている
			KASSERT(tmp.i == 1);
			sptr += src->fmt.channels;
			tmp.i = 0;
			src->top++;
			src->count--;
		}
	}

	track->freq_current = tmp;
#else
#error unknown FREQ
#endif
}

static void
audio_track_freq_down(audio_filter_arg_t *arg)
{
	audio_track_t *track = arg->context;
	audio_ring_t *src = &track->freq.srcbuf;
	audio_ring_t *dst = track->freq.dst;

	KASSERT(track);
	KASSERT(is_valid_ring(dst));
	KASSERT(is_valid_ring(src));
	KASSERT(src->count > 0);
	KASSERT(src->fmt.channels == dst->fmt.channels);
	KASSERT(src->top == 0);

	const internal_t *sptr0 = arg->src;
	internal_t *dptr = arg->dst;
#if defined(FREQ_CYCLE2)
	unsigned int t = track->freq_current;
	unsigned int step = track->freq_step;
	int nch = dst->fmt.channels;

	for (int i = 0; i < arg->count && t / 65536 < src->count; i++) {
		const internal_t *sptr1;
		sptr1 = sptr0 + (t / 65536) * nch;
		for (int ch = 0; ch < nch; ch++) {
			*dptr++ = sptr1[ch];
		}
		t += step;
	}
	dst->count += arg->count;
	t += track->freq_leap;
	// XXX うーんなんだこの min
	audio_ring_tookfromtop(src, min(t / 65536, src->count));
	track->freq_current = t % 65536;

#elif defined(FREQ_ORIG)
	audio_rational_t tmp = track->freq_current;
	const internal_t *sptr1;
	int src_taken = -1;

	for (int i = 0; i < arg->count; i++) {
//#define AUDIO_FREQ_HQ
#if defined(AUDIO_FREQ_HQ)
		if (tmp.n == 0) {
			if (tmp.i >= src->count) {
				break;
			}
			sptr1 = sptr0 + tmp.i * src->fmt.channels;
			for (int ch = 0; ch < dst->fmt.channels; ch++) {
				*dptr++ = sptr1[ch];
			}
		} else {
			const internal_t *sptr2;
			if (tmp.i + 1 >= src->count) {
				break;
			}
			sptr1 = sptr0 + tmp.i * src->fmt.channels;
			sptr2 = sptr1 + src->fmt.channels;
			// 加算前の下駄 2^22 を脱ぐ
			int b = tmp.n * track->freq_coef / (1 << 22);
			int a = 256 - b;
			for (int ch = 0; ch < dst->fmt.channels; ch++) {
				// 加算後の下駄 2^8 を脱ぐ
				*dptr++ = (sptr1[ch] * a + sptr2[ch] * b) / 256;
			}
		}
#else
		if (tmp.i >= src->count) {
			break;
		}
		sptr1 = sptr0 + tmp.i * src->fmt.channels;
		for (int ch = 0; ch < dst->fmt.channels; ch++) {
			*dptr++ = sptr1[ch];
		}
#endif
		dst->count++;
		src_taken = tmp.i;
		audio_rational_add(&tmp, &track->freq_step, dst->fmt.sample_rate);
	}
	audio_ring_tookfromtop(src, src_taken + 1);
	tmp.i = 0;
	track->freq_current = tmp;
#else
#error unknown FREQ
#endif
}

// トラックを初期化します。
// 初期化できれば 0 を返して *trackp に初期化済みのトラックを格納します。
// 初期化できなければ errno を返し、*trackp は変更しません。
// mode は再生なら AUMODE_PLAY、録音なら AUMODE_RECORD を指定します。
// 単に録音再生のどちら側かだけなので AUMODE_PLAY_ALL は関係ありません。
int
audio_track_init(struct audio_softc *sc, audio_track_t **trackp, int mode)
{
	audio_track_t *track;
	audio_format2_t *default_format;
	audio_trackmixer_t *mixer;
	const char *cvname;
	int error;
	static int newid = 0;

	track = kmem_zalloc(sizeof(*track), KM_SLEEP);

	track->id = newid++;
	// ここだけ id が決まってから表示
	TRACE(track, "");

	if (mode == AUMODE_PLAY) {
		cvname = "audiowr";
		default_format = &sc->sc_pparams;
		mixer = sc->sc_pmixer;
	} else {
		cvname = "audiord";
		default_format = &sc->sc_rparams;
		mixer = sc->sc_rmixer;
	}

	track->mixer = mixer;
	track->mode = mode;
	cv_init(&track->outchan, cvname);
#if !defined(AUDIO_SOFTINTR)
	track->track_cl = 0;
#endif

	// 固定初期値
	track->volume = 256;
	for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
		track->ch_volume[i] = 256;
	}

	// デフォルトフォーマットでセット
	audio_mixer_enter_lock(mixer);
	error = audio_track_set_format(track, default_format);
	audio_mixer_leave_lock(mixer);
	if (error)
		goto error;

	*trackp = track;
	return 0;

error:
	audio_track_destroy(track);
	return error;
}

// track のすべてのリソースと track 自身を解放します。
void
audio_track_destroy(audio_track_t *track)
{
	// 関数仕様を track は NULL 許容にしてもいいけど、これを呼ぶところは
	// たいてい track が NULL でないと分かっていて呼んでるはずなので
	// ASSERT のほうがよかろう。
	KASSERT(track);

	audio_free(track->usrbuf.sample);
	audio_free(track->codec.srcbuf.sample);
	audio_free(track->chvol.srcbuf.sample);
	audio_free(track->chmix.srcbuf.sample);
	audio_free(track->freq.srcbuf.sample);
	audio_free(track->outputbuf.sample);
	cv_destroy(&track->outchan);

	kmem_free(track, sizeof(*track));
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

// track の codec ステージを必要に応じて初期化します。
// 成功すれば、codec ステージが必要なら初期化した上で、いずれにしても
// 更新された last_dst を返します。
// 失敗すれば(今のところ ENOMEM のみ) NULL を返します。
static audio_ring_t *
init_codec(audio_track_t *track, audio_ring_t *last_dst)
{
	KASSERT(track);

	audio_format2_t *srcfmt = &track->inputfmt;
	audio_format2_t *dstfmt = &last_dst->fmt;

	if (srcfmt->encoding != dstfmt->encoding
	 || srcfmt->precision != dstfmt->precision
	 || srcfmt->stride != dstfmt->stride) {
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
		track->codec.srcbuf.capacity = frame_per_block_roundup(track->mixer, &track->codec.srcbuf.fmt);
		track->codec.srcbuf.sample = audio_realloc(track->codec.srcbuf.sample, RING_BYTELEN(&track->codec.srcbuf));
		if (track->codec.srcbuf.sample == NULL) {
			DPRINTF(1, "%s: malloc(%d) failed\n", __func__,
			    RING_BYTELEN(&track->codec.srcbuf));
			last_dst = NULL;
			goto done;
		}

		return &track->codec.srcbuf;
	}

done:
	track->codec.filter = NULL;
	audio_free(track->codec.srcbuf.sample);
	return last_dst;
}

// track の chvol ステージを必要に応じて初期化します。
// 成功すれば、chvol ステージが必要なら初期化した上で、いずれにしても
// 更新された last_dst を返します。
// 失敗すれば(今のところ ENOMEM のみ) NULL を返します。
static audio_ring_t *
init_chvol(audio_track_t *track, audio_ring_t *last_dst)
{
	KASSERT(track);

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

	if (use_chvol == true) {
		track->chvol.filter = audio_track_chvol;
		track->chvol.dst = last_dst;

		// 周波数とチャンネル数がユーザ指定値。
		track->chvol.srcbuf.fmt = *dstfmt;
		track->chvol.srcbuf.capacity = frame_per_block_roundup(track->mixer, &track->chvol.srcbuf.fmt);
		track->chvol.srcbuf.sample = audio_realloc(track->chvol.srcbuf.sample, RING_BYTELEN(&track->chvol.srcbuf));
		if (track->chvol.srcbuf.sample == NULL) {
			DPRINTF(1, "%s: malloc(%d) failed\n", __func__,
			    RING_BYTELEN(&track->chvol.srcbuf));
			last_dst = NULL;
			goto done;
		}

		track->chvol.arg.count = track->chvol.srcbuf.capacity;
		track->chvol.arg.context = track->ch_volume;
		return &track->chvol.srcbuf;
	}

done:
	track->chvol.filter = NULL;
	audio_free(track->chvol.srcbuf.sample);
	return last_dst;
}


// track の chmix ステージを必要に応じて初期化します。
// 成功すれば、chmix ステージが必要なら初期化した上で、いずれにしても
// 更新された last_dst を返します。
// 失敗すれば(今のところ ENOMEM のみ) NULL を返します。
static audio_ring_t *
init_chmix(audio_track_t *track, audio_ring_t *last_dst)
{
	KASSERT(track);

	audio_format2_t *srcfmt = &track->inputfmt;
	audio_format2_t *dstfmt = &last_dst->fmt;

	int srcch = srcfmt->channels;
	int dstch = dstfmt->channels;

	if (srcch != dstch) {
		if (srcch >= 2 && dstch == 1) {
			track->chmix.filter = audio_track_chmix_mixLR;
		} else if (srcch == 1 && dstch >= 2) {
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
		track->chmix.srcbuf.capacity = frame_per_block_roundup(track->mixer, &track->chmix.srcbuf.fmt);
		track->chmix.srcbuf.sample = audio_realloc(track->chmix.srcbuf.sample, RING_BYTELEN(&track->chmix.srcbuf));
		if (track->chmix.srcbuf.sample == NULL) {
			DPRINTF(1, "%s: malloc(%d) failed\n", __func__,
			    RING_BYTELEN(&track->chmix.srcbuf));
			last_dst = NULL;
			goto done;
		}

		track->chmix.arg.srcfmt = &track->chmix.srcbuf.fmt;
		track->chmix.arg.dstfmt = dstfmt;

		return &track->chmix.srcbuf;
	}

done:
	track->chmix.filter = NULL;
	audio_free(track->chmix.srcbuf.sample);
	return last_dst;
}

// track の freq ステージを必要に応じて初期化します。
// 成功すれば、freq ステージが必要なら初期化した上で、いずれにしても
// 更新された last_dst を返します。
// 失敗すれば(今のところ ENOMEM のみ) NULL を返します。
static audio_ring_t *
init_freq(audio_track_t *track, audio_ring_t *last_dst)
{
	KASSERT(track);

	audio_format2_t *srcfmt = &track->inputfmt;
	audio_format2_t *dstfmt = &last_dst->fmt;

	uint32_t srcfreq = srcfmt->sample_rate;
	uint32_t dstfreq = dstfmt->sample_rate;

	if (srcfreq != dstfreq) {
		track->freq.arg.context = track;
		track->freq.arg.srcfmt = &track->freq.srcbuf.fmt;
		track->freq.arg.dstfmt = &last_dst->fmt;

#if defined(FREQ_CYCLE2)
		track->freq_current = 0;

		// freq_step は dstfreq を 65536 とした時の src/dst 比
		track->freq_step = (uint64_t)srcfreq * 65536 / dstfreq;

		// freq_leap は1ブロックごとの freq_step の補正値
		// を四捨五入したもの。
		int dst_capacity = frame_per_block_roundup(track->mixer,
		    dstfmt);
		int mod = (uint64_t)srcfreq * 65536 % dstfreq;
		track->freq_leap = (mod * dst_capacity + dstfreq / 2) / dstfreq;

		if (track->freq_step < 65536) {
#elif defined(FREQ_ORIG)
		track->freq_step.i = srcfreq / dstfreq;
		track->freq_step.n = srcfreq % dstfreq;
		audio_rational_clear(&track->freq_current);
		// 除算をループから追い出すため、変換先周波数の逆数を求める。
		// 変換先周波数の逆数を << (8 + 22) したもの
		// 8 は加算後の下駄で、22 は加算前の下駄。
		track->freq_coef = (1 << (8 + 22)) / dstfreq;

		if (srcfreq < dstfreq) {
#else
#error unknown FREQ
#endif
			track->freq.filter = audio_track_freq_up;
		} else {
			track->freq.filter = audio_track_freq_down;
		}
		track->freq.dst = last_dst;
		// 周波数のみ srcfreq
		track->freq.srcbuf.fmt = *dstfmt;
		track->freq.srcbuf.fmt.sample_rate = srcfreq;
		track->freq.srcbuf.top = 0;
		track->freq.srcbuf.count = 0;
		track->freq.srcbuf.capacity = frame_per_block_roundup(track->mixer, &track->freq.srcbuf.fmt);
		track->freq.srcbuf.sample = audio_realloc(track->freq.srcbuf.sample, RING_BYTELEN(&track->freq.srcbuf));
		if (track->freq.srcbuf.sample == NULL) {
			DPRINTF(1, "%s: malloc(%d) failed\n", __func__,
			    RING_BYTELEN(&track->freq.srcbuf));
			last_dst = NULL;
			goto done;
		}
		return &track->freq.srcbuf;
	}

done:
	track->freq.filter = NULL;
	audio_free(track->freq.srcbuf.sample);
	return last_dst;
}

// トラックのユーザランド側フォーマットを設定します。
// 変換用内部バッファは一度破棄されます。
int
audio_track_set_format(audio_track_t *track, audio_format2_t *fmt)
{
	KASSERT(track);

	TRACE(track, "");
	KASSERT(is_valid_format(fmt));
	KASSERT(mutex_owned(audio_mixer_get_lock(track->mixer)));

	// 入力値チェック
#if defined(_KERNEL)
	// XXX audio.c にある。どうしたもんか
	audio_check_params2(fmt);
#endif

	// TODO: まず現在のバッファとかを全部破棄すると分かり易いが。

	audio_ring_t *last_dst = &track->outputbuf;
	if (audio_track_is_playback(track)) {
		// 再生はトラックミキサ側から作る

		track->inputfmt = *fmt;
		track->outputbuf.fmt =  track->mixer->track_fmt;

		if ((last_dst = init_freq(track, last_dst)) == NULL)
			goto error;
		if ((last_dst = init_chmix(track, last_dst)) == NULL)
			goto error;
		if ((last_dst = init_chvol(track, last_dst)) == NULL)
			goto error;
		if ((last_dst = init_codec(track, last_dst)) == NULL)
			goto error;
	} else {
		// 録音はユーザランド側から作る

		track->inputfmt = track->mixer->track_fmt;
		track->outputbuf.fmt = *fmt;

		if ((last_dst = init_codec(track, last_dst)) == NULL)
			goto error;
		if ((last_dst = init_chvol(track, last_dst)) == NULL)
			goto error;
		if ((last_dst = init_chmix(track, last_dst)) == NULL)
			goto error;
		if ((last_dst = init_freq(track, last_dst)) == NULL)
			goto error;
	}

	// 入力バッファは先頭のステージ相当品
	track->input = last_dst;

	// 入力フォーマットに従って usrbuf を作る
	track->usrbuf.top = 0;
	track->usrbuf.count = 0;
	track->usrbuf.capacity = NBLKOUT *
	    frametobyte(&track->inputfmt, track->input->capacity);
	track->usrbuf.sample = audio_realloc(track->usrbuf.sample,
	    track->usrbuf.capacity);
	if (track->usrbuf.sample == NULL) {
		DPRINTF(1, "%s: malloc usrbuf(%d) failed\n", __func__,
		    track->usrbuf.capacity);
		goto error;
	}
	// usrbuf の fmt は1フレーム=1バイトになるようにしておくが
	// 基本 fmt は参照せず 1フレーム=1バイトでコーディングしたほうがいいか。
	track->usrbuf.fmt = *fmt;
	track->usrbuf.fmt.channels = 1;
	track->usrbuf.fmt.precision = 8;
	track->usrbuf.fmt.stride = 8;

	// 出力フォーマットに従って outputbuf を作る
	track->outputbuf.top = 0;
	track->outputbuf.count = 0;
	track->outputbuf.capacity = NBLKOUT * frame_per_block_roundup(track->mixer, &track->outputbuf.fmt);
	track->outputbuf.sample = audio_realloc(track->outputbuf.sample, RING_BYTELEN(&track->outputbuf));
	if (track->outputbuf.sample == NULL) {
		DPRINTF(1, "%s: malloc outbuf(%d) failed\n", __func__,
		    RING_BYTELEN(&track->outputbuf));
		goto error;
	}

#if AUDIO_DEBUG > 1
	char buf[100];
	int n;
	n = snprintf(buf, sizeof(buf), " out=%d",
	    track->outputbuf.capacity *
	    frametobyte(&track->outputbuf.fmt, 1));
	if (track->freq.filter)
		n += snprintf(buf + n, sizeof(buf) - n, " freq=%d",
		    track->freq.srcbuf.capacity *
		    frametobyte(&track->freq.srcbuf.fmt, 1));
	if (track->chmix.filter)
		n += snprintf(buf + n, sizeof(buf) - n, " chmix=%d",
		    track->chmix.srcbuf.capacity *
		    frametobyte(&track->chmix.srcbuf.fmt, 1));
	if (track->chvol.filter)
		n += snprintf(buf + n, sizeof(buf) - n, " chvol=%d",
		    track->chvol.srcbuf.capacity *
		    frametobyte(&track->chvol.srcbuf.fmt, 1));
	if (track->codec.filter)
		n += snprintf(buf + n, sizeof(buf) - n, " codec=%d",
		    track->codec.srcbuf.capacity *
		    frametobyte(&track->codec.srcbuf.fmt, 1));
	n += snprintf(buf + n, sizeof(buf) - n, " usr=%d",
	    track->usrbuf.capacity);
	DPRINTF(1, "%s: bufsize:%s\n", __func__, buf);
#endif
	return 0;

error:
	audio_free(track->usrbuf.sample);
	audio_free(track->codec.srcbuf.sample);
	audio_free(track->chvol.srcbuf.sample);
	audio_free(track->chmix.srcbuf.sample);
	audio_free(track->freq.srcbuf.sample);
	audio_free(track->outputbuf.sample);
	return ENOMEM;
}

// ring が空でなく 1 ブロックに満たない時、1ブロックまで無音を追加します。
// 追加したフレーム数を返します。
static int
audio_append_silence(audio_track_t *track, audio_ring_t *ring)
{
	KASSERT(track);
	KASSERT(is_internal_format(&ring->fmt));

	if (ring->count == 0) return 0;

	int fpb = frame_per_block_roundup(track->mixer, &ring->fmt);
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
static void
audio_apply_stage(audio_track_t *track, audio_stage_t *stage, bool isfreq)
{
	KASSERT(track);

	if (stage->filter != NULL) {
		int srccount = audio_ring_unround_count(&stage->srcbuf);
		int dstcount = audio_ring_unround_free_count(stage->dst);
		
		int count;
		if (isfreq) {
			if (srccount == 0) {
				panic("freq but srccount=0");
			}
			count = min(dstcount, track->mixer->frames_per_block);
		} else {
			count = min(srccount, dstcount);
		}

		if (count > 0) {
			audio_filter_arg_t *arg = &stage->arg;

			arg->src = RING_TOP_UINT8(&stage->srcbuf);
			arg->dst = RING_BOT_UINT8(stage->dst);
			arg->count = count;

			stage->filter(arg);

			if (!isfreq) {
				audio_ring_tookfromtop(&stage->srcbuf, count);
				audio_ring_appended(stage->dst, count);
			}
		}
	}
}

/*
 * 再生時の入力データを変換してトラックバッファに投入します。
 */
void
audio_track_play(audio_track_t *track, bool isdrain)
{
	int inpbuf_frames_per_block;
	int blockbytes;	// usrbuf および input の1ブロックのバイト数
	int movebytes;	// usrbuf から input に転送するバイト数

	KASSERT(track);

	int track_count_0 = track->outputbuf.count;

	inpbuf_frames_per_block = frame_per_block_roundup(track->mixer,
	    &track->input->fmt);
	blockbytes = frametobyte(&track->input->fmt, inpbuf_frames_per_block);

	// usrbuf が1ブロックに満たない場合、
	// - drain 中なら、足りない分を無音で埋めて処理を続ける
	// - PLAY なら、足りない分を無音で埋めて処理を続ける
	// - PLAY_ALL なら、何もせず帰る
	if (track->usrbuf.count < blockbytes) {
		if (isdrain == false && (track->mode & AUMODE_PLAY_ALL) != 0) {
			return;
		}
	}

	// usrbuf からコピー。

	int count = audio_ring_unround_free_count(track->input);
#if 0
	// XXX 本当は input 以降のバッファが1ブロック以下で細切れにならない
	// ように(できれば)したほうがいいと思うのだが、今は周波数変換の
	// 端数が出るので、これは起こりうる。
	if (count < inpbuf_frames_per_block) {
		panic("count(%d) < inpbuf_frames_per_block(%d)",
		    count, inpbuf_frames_per_block);
	}
#endif
	// input バッファに空きがない
	if (count == 0) {
		return;
	}
	count = min(count, inpbuf_frames_per_block);
	// 入力に4bitは来ないので1フレームは必ず1バイト以上ある。
	int framesize = frametobyte(&track->input->fmt, 1);
	// count は usrbuf からコピーするフレーム数。
	// movebytes は usrbuf からコピーするバイト数。
	// ただし1フレーム未満のバイトはコピーしない。
	count = min(count, track->usrbuf.count / framesize);
	movebytes = count * framesize;
	if (track->usrbuf.top + movebytes < track->usrbuf.capacity) {
		memcpy(RING_BOT(internal_t, track->input),
		    (uint8_t *)track->usrbuf.sample + track->usrbuf.top,
		    movebytes);
		audio_ring_tookfromtop(&track->usrbuf, movebytes);
	} else {
		int bytes1 = audio_ring_unround_count(&track->usrbuf);
		memcpy(RING_BOT(internal_t, track->input),
		    (uint8_t *)track->usrbuf.sample + track->usrbuf.top,
		    bytes1);
		audio_ring_tookfromtop(&track->usrbuf, bytes1);

		int bytes2 = movebytes - bytes1;
		memcpy((uint8_t *)(RING_BOT(internal_t, track->input)) + bytes1,
		    (uint8_t *)track->usrbuf.sample + track->usrbuf.top,
		    bytes2);
		audio_ring_tookfromtop(&track->usrbuf, bytes2);
	}
	audio_ring_appended(track->input, count);

	/* エンコーディング変換 */
	audio_apply_stage(track, &track->codec, false);

	/* チャンネルボリューム */
	audio_apply_stage(track, &track->chvol, false);

	/* チャンネルミキサ */
	audio_apply_stage(track, &track->chmix, false);

	/* 周波数変換 */
	if (track->freq.filter != NULL) {
		int n = 0;
		if (isdrain) {
			n = audio_append_silence(track, &track->freq.srcbuf);
			if (n > 0) {
				TRACE(track, "freq.srcbuf appended silence %d frames", n);
			}
		}
		if (track->freq.srcbuf.count > 0) {
			audio_apply_stage(track, &track->freq, true);
			// freq の入力はバッファ先頭から。
			// サブフレームの問題があるので、top 位置以降の全域をずらす。
			if (track->freq.srcbuf.top != 0) {
#if defined(AUDIO_DEBUG)
				if (track->freq.srcbuf.top + track->freq.srcbuf.count > track->freq.srcbuf.capacity) {
					panic("srcbuf broken, %d/%d/%d\n",
						track->freq.srcbuf.top,
						track->freq.srcbuf.count,
						track->freq.srcbuf.capacity);
				}
#endif
				uint8_t *s = track->freq.srcbuf.sample;
				uint8_t *p = RING_TOP_UINT8(&track->freq.srcbuf);
				uint8_t *e = RING_END_UINT8(&track->freq.srcbuf);
				memmove(s, p, e - p);
				track->freq.srcbuf.top = 0;
			}
		}
		if (n > 0 && track->freq.srcbuf.count > 0) {
			TRACE(track, "freq.srcbuf cleanup count=%d", track->freq.srcbuf.count);
			track->freq.srcbuf.count = 0;
		}
	}

	if (isdrain) {
		/* 無音をブロックサイズまで埋める */
		/* 内部フォーマットだとわかっている */
		/* 周波数変換の結果、ブロック未満の端数フレームが出ることもあるし、
		変換なしのときは入力自体が半端なときもあろう */
		int n = audio_append_silence(track, &track->outputbuf);
		if (n > 0) {
			TRACE(track, "track.outputbuf appended silence %d frames", n);
		}
	}

	if (track->input == &track->outputbuf) {
		track->outputcounter = track->inputcounter;
	} else {
		track->outputcounter += track->outputbuf.count - track_count_0;
	}

#if AUDIO_DEBUG > 2
	char buf[100];
	int n = 0;
	buf[n] = '\0';
	if (track->freq.filter)
		n += snprintf(buf + n, 100 - n, " f=%d", track->freq.srcbuf.count);
	if (track->chmix.filter)
		n += snprintf(buf + n, 100 - n, " m=%d", track->chmix.srcbuf.count);
	if (track->chvol.filter)
		n += snprintf(buf + n, 100 - n, " v=%d", track->chvol.srcbuf.count);
	if (track->codec.filter)
		n += snprintf(buf + n, 100 - n, " e=%d", track->codec.srcbuf.count);
	TRACE(track, "end out=%d/%d/%d%s usr=%d/%d/%d",
	    track->outputbuf.top, track->outputbuf.count, track->outputbuf.capacity,
	    buf,
		track->usrbuf.top, track->usrbuf.count, track->usrbuf.capacity
	);
#endif
}

// ミキサを初期化します。
// mode は再生なら AUMODE_PLAY、録音なら AUMODE_RECORD を指定します。
// 単に録音再生のどちら側かだけなので AUMODE_PLAY_ALL は関係ありません。
int
audio_mixer_init(struct audio_softc *sc, audio_trackmixer_t *mixer, int mode)
{
	memset(mixer, 0, sizeof(audio_trackmixer_t));
	mixer->sc = sc;

#if defined(AUDIO_SOFTINTR)
	mixer->softintr = softint_establish(SOFTINT_SERIAL, audio_pmixer_softintr, mixer);
#endif

	mixer->blktime_d = 1000;
	mixer->blktime_n = AUDIO_BLK_MS;
	mixer->hwblks = 16;

#if defined(_KERNEL)
	// XXX とりあえず
	if (mode == AUMODE_PLAY)
		mixer->hwbuf.fmt = sc->sc_phwfmt;
	else
		mixer->hwbuf.fmt = sc->sc_rhwfmt;
#else
	mixer->hwbuf.fmt = audio_softc_get_hw_format(mixer->sc, mode);
#endif

	mixer->frames_per_block = frame_per_block_roundup(mixer, &mixer->hwbuf.fmt)
		* audio_framealign(mixer->hwbuf.fmt.stride);
	int blksize = frametobyte(&mixer->hwbuf.fmt, mixer->frames_per_block);
	if (sc->hw_if->round_blocksize) {
		int rounded;
		audio_params_t p = format2_to_params(&mixer->hwbuf.fmt);
		mutex_enter(sc->sc_lock);
		rounded = sc->hw_if->round_blocksize(sc->hw_hdl, blksize, mode, &p);
		mutex_exit(sc->sc_lock);
		// 違っていても困る?
		if (rounded != blksize) {
			if ((rounded * 8) % (mixer->hwbuf.fmt.stride * mixer->hwbuf.fmt.channels) != 0) {
				aprint_error_dev(sc->dev, "blksize not configured"
					" %d -> %d\n", blksize, rounded);
				return ENXIO;
			}
			// 再計算
			mixer->frames_per_block = rounded * 8 / (mixer->hwbuf.fmt.stride * mixer->hwbuf.fmt.channels);
		}
	}
	mixer->blktime_n = mixer->frames_per_block;
	mixer->blktime_d = mixer->hwbuf.fmt.sample_rate;

	int capacity = mixer->frames_per_block * mixer->hwblks;
	size_t bufsize = frametobyte(&mixer->hwbuf.fmt, capacity);
	if (sc->hw_if->round_buffersize) {
		size_t rounded;
		mutex_enter(sc->sc_lock);
		rounded = sc->hw_if->round_buffersize(sc->hw_hdl, mode, bufsize);
		mutex_exit(sc->sc_lock);
		// 縮められても困る?
		if (rounded != bufsize) {
			aprint_error_dev(sc->dev, "buffer size not configured"
			    " %zu -> %zu\n", bufsize, rounded);
			return ENXIO;
		}
	}
	mixer->hwbuf.capacity = capacity;

	if (sc->hw_if->allocm) {
		mixer->hwbuf.sample = sc->hw_if->allocm(sc->hw_hdl, mode,
		    bufsize);
	} else {
		mixer->hwbuf.sample = kern_malloc(bufsize, M_NOWAIT);
	}

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

	// XXX どうするか
	audio_filter_reg_t *reg;
	if (mode == AUMODE_PLAY) {
		reg = &sc->sc_xxx_pfilreg;
	} else {
		reg = &sc->sc_xxx_rfilreg;
	}
	mixer->codec = reg->codec;
	mixer->codecarg.context = reg->context;
	mixer->codecarg.srcfmt = &mixer->track_fmt;
	mixer->codecarg.dstfmt = &mixer->hwbuf.fmt;
	mixer->codecbuf.fmt = mixer->track_fmt;
	mixer->codecbuf.capacity = mixer->frames_per_block;
	mixer->codecbuf.sample = audio_realloc(mixer->codecbuf.sample, RING_BYTELEN(&mixer->codecbuf));

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

#if defined(AUDIO_SOFTINTR)
	softint_disestablish(mixer->softintr);
#endif

	// intrcv を cv_destroy() してはいけないっぽい。KASSERT で死ぬ。
}

// 再生ミキサを起動します。起動できれば true を返します。
// すでに起動されていれば何もせず true を返します。
// 割り込みコンテキストから呼び出してはいけません。
bool
audio_pmixer_start(audio_trackmixer_t *mixer, bool force)
{
	struct audio_softc *sc;

	sc = mixer->sc;
	KASSERT(mutex_owned(sc->sc_lock));
	KASSERT(!mutex_owned(sc->sc_intr_lock));

	// すでに再生ミキサが起動していたら、true を返す
	if (sc->sc_pbusy)
		return true;

	TRACE0("begin mixseq=%d hwseq=%d hwbuf=%d/%d/%d",
		(int)mixer->mixseq, (int)mixer->hwseq,
		mixer->hwbuf.top, mixer->hwbuf.count, mixer->hwbuf.capacity);

	// バッファを埋める
	if (mixer->hwbuf.count < mixer->frames_per_block) {
		audio_pmixer_process(mixer);

		// トラックミキサ出力開始
		mutex_enter(sc->sc_intr_lock);
		audio_pmixer_output(mixer);
		mutex_exit(sc->sc_intr_lock);
	}

	TRACE0("end   mixseq=%d hwseq=%d hwbuf=%d/%d/%d",
		(int)mixer->mixseq, (int)mixer->hwseq,
		mixer->hwbuf.top, mixer->hwbuf.count, mixer->hwbuf.capacity);

	return sc->sc_pbusy;
}

// 全トラックを req フレーム分合成します。
// 合成されたトラック数を返します。
// mixer->softintrlock を取得して呼び出してください。
static int
audio_pmixer_mixall(audio_trackmixer_t *mixer, int req, bool isintr)
{
	struct audio_softc *sc;
	audio_file_t *f;
	int mixed = 0;

	sc = mixer->sc;
#if defined(AUDIO_SOFTINTR)
	KASSERT(mutex_owned(&mixer->softintrlock));
#endif

	SLIST_FOREACH(f, &sc->sc_files, entry) {
		audio_track_t *track = f->ptrack;

		KASSERT(track);

#if !defined(AUDIO_SOFTINTR)
		if (isintr) {
			// 協調的ロックされているトラックは、今回ミキシングしない。
			if (track->track_cl) continue;
		}
#endif

		// 合成
		if (track->outputbuf.count > 0) {
			mixed = audio_pmixer_mix_track(mixer, track, req, mixed);
		}
	}
	return mixed;
}

/*
* トラックバッファから取り出し、ミキシングします。
* mixed には呼び出し時点までの合成済みトラック数を渡します。
* 戻り値はこの関数終了時での合成済みトラック数)です。
* つまりこのトラックを合成すれば mixed + 1 を返します。
*/
int
audio_pmixer_mix_track(audio_trackmixer_t *mixer, audio_track_t *track, int req, int mixed)
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

// 全トラックを倍精度ミキシングバッファで合成し、
// 倍精度ミキシングバッファから hwbuf への変換を行います。
// (hwbuf からハードウェアへの転送はここでは行いません)
// 呼び出し時の sc_intr_lock の状態はどちらでもよく、hwbuf へのアクセスを
// sc_intr_lock でこの関数が保護します。
void
audio_pmixer_process(audio_trackmixer_t *mixer /*, bool force */)
{
	struct audio_softc *sc;
	int mixed;
	internal2_t *mptr;

	sc = mixer->sc;

	// 今回取り出すフレーム数を決定
	// 実際には hwbuf はブロック単位で変動するはずなので
	// count は1ブロック分になるはず
	int hw_free_count = audio_ring_unround_free_count(&mixer->hwbuf);
	int frame_count = min(hw_free_count, mixer->frames_per_block);
	if (frame_count <= 0) {
		TRACE0("count too short: hw_free=%d frames_per_block=%d",
		    hw_free_count, mixer->frames_per_block);
		return;
	}
	int sample_count = frame_count * mixer->mixfmt.channels;

	mixer->mixseq++;

	// 全トラックを合成
	mixed = audio_pmixer_mixall(mixer, mixer->frames_per_block, true);
	if (mixed == 0) {
		// 無音
		memset(mixer->mixsample, 0,
		    frametobyte(&mixer->mixfmt, frame_count));
	} else {
		// オーバーフロー検出
		internal2_t ovf_plus = AUDIO_INTERNAL_T_MAX;
		internal2_t ovf_minus = AUDIO_INTERNAL_T_MIN;

		mptr = mixer->mixsample;

		for (int i = 0; i < sample_count; i++) {
			if (*mptr > ovf_plus) ovf_plus = *mptr;
			if (*mptr < ovf_minus) ovf_minus = *mptr;

			mptr++;
		}

		// マスタボリュームの自動制御
		int vol = mixer->volume;
		if (ovf_plus > (internal2_t)AUDIO_INTERNAL_T_MAX
		 || ovf_minus < (internal2_t)AUDIO_INTERNAL_T_MIN) {
			// TODO: AUDIO_INTERNAL2_T_MIN チェック?
			internal2_t ovf = ovf_plus;
			if (ovf < -ovf_minus) ovf = -ovf_minus;

			// オーバーフローしてたら少なくとも今回はボリュームを
			// 下げる
			int vol2 = (int)((internal2_t)AUDIO_INTERNAL_T_MAX * 256 / ovf);
			if (vol2 < vol) vol = vol2;

			if (vol < mixer->volume) {
				// 128 までは自動でマスタボリュームを下げる
				// 今の値の 95% ずつに下げていってみる
				if (mixer->volume > 128) {
					mixer->volume = mixer->volume * 95 / 100;
					aprint_normal_dev(sc->dev,
					    "auto volume adjust: volume %d\n",
					    mixer->volume);
				}
			}
		}

		// マスタボリューム適用
		if (vol != 256) {
			mptr = mixer->mixsample;
			for (int i = 0; i < sample_count; i++) {
				*mptr = *mptr * vol / 256;
				mptr++;
			}
		}
	}

	// ここから ハードウェアチャンネル

	// ハードウェアバッファへ転送
	int need_exit = mutex_tryenter(sc->sc_intr_lock);

	mptr = mixer->mixsample;
	internal_t *hptr;
	// MD 側フィルタがあれば internal2_t -> internal_t を codecbuf へ
	if (mixer->codec) {
		hptr = RING_BOT(internal_t, &mixer->codecbuf);
	} else {
		hptr = RING_BOT(internal_t, &mixer->hwbuf);
	}

	for (int i = 0; i < sample_count; i++) {
		*hptr++ = *mptr++;
	}

	// MD 側フィルタ
	if (mixer->codec) {
		audio_ring_appended(&mixer->codecbuf, frame_count);
		mixer->codecarg.src = RING_TOP_UINT8(&mixer->codecbuf);
		mixer->codecarg.dst = RING_BOT_UINT8(&mixer->hwbuf);
		mixer->codecarg.count = frame_count;
		mixer->codec(&mixer->codecarg);
		audio_ring_tookfromtop(&mixer->codecbuf, mixer->codecarg.count);
	}

	audio_ring_appended(&mixer->hwbuf, frame_count);

	TRACE0("done mixseq=%d hwbuf=%d/%d/%d%s",
	    (int)mixer->mixseq,
	    mixer->hwbuf.top, mixer->hwbuf.count, mixer->hwbuf.capacity,
	    (mixed == 0) ? " silent" : "");

	if (need_exit) {
		mutex_exit(sc->sc_intr_lock);
	}
}

// ハードウェアバッファから 1 ブロック出力
// sc_intr_lock で呼び出してください。
void
audio_pmixer_output(audio_trackmixer_t *mixer)
{
	struct audio_softc *sc;

	sc = mixer->sc;
	KASSERT(mutex_owned(sc->sc_intr_lock));

	TRACE0("pbusy=%d hwbuf=%d/%d/%d",
	    sc->sc_pbusy,
	    mixer->hwbuf.top, mixer->hwbuf.count, mixer->hwbuf.capacity);
	KASSERT(mixer->hwbuf.count >= mixer->frames_per_block);

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

// 割り込みハンドラ
// sc_intr_lock で呼び出されます。
void
audio_pmixer_intr(audio_trackmixer_t *mixer)
{
	struct audio_softc *sc __diagused;

	sc = mixer->sc;
	KASSERT(mutex_owned(sc->sc_intr_lock));

	mixer->hw_complete_counter += mixer->frames_per_block;
	mixer->hwseq++;

	audio_ring_tookfromtop(&mixer->hwbuf, mixer->frames_per_block);

	TRACE0("HW_INT ++hwseq=%d cmplcnt=%d hwbuf=%d/%d/%d",
		(int)mixer->hwseq,
		(int)mixer->hw_complete_counter,
		mixer->hwbuf.top, mixer->hwbuf.count, mixer->hwbuf.capacity);

	// まず出力待ちのシーケンスを出力
	if (mixer->hwbuf.count >= mixer->frames_per_block) {
		audio_pmixer_output(mixer);
	}

#if defined(AUDIO_SOFTINTR)
	// ハードウェア割り込みでは待機関数が使えないため、
	// softintr へ転送。
	softint_schedule(mixer->softintr);

#else
	bool later = false;

	if (mixer->hwbuf.count < mixer->frames_per_block) {
		later = true;
	}

	// 次のバッファを用意する
	audio_pmixer_process(mixer);

	if (later) {
		audio_pmixer_output(mixer);
	}

	// drain 待ちしている人のために通知
	cv_broadcast(&mixer->intrcv);
#endif
}

#if defined(AUDIO_SOFTINTR)
static void
audio_pmixer_softintr(void *arg)
{
	audio_trackmixer_t *mixer = arg;
	struct audio_softc *sc = mixer->sc;

	KASSERT(!mutex_owned(sc->sc_intr_lock));

	mutex_enter(&mixer->softintrlock);

	bool later = false;

	if (mixer->hwbuf.count < mixer->frames_per_block) {
		later = true;
	}

	// 次のバッファを用意する
	audio_pmixer_process(mixer);

	if (later) {
		mutex_enter(sc->sc_intr_lock);
		audio_pmixer_output(mixer);
		mutex_exit(sc->sc_intr_lock);
	}
	// finally
	mutex_exit(&mixer->softintrlock);

	// drain 待ちしている人のために通知
	cv_broadcast(&mixer->intrcv);
	TRACE0("SW_INT ++hwseq=%d cmplcnt=%d hwbuf=%d/%d/%d",
		(int)mixer->hwseq,
		(int)mixer->hw_complete_counter,
		mixer->hwbuf.top, mixer->hwbuf.count, mixer->hwbuf.capacity);
}
#endif

static void
audio2_pintr(void *arg)
{
	struct audio_softc *sc;
	audio_trackmixer_t *mixer;

	sc = arg;
	KASSERT(mutex_owned(sc->sc_intr_lock));

	mixer = sc->sc_pmixer;

	audio_pmixer_intr(mixer);
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

int
audio2_halt_output(struct audio_softc *sc)
{
	int error;

	TRACE0("");
	KASSERT(mutex_owned(sc->sc_lock));
	KASSERT(mutex_owned(sc->sc_intr_lock));

	error = sc->hw_if->halt_output(sc->hw_hdl);
	// エラーが起きても停止は停止する
	sc->sc_pbusy = false;
	sc->sc_pmixer->hwbuf.top = 0;
	sc->sc_pmixer->hwbuf.count = 0;
	sc->sc_pmixer->mixseq = 0;
	sc->sc_pmixer->hwseq = 0;

	return error;
}

// errno を返します。
int
audio_track_drain(audio_track_t *track, bool wait)
{
	audio_trackmixer_t *mixer;
	struct audio_softc *sc;
	int error;

	KASSERT(track);
	TRACE(track, "start");
	mixer = track->mixer;
	sc = mixer->sc;
	KASSERT(mutex_owned(sc->sc_lock));
	KASSERT(!mutex_owned(sc->sc_intr_lock));

	track->is_draining = true;

	// 必要があれば無音挿入させる

	audio_track_enter_colock(sc, track);
	audio_track_play(track, true);
	audio_track_leave_colock(sc, track);

	if (wait) {
		while (track->seq > mixer->hwseq) {
			TRACE(track, "trkseq=%d hwseq=%d",
			    (int)track->seq, (int)mixer->hwseq);
			error = cv_wait_sig(&mixer->intrcv, sc->sc_lock);
			if (error) {
				printf("cv_wait_sig failed %d\n", error);
				if (error < 0)
					error = EINTR;
				return error;
			}
		}

		track->is_draining = false;
		TRACE(track, "done trk_inp=%d trk_out=%d",
			(int)track->inputcounter, (int)track->outputcounter);
	}
	return 0;
}

// track の usrbuf に bottom から len バイトを uiomove します。
// リングバッファの折り返しはしません。
static inline int
audio_write_uiomove(audio_track_t *track, int bottom, int len, struct uio *uio)
{
	audio_ring_t *usrbuf;
	int error;

	usrbuf = &track->usrbuf;
	error = uiomove((uint8_t *)usrbuf->sample + bottom, len, uio);
	if (error) {
		TRACE(track, "uiomove(len=%d) failed: %d",
		    len, error);
		return error;
	}
	audio_ring_appended(usrbuf, len);
	track->inputcounter += len;
	TRACE(track, "uiomove(len=%d) usrbuf=%d/%d/%d",
	    len,
	    usrbuf->top, usrbuf->count, usrbuf->capacity);
	return 0;
}

/* write の MI 側 */
int
audio_write(struct audio_softc *sc, struct uio *uio, int ioflag, audio_file_t *file)
{
	int error;
	audio_track_t *track = file->ptrack;
	KASSERT(track);
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
	if (!sc->sc_full_duplex && file->rtrack) {
		uio->uio_offset += uio->uio_resid;
		uio->uio_resid = 0;
		DPRINTF(1, "audio_write: half-dpx read busy\n");
		return 0;
	}

	// XXX playdrop と PLAY_ALL はちょっと後回し
#endif // _KERNEL

	error = 0;

	// inp_thres は usrbuf に書き込む際の閾値。
	// usrbuf.count が inp_thres より小さければ uiomove する。
	// o PLAY なら常にコピーなので capacity を設定
	// o PLAY_ALL なら1ブロックあれば十分なので block size を設定
	//
	// out_thres は usrbuf から読み出す際の閾値。
	// trkbuf.count が out_thres より大きければ変換処理を行う。
	// o PLAY なら常に変換処理をしたいので 0 に設定
	// o PLAY_ALL なら1ブロック溜まってから処理なので block size を設定
	audio_ring_t *usrbuf = &track->usrbuf;
	int inp_thres;
	int out_thres;
	if ((track->mode & AUMODE_PLAY_ALL) != 0) {
		/* PLAY_ALL */
		int usrbuf_blksize = track->inputfmt.sample_rate
		    * track->inputfmt.channels
		    * track->inputfmt.stride / NBBY
		    * AUDIO_BLK_MS / 1000;
		inp_thres = usrbuf_blksize;
		out_thres = usrbuf_blksize;
	} else {
		/* PLAY */
		inp_thres = usrbuf->capacity;
		out_thres = 0;
	}
	TRACE(track, "resid=%zd inp_thres=%d out_thres=%d",
	    uio->uio_resid, inp_thres, out_thres);

	error = 0;
	while (uio->uio_resid > 0 && error == 0) {
		int bytes;

		TRACE(track, "while resid=%zd usrbuf=%d/%d/%d",
		    uio->uio_resid,
		    usrbuf->top, usrbuf->count, usrbuf->capacity);

		// usrbuf 閾値に満たなければ、可能な限りを一度にコピー
		if (usrbuf->count < inp_thres) {
			bytes = min(usrbuf->capacity - usrbuf->count,
			    uio->uio_resid);
			int bottom = audio_ring_bottom(usrbuf);
			if (bottom  + bytes < usrbuf->capacity) {
				error = audio_write_uiomove(track, bottom,
				    bytes, uio);
				if (error)
					break;
			} else {
				int bytes1;
				int bytes2;

				bytes1 = usrbuf->capacity - bottom;
				error = audio_write_uiomove(track, bottom,
				    bytes1, uio);
				if (error)
					break;

				bytes2 = bytes - bytes1;
				error = audio_write_uiomove(track, 0,
				    bytes2, uio);
				if (error)
					break;
			}
		}

		audio_track_enter_colock(sc, track);

		while (track->usrbuf.count >= out_thres && error == 0) {
			if (track->outputbuf.count == track->outputbuf.capacity) {
				// trkbuf が一杯ならここで待機
				audio_track_leave_colock(sc, track);
				error = audio_waitio(sc, track);
				audio_track_enter_colock(sc, track);
				if (error != 0) {
					if (error < 0) {
						error = EINTR;
					}
					return error;
				}
				continue;
			}

			audio_track_play(track, false);
		}

		audio_track_leave_colock(sc, track);
#if !defined(_KERNEL)
		emu_intr_check();
#endif
	}

	return error;
}

static int
audio_waitio(struct audio_softc *sc, audio_track_t *track)
{
	// XXX 自分がいなくなることを想定する必要があるのかどうか
	int error;

	KASSERT(track);
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
		audio_track_init(sc, &file->ptrack, AUMODE_PLAY);
	} else {
		audio_track_init(sc, &file->rtrack, AUMODE_RECORD);
	}

	SLIST_INSERT_HEAD(&sc->sc_files, file, entry);

	return file;
}

// ioctl(AUDIO_DRAIN) 相当
int
sys_ioctl_drain(audio_track_t *track, bool wait)
{
	// 割り込みエミュレートしているときはメインループに制御を戻さないといけない
	audio_trackmixer_t *mixer = track->mixer;
	struct audio_softc *sc = mixer->sc;

	mutex_enter(sc->sc_lock);
	audio_track_drain(track, wait);
	mutex_exit(sc->sc_lock);

	return 0;
}
#endif // !_KERNEL
