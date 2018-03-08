#if defined(_KERNEL)
#include <dev/audio/auring.h>
#include <dev/audio/aucodec.h>
#include <sys/intr.h>
#else
#include <errno.h>
#include <stdlib.h>
#include <memory.h>
#include <stdbool.h>
#include "audiovar.h"
#include "aucodec.h"
#include "auformat.h"
#endif

void *audio_realloc(void *memblock, size_t bytes);
static int audio_realloc_usrbuf(audio_track_t *, int);
static void audio_free_usrbuf(audio_track_t *);
static audio_filter_t audio_track_get_codec(const audio_format2_t *,
	const audio_format2_t *);
static int audio_pmixer_mixall(struct audio_softc *sc, bool isintr);
void audio_pmixer_output(struct audio_softc *sc);
static void audio_rmixer_input(struct audio_softc *sc);
static int audio_waitio(struct audio_softc *sc, audio_track_t *track);

void
audio_vtrace(const char *funcname, const char *header, const char *fmt,
	va_list ap)
{
	char buf[256];
	struct timeval tv;
	int n;

	// デバッグレベル4だけ割り込み中のトレースも出力する
#if AUDIO_DEBUG < 4
	if (cpu_intr_p())
		return;
#endif

	n = 0;
	buf[0] = '\0';
	getmicrotime(&tv);
	n += snprintf(buf + n, sizeof(buf) - n, "%02d.%06d %s %s",
	    (int)tv.tv_sec % 60, (int)tv.tv_usec, funcname, header);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	printf("%s\n", buf);
}

void
audio_trace(const char *funcname, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	audio_vtrace(funcname, "", fmt, ap);
	va_end(ap);
}

void
audio_tracet(const char *funcname, audio_track_t *track, const char *fmt, ...)
{
	char hdr[16];
	va_list ap;

	snprintf(hdr, sizeof(hdr), "#%d ", track->id);
	va_start(ap, fmt);
	audio_vtrace(funcname, hdr, fmt, ap);
	va_end(ap);
}

void
audio_tracef(const char *funcname, audio_file_t *file, const char *fmt, ...)
{
	char hdr[32];
	char phdr[16], rhdr[16];
	va_list ap;

	phdr[0] = '\0';
	rhdr[0] = '\0';
	if (file->ptrack)
		snprintf(phdr, sizeof(phdr), "#%d", file->ptrack->id);
	if (file->rtrack)
		snprintf(rhdr, sizeof(rhdr), "#%d", file->rtrack->id);
	snprintf(hdr, sizeof(hdr), "{%s,%s} ", phdr, rhdr);

	va_start(ap, fmt);
	audio_vtrace(funcname, hdr, fmt, ap);
	va_end(ap);
}

#if AUDIO_DEBUG > 2
struct audio_track_debugbuf {
	char outbuf[32];
	char freq[32];
	char chvol[32];
	char chmix[32];
	char codec[32];
	char usrbuf[32];
};

// track の各バッファの状態を buf に出力します。
static void
audio_track_bufstat(audio_track_t *track, struct audio_track_debugbuf *buf)
{
	memset(buf, 0, sizeof(*buf));

	snprintf(buf->outbuf, sizeof(buf->outbuf), " out=%d/%d/%d",
	    track->outputbuf.head, track->outputbuf.used,
	    track->outputbuf.capacity);
	if (track->freq.filter)
		snprintf(buf->freq, sizeof(buf->freq), " f=%d/%d/%d",
		    track->freq.srcbuf.head,
		    track->freq.srcbuf.used,
		    track->freq.srcbuf.capacity);
	if (track->chmix.filter)
		snprintf(buf->chmix, sizeof(buf->chmix), " m=%d",
		    track->chmix.srcbuf.used);
	if (track->chvol.filter)
		snprintf(buf->chvol, sizeof(buf->chvol), " v=%d",
		    track->chvol.srcbuf.used);
	if (track->codec.filter)
		snprintf(buf->codec, sizeof(buf->codec), " e=%d",
		    track->codec.srcbuf.used);
	snprintf(buf->usrbuf, sizeof(buf->usrbuf), " usr=%d/%d/H%d",
	    track->usrbuf.head, track->usrbuf.used, track->usrbuf_usedhigh);
}
#endif

/*
 * audio_realloc:
 *	Reallocate 'memblock' with specified 'bytes' if 'bytes' > 0.
 *	Or free 'memblock' and return NULL if 'byte' is zero.
 */
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
 * audio_free:
 *	Free 'mem' if available.
 *	And initialize the pointer.  For this reason, this is implemented
 *	as macro.
 */
#define audio_free(mem)	do {	\
	if (mem != NULL) {	\
		kern_free(mem);	\
		mem = NULL;	\
	}	\
} while (0)

// usrbuf を newbufsize で確保し直します。
// usrbuf は mmap されるためこちらを使用してください。
// usrbuf.capacity を更新する前に呼び出してください。
// メモリが確保できれば track->mem、track->capacity をセットし 0 を返します。
// 確保できなければ track->mem、track->capacity をクリアし errno を返します。
/*
 * audio_realloc_usrbuf:
 *	(Re)allocate usrbuf with 'newbufsize' bytes.  Use this function for
 *	usrbuf because only usrbuf may be mmapped.
 *	If successful, it updates track->usrbuf.mem, track->usrbuf.capacity
 *	and returns 0.  Otherwise, it clears track->usrbuf.mem,
 *	track->usrbuf.capacity and returns errno.
 *	It must be called before updating usrbuf.capacity.
 */
static int
audio_realloc_usrbuf(audio_track_t *track, int newbufsize)
{
	vaddr_t vstart;
	vsize_t oldvsize;
	vsize_t newvsize;
	int error;

	KASSERT(newbufsize > 0);

	/* Get a nonzero multiple of PAGE_SIZE */
	newvsize = roundup2(MAX(newbufsize, PAGE_SIZE), PAGE_SIZE);

	if (track->usrbuf.mem != NULL) {
		oldvsize = roundup2(MAX(track->usrbuf.capacity, PAGE_SIZE),
		    PAGE_SIZE);
		if (oldvsize == newvsize) {
			track->usrbuf.capacity = newbufsize;
			return 0;
		}
		vstart = (vaddr_t)track->usrbuf.mem;
		uvm_unmap(kernel_map, vstart, vstart + oldvsize);
		/* uvm_unmap also detach uobj */
		track->uobj = NULL;		/* paranoia */
		track->usrbuf.mem = NULL;
	}

	/* Create a uvm anonymous object */
	track->uobj = uao_create(newvsize, 0);

	/* Map it into the kernel virtual address space */
	vstart = 0;
	error = uvm_map(kernel_map, &vstart, newvsize, track->uobj, 0, 0,
	    UVM_MAPFLAG(UVM_PROT_RW, UVM_PROT_RW, UVM_INH_NONE,
	    UVM_ADV_RANDOM, 0));
	if (error) {
		DPRINTF(1, "%s: uvm_map failed\n", __func__);
		uao_detach(track->uobj);	/* release reference */
		goto abort;
	}

	error = uvm_map_pageable(kernel_map, vstart, vstart + newvsize,
	    false, 0);
	if (error) {
		DPRINTF(1, "%s: uvm_map_pageable failed\n", __func__);
		uvm_unmap(kernel_map, vstart, vstart + newvsize);
		/* uvm_unmap also detach uobj */
		goto abort;
	}

	track->usrbuf.mem = (void *)vstart;
	track->usrbuf.capacity = newbufsize;
	memset(track->usrbuf.mem, 0, newvsize);
	return 0;

	/* failure */
abort:
	track->uobj = NULL;		/* paranoia */
	track->usrbuf.mem = NULL;
	track->usrbuf.capacity = 0;
	return error;
}

/*
 * audio_free_usrbuf:
 *	Free usrbuf (if available).
 */
static void
audio_free_usrbuf(audio_track_t *track)
{
	vaddr_t vstart;
	vsize_t vsize;

	vstart = (vaddr_t)track->usrbuf.mem;
	vsize = roundup2(MAX(track->usrbuf.capacity, PAGE_SIZE), PAGE_SIZE);
	if (track->usrbuf.mem != NULL) {
		/*
		 * Unmap the kernel mapping.  uvm_unmap releases the
		 * reference to the uvm object, and this should be the
		 * last virtual mapping of the uvm object, so no need
		 * to explicitly release (`detach') the object.
		 */
		uvm_unmap(kernel_map, vstart, vstart + vsize);

		track->uobj = NULL;
		track->usrbuf.mem = NULL;
		track->usrbuf.capacity = 0;
	}
}

/*
 * audio_track_chvol:
 *	This filter changes the volume for each channel.
 *	arg->context points track->ch_volume[].
 */
static void
audio_track_chvol(audio_filter_arg_t *arg)
{
	int16_t *ch_volume;
	const aint_t *s;
	aint_t *d;
	int channels;
	int i;
	int ch;

	KASSERT(is_valid_filter_arg(arg));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);
	KASSERT(arg->context != NULL);
	KASSERT(arg->srcfmt->channels <= AUDIO_MAX_CHANNELS);

	s = arg->src;
	d = arg->dst;
	ch_volume = arg->context;

	channels = arg->srcfmt->channels;
	for (i = 0; i < arg->count; i++) {
		for (ch = 0; ch < channels; ch++) {
			aint2_t val;
			val = *s++;
#if defined(AUDIO_USE_C_IMPLEMENTATION_DEFINED_BEHAVIOR) && defined(__GNUC__)
			val = val * ch_volume[ch] >> 8;
#else
			val = val * ch_volume[ch] / 256;
#endif
			*d++ = (aint_t)val;
		}
	}
}

/*
 * audio_track_chmix_mixLR:
 *	This filter performs conversion from stereo (or more channels) to mono.
 */
static void
audio_track_chmix_mixLR(audio_filter_arg_t *arg)
{
	// L と R、2つのサンプルの算術平均を取る。
	//  mixed = (L + R) / 2;
	// のように先に加算をしてしまうと整数オーバーフローが起きうる。
	// aint2_t にキャストしてから演算すれば問題ないが、このために
	// そこまでするかという気はする。
	//
	// そこで L と R を先に割っておいてから足す。
	//  mixed = (L / 2) + (R / 2);  // (1)
	// この式で例えば L = 1, R = 1 の場合数学的には答えは1 になってほしいが
	// 先に切り捨てが起きるため答えは 0 となり、誤差は全域で最大
	// (aint_t が 16bit の場合) 1/65536 (かな?)。
	//
	// ところで C で負数の除算は 0 方向への丸めと定義されているため、
	// アセンブラの算術右シフトだけでは実現できず (算術右シフトはマイナス
	// 無限大方向への丸めになる)、実行時に値が負数だったら 1補正する
	// みたいな処理が(たぶん大抵)書いてある。少なくとも gcc 4.8 とかには。
	// ただしここの目的は (たぶんこの後スピーカーから出て人間の耳に届く)
	// 音声波形をなるべく高速に処理したいところなので、その誤差 1/65536
	// を許容して高速化したい。
	// ところが C で負数の右シフトは処理系定義となっている (不定動作では
	// ない)。GCC では負数の右シフトは算術右シフトと定義してあるようなので
	// https://gcc.gnu.org/onlinedocs/gcc-5.5.0/gcc/ (section 4.5)
	// もし許されるなら使いたいところ。
	//  mixed = (L >> 1) + (R >> 1); // (2)
	// この場合の誤差は負領域のみで最大 (aint_t が 16bit の場合)
	// 1/65536 (かな?)。
	//
	//	amd64 [times/msec]	x68k [times/sec]
	// (1)	 78.8			176.4
	// (2)	150.1			245.8

	const aint_t *s;
	aint_t *d;
	int i;

	KASSERT(is_valid_filter_arg(arg));

	s = arg->src;
	d = arg->dst;

	for (i = 0; i < arg->count; i++) {
#if defined(AUDIO_USE_C_IMPLEMENTATION_DEFINED_BEHAVIOR) && defined(__GNUC__)
		*d++ = (s[0] >> 1) + (s[1] >> 1);
#else
		*d++ = (s[0] / 2) + (s[1] / 2);
#endif
		s += arg->srcfmt->channels;
	}
}

/*
 * audio_track_chmix_dupLR:
 *	This filter performs conversion from mono to stereo (or more channels).
 */
static void
audio_track_chmix_dupLR(audio_filter_arg_t *arg)
{
	const aint_t *s;
	aint_t *d;
	int i;
	int ch;
	int dstchannels;

	KASSERT(is_valid_filter_arg(arg));

	s = arg->src;
	d = arg->dst;
	dstchannels = arg->dstfmt->channels;

	for (i = 0; i < arg->count; i++) {
		d[0] = s[0];
		d[1] = s[0];
		s++;
		d += dstchannels;
	}
	if (dstchannels > 2) {
		d = arg->dst;
		for (i = 0; i < arg->count; i++) {
			for (ch = 2; ch < dstchannels; ch++) {
				d[ch] = 0;
			}
			d += dstchannels;
		}
	}
}

/*
 * audio_track_chmix_shrink:
 *	This filter shrinks M channels into N channels.
 *	Extra channels are discarded.
 */
static void
audio_track_chmix_shrink(audio_filter_arg_t *arg)
{
	const aint_t *s;
	aint_t *d;
	int i;
	int ch;

	KASSERT(is_valid_filter_arg(arg));

	s = arg->src;
	d = arg->dst;

	for (i = 0; i < arg->count; i++) {
		for (ch = 0; ch < arg->dstfmt->channels; ch++) {
			*d++ = s[ch];
		}
		s += arg->srcfmt->channels;
	}
}

/*
 * audio_track_chmix_expand:
 *	This filter expands M channels into N channels.
 *	Silence is inserted for missing channels.
 */
static void
audio_track_chmix_expand(audio_filter_arg_t *arg)
{
	const aint_t *s;
	aint_t *d;
	int i;
	int ch;
	int srcchannels;
	int dstchannels;

	KASSERT(is_valid_filter_arg(arg));

	s = arg->src;
	d = arg->dst;

	srcchannels = arg->srcfmt->channels;
	dstchannels = arg->dstfmt->channels;
	for (i = 0; i < arg->count; i++) {
		for (ch = 0; ch < srcchannels; ch++) {
			*d++ = *s++;
		}
		for (; ch < dstchannels; ch++) {
			*d++ = 0;
		}
	}
}

// 周波数変換は入出力周波数の比 (srcfreq / dstfreq) で計算を行う。
// そのまま分数で計算するのがシンプルだが、ここでは除算回数を減らす
// ため dstfreq を 65536 とした時の src/dst 比を用いる。
// なおこのアイデアは S44PLAY.X から拝借したもの。
//  http://stdkmd.com/kohx3/
//
// 例えば入力 24kHz を 48kHz に変換する場合は src/dst = 32768/65536 と
// なり、この分子 32768 が track->freq_step である。
// 原理としては出力1サンプルごとに変数(ここでは t)に freq_step を
// 加算していき、これが 65536 以上になるごとに入力を行って、その間を
// 補間する。
//
// 入出力周波数の組み合わせによっては freq_step が整数にならない場合も
// 当然ある。例えば入力 8kHz を 48kHz に変換する場合
//  freq_step = 8000 / 48000 * 65536 = 10922.6666…
// となる。
// この場合出力1サンプルあたり理論値よりも 0.6666 ずつカウントが少なく
// なるわけなので、これをブロックごとに補正する。
// 1ブロックの時間 AUDIO_BLK_MS が標準の 40msec であれば、出力周波数
// 48kHz に対する1ブロックの出力サンプル数は
//  dstcount = 48000[Hz] * 0.04[sec] = 1920
// より 1920個なので、補正値は
//  freq_leap = 0.6666… * 1920 = 1280
// となる。つまり 8kHz を 48kHz に変換する場合、1920 出力サンプルごとに
// t にこの 1280 を足すことで周波数変換誤差は出なくなる。
//
// さらに freq_leap が整数にならないような入出力周波数の組み合わせも
// もちろんありうるが、日常使う程度の組み合わせではほぼ発生しないと
// 思うし、また発生したとしてもその誤差は 10^-6 以下でありこれは水晶
// 振動子の誤差程度かそれ以下であるので、用途に対しては十分許容できる
// と思う。

// AUDIO_DEBUG=1、AUDIO_ASSERT なしで main.c による計測。
//
//        44->48    8->48   48->44    48->8	[times/msec]
// amd64    70.9    113.4    177.8    984.8	Pentium DC E5400/2.7GHz
// x68k    0.048    0.065    0.129    0.688	68030/30MHz

// 周波数変換(アップサンプリング)をします。線形補間です。
/*
 * audio_track_freq_up:
 *	This filter performs frequency conversion (up sampling).
 *	It uses linear interpolation.
 */
static void
audio_track_freq_up(audio_filter_arg_t *arg)
{
	audio_track_t *track = arg->context;
	audio_ring_t *src = &track->freq.srcbuf;
	audio_ring_t *dst = track->freq.dst;

	KASSERT(track);
	KASSERT(audio_ring_is_valid(dst));
	KASSERT(audio_ring_is_valid(src));
	KASSERT(src->used > 0);
	KASSERT(src->fmt.channels == dst->fmt.channels);
	KASSERT(src->head % track->mixer->frames_per_block == 0);

	const aint_t *s = arg->src;
	aint_t *d = arg->dst;

	// 補間はブロック単位での処理がしやすいように入力を1サンプルずらして(?)
	// 補間を行なっている。このため厳密には位相が 1/dstfreq 分だけ遅れる
	// ことになるが、これによる観測可能な影響があるとは思えない。
	/*
	 * In order to faciliate interpolation for each block, slide (delay)
	 * input by one sample.  As a result, strictly speaking, the output
	 * phase is delayed by 1/dstfreq.  However, I believe there is no
	 * observable impact.
	 *
	 * Example)
	 * srcfreq:dstfreq = 1:3
	 *
	 *  A - -
	 *  |
	 *  |
	 *  |     B - -
	 *  +-----+-----> input timeframe
	 *  0     1
	 *
	 *  0     1
	 *  +-----+-----> input timeframe
	 *  |     A
	 *  |   x   x
	 *  | x       x
	 *  x          (B)
	 *  +-+-+-+-+-+-> output timeframe
	 *  0 1 2 3 4 5
	 */

	aint_t prev[AUDIO_MAX_CHANNELS];
	aint_t curr[AUDIO_MAX_CHANNELS];
	aint_t grad[AUDIO_MAX_CHANNELS];
	unsigned int t;
	int step = track->freq_step;
	u_int channels;
	u_int ch;

	channels = src->fmt.channels;

	// 前回の最終サンプル
	for (ch = 0; ch < channels; ch++) {
		prev[ch] = track->freq_prev[ch];
		curr[ch] = track->freq_curr[ch];
		grad[ch] = curr[ch] - prev[ch];
	}

	t = track->freq_current;
//#define FREQ_DEBUG
#if defined(FREQ_DEBUG)
#define PRINTF(fmt...)	printf(fmt)
#else
#define PRINTF(fmt...)	/**/
#endif
	int srcused = src->used;
	PRINTF("start step=%d leap=%d", step, track->freq_leap);
	PRINTF(" srcused=%d arg->count=%d", src->used, arg->count);
	PRINTF(" prev=%d curr=%d grad=%d", prev[0], curr[0], grad[0]);
	PRINTF(" t=%d\n", t);

	int i;
	for (i = 0; i < arg->count; i++) {
		PRINTF("i=%d t=%5d", i, t);
		if (t >= 65536) {
			for (ch = 0; ch < channels; ch++) {
				// 前回値
				prev[ch] = curr[ch];
				// 今回値
				curr[ch] = *s++;
				// 傾き
				grad[ch] = curr[ch] - prev[ch];
			}
			PRINTF(" prev=%d s[%d]=%d",
			    prev[0], src->used - srcused, curr[0]);

			// 更新
			t -= 65536;
			srcused--;
			if (srcused < 0) {
				PRINTF(" break\n");
				break;
			}
		}

		for (ch = 0; ch < channels; ch++) {
			*d++ = prev[ch] + (aint2_t)grad[ch] * t / 65536;
#if defined(FREQ_DEBUG)
			if (ch == 0)
				printf(" t=%5d *d=%d", t, d[-1]);
#endif
		}
		t += step;

		PRINTF("\n");
	}
	PRINTF("end prev=%d curr=%d\n", prev[0], curr[0]);

	audio_ring_take(src, src->used);
	audio_ring_push(dst, i);

	// 補正
	t += track->freq_leap;

	track->freq_current = t;
	for (ch = 0; ch < channels; ch++) {
		track->freq_prev[ch] = prev[ch];
		track->freq_curr[ch] = curr[ch];
	}
}

// 周波数変換(ダウンサンプリング)をします。単純間引きです。
/*
 * audio_track_freq_down:
 *	This filter performs frequency conversion (down sampling).
 *	It uses simple thinning.
 */
static void
audio_track_freq_down(audio_filter_arg_t *arg)
{
	audio_track_t *track;
	audio_ring_t *src;
	audio_ring_t *dst;
	const aint_t *s0;
	aint_t *d;
	u_int i;
	u_int t;
	u_int step;
	int ch;
	int nchannels;

	track = arg->context;
	src = &track->freq.srcbuf;
	dst = track->freq.dst;

	KASSERT(track);
	KASSERT(audio_ring_is_valid(dst));
	KASSERT(audio_ring_is_valid(src));
	KASSERT(src->used > 0);
	KASSERT(src->fmt.channels == dst->fmt.channels);
	KASSERTMSG(src->head % track->mixer->frames_per_block == 0,
	    "src->head=%d fpb=%d",
	    src->head, track->mixer->frames_per_block);

	s0 = arg->src;
	d = arg->dst;
	t = track->freq_current;
	step = track->freq_step;
	nchannels = dst->fmt.channels;

	for (i = 0; i < arg->count && t / 65536 < src->used; i++) {
		const aint_t *s;
		s = s0 + (t / 65536) * nchannels;
		for (ch = 0; ch < nchannels; ch++) {
			*d++ = s[ch];
		}
		t += step;
	}
	t += track->freq_leap;
	audio_ring_take(src, src->used);
	audio_ring_push(dst, i);
	track->freq_current = t % 65536;
}

// トラックを初期化します。
// 初期化できれば 0 を返して *trackp に初期化済みのトラックを格納します。
// 初期化できなければ errno を返し、*trackp は変更しません。
// mode は再生なら AUMODE_PLAY、録音なら AUMODE_RECORD を指定します。
// 単に録音再生のどちら側かだけなので AUMODE_PLAY_ALL は関係ありません。
// trackp には sc_files に繋がっている file 構造体内のポインタを直接
// 指定してはいけません。呼び出し側で一旦受け取って sc_intr_lock を
// とってから繋ぎ変えてください。
/*
 * audio_track_init:
 *	Initialize the track.
 *	If successful, it stores the allocated and initialized track in
 *	*trackp and return 0.  Otherwise, it returns errno without
 *	modifying *trackp.
 *	For mode, specify AUMODE_PLAY for playback, AUMODE_RECORD for record.
 *	AUMODE_PLAY_ALL does not matter here.
 *	Don't specify the track within the file structure linked from
 *	sc->sc_files.
 */
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
	TRACET(track, "for %s", mode == AUMODE_PLAY ? "playback" : "recording");

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

	// 固定初期値
	track->volume = 256;
	for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
		track->ch_volume[i] = 256;
	}

	// デフォルトフォーマットでセット
	error = audio_track_set_format(track, default_format);
	if (error) {
		audio_track_destroy(track);
		return error;
	}

	*trackp = track;
	return 0;
}

// track のすべてのリソースと track 自身を解放します。
// sc_files に繋がっている file 構造体内の [pr]track ポインタを直接
// 指定してはいけません。呼び出し側で sc_intr_lock をとった上でリストから
// 外したものを指定してください。
/*
 * audio_track_destroy:
 *	Release all resources of track and track itself.
 *	track must not be NULL.  Don't specify the track within the file
 *	structure linked from sc->sc_files.
 */
void
audio_track_destroy(audio_track_t *track)
{
	// 関数仕様を track は NULL 許容にしてもいいけど、これを呼ぶところは
	// たいてい track が NULL でないと分かっていて呼んでるはずなので
	// ASSERT のほうがよかろう。
	KASSERT(track);

	audio_free_usrbuf(track);
	audio_free(track->codec.srcbuf.mem);
	audio_free(track->chvol.srcbuf.mem);
	audio_free(track->chmix.srcbuf.mem);
	audio_free(track->freq.srcbuf.mem);
	audio_free(track->outputbuf.mem);
	cv_destroy(&track->outchan);
	if (track->sih_wr) {
		softint_disestablish(track->sih_wr);
		track->sih_wr = NULL;
	}

	kmem_free(track, sizeof(*track));
}

/*
 * is_valid_filter_arg:
 *	Return true if filter argument 'arg' is correct.
 */
bool
is_valid_filter_arg(const audio_filter_arg_t *arg)
{
	KASSERT(arg != NULL);

	KASSERT(arg->src != NULL);
	KASSERT(arg->dst != NULL);
	if (!audio_format2_is_valid(arg->srcfmt)) {
		printf("%s: invalid srcfmt\n", __func__);
		return false;
	}
	if (!audio_format2_is_valid(arg->dstfmt)) {
		printf("%s: invalid dstfmt\n", __func__);
		return false;
	}
	if (arg->count <= 0) {
		printf("%s: count(%d) < 0\n", __func__, arg->count);
		return false;
	}
	return true;
}

// src, dst のフォーマットに応じて変換フィルタを返します。
// src か dst のどちらか一方が internal 形式でなければなりません。
// 変換できない組み合わせの場合は NULL を返します。
/*
 * audio_track_get_codec:
 *	It returns encoding conversion filter according to src and dst
 *	format.  If it is not a convertible pair, it returns NULL.
 *	Either src or dst must be internal format.
 */
static audio_filter_t
audio_track_get_codec(const audio_format2_t *src, const audio_format2_t *dst)
{

	if (audio_format2_is_internal(src)) {
		if (dst->encoding == AUDIO_ENCODING_ULAW) {
			return internal_to_mulaw;
		} else if (audio_format2_is_linear(dst)) {
			if (dst->stride == 8) {
				return internal_to_linear8;
			} else if (dst->stride == 16) {
				return internal_to_linear16;
#if defined(AUDIO_SUPPORT_LINEAR24)
			} else if (dst->stride == 24) {
				return internal_to_linear24;
#endif
			} else if (dst->stride == 32) {
				return internal_to_linear32;
			} else {
				DPRINTF(1, "%s: unsupported %s stride %d\n",
				    __func__, "dst", dst->stride);
				goto abort;
			}
		}
	} else if (audio_format2_is_internal(dst)) {
		if (src->encoding == AUDIO_ENCODING_ULAW) {
			return mulaw_to_internal;
		} else if (audio_format2_is_linear(src)) {
			if (src->stride == 8) {
				return linear8_to_internal;
			} else if (src->stride == 16) {
				return linear16_to_internal;
#if defined(AUDIO_SUPPORT_LINEAR24)
			} else if (src->stride == 24) {
				return linear24_to_internal;
#endif
			} else if (src->stride == 32) {
				return linear32_to_internal;
			} else {
				DPRINTF(1, "%s: unsupported %s stride %d\n",
				    __func__, "src", src->stride);
				goto abort;
			}
		}
	}

	DPRINTF(1, "unsupported encoding\n");
abort:
#if defined(AUDIO_DEBUG)
	{
		char buf[100];
		audio_format2_tostr(buf, sizeof(buf), src);
		printf("%s: src %s\n", __func__, buf);
		audio_format2_tostr(buf, sizeof(buf), dst);
		printf("%s: dst %s\n", __func__, buf);
	}
#endif
	return NULL;
}

// track の codec ステージを必要に応じて初期化します。
// 成功すれば、codec ステージが必要なら初期化した上で、いずれにしても
// 更新された last_dst を last_dstp に格納して、0 を返します。
// 失敗すれば last_dstp は更新せずに errno を返します。
/*
 * audio_track_init_codec:
 *	Initialize the codec stage of this track as necessary.
 *	If successful, it initializes the codec stage as necessary, stores
 *	updated last_dst in *last_dstp in any case, and returns 0.
 *	Otherwise, it returns errno without modifying *last_dstp.
 */
static int
audio_track_init_codec(audio_track_t *track, audio_ring_t **last_dstp)
{
	audio_ring_t *last_dst;
	audio_ring_t *srcbuf;
	audio_format2_t *srcfmt;
	audio_format2_t *dstfmt;
	audio_filter_arg_t *arg;
	int len;
	int error;

	KASSERT(track);

	last_dst = *last_dstp;
	dstfmt = &last_dst->fmt;
	srcfmt = &track->inputfmt;
	srcbuf = &track->codec.srcbuf;
	error = 0;

	if (srcfmt->encoding != dstfmt->encoding
	 || srcfmt->precision != dstfmt->precision
	 || srcfmt->stride != dstfmt->stride) {
		track->codec.dst = last_dst;

		srcbuf->fmt = *dstfmt;
		srcbuf->fmt.encoding = srcfmt->encoding;
		srcbuf->fmt.precision = srcfmt->precision;
		srcbuf->fmt.stride = srcfmt->stride;

		track->codec.filter = audio_track_get_codec(&srcbuf->fmt,
		    dstfmt);
		if (track->codec.filter == NULL) {
			DPRINTF(1, "%s: get_codec_filter failed\n", __func__);
			error = EINVAL;
			goto abort;
		}

		srcbuf->head = 0;
		srcbuf->used = 0;
		srcbuf->capacity = frame_per_block_roundup(track->mixer, &srcbuf->fmt);
		len = audio_ring_bytelen(srcbuf);
		srcbuf->mem = audio_realloc(srcbuf->mem, len);
		if (srcbuf->mem == NULL) {
			DPRINTF(1, "%s: malloc(%d) failed\n", __func__, len);
			error = ENOMEM;
			goto abort;
		}

		arg = &track->codec.arg;
		arg->srcfmt = &srcbuf->fmt;
		arg->dstfmt = dstfmt;
		arg->context = NULL;

		*last_dstp = srcbuf;
		return 0;
	}

abort:
	track->codec.filter = NULL;
	audio_free(srcbuf->mem);
	return error;
}

// track の chvol ステージを必要に応じて初期化します。
// 成功すれば、chvol ステージが必要なら初期化した上で、いずれにしても
// 更新された last_dst を last_dstp に格納して、0 を返します。
// 失敗すれば last_dstp は更新せずに errno を返します。
/*
 * audio_track_init_chvol:
 *	Initialize the chvol stage of this track as necessary.
 *	If successful, it initializes the chvol stage as necessary, stores
 *	updated last_dst in *last_dstp in any case, and returns 0.
 *	Otherwise, it returns errno without modifying *last_dstp.
 */
static int
audio_track_init_chvol(audio_track_t *track, audio_ring_t **last_dstp)
{
	audio_ring_t *last_dst;
	audio_ring_t *srcbuf;
	audio_format2_t *srcfmt;
	audio_format2_t *dstfmt;
	audio_filter_arg_t *arg;
	int len;
	int error;

	KASSERT(track);

	last_dst = *last_dstp;
	dstfmt = &last_dst->fmt;
	srcfmt = &track->inputfmt;
	srcbuf = &track->chvol.srcbuf;
	error = 0;

	/* Check whether channel volume conversion is necessary. */
	bool use_chvol = false;
	for (int ch = 0; ch < srcfmt->channels; ch++) {
		if (track->ch_volume[ch] != 256) {
			use_chvol = true;
			break;
		}
	}

	if (use_chvol == true) {
		track->chvol.dst = last_dst;
		track->chvol.filter = audio_track_chvol;

		srcbuf->fmt = *dstfmt;
		/* no format conversion occurs */

		srcbuf->head = 0;
		srcbuf->used = 0;
		srcbuf->capacity = frame_per_block_roundup(track->mixer, &srcbuf->fmt);
		len = audio_ring_bytelen(srcbuf);
		srcbuf->mem = audio_realloc(srcbuf->mem, len);
		if (srcbuf->mem == NULL) {
			DPRINTF(1, "%s: malloc(%d) failed\n", __func__, len);
			error = ENOMEM;
			goto abort;
		}

		arg = &track->chvol.arg;
		arg->srcfmt = &srcbuf->fmt;
		arg->dstfmt = dstfmt;
		arg->context = track->ch_volume;

		*last_dstp = srcbuf;
		return 0;
	}

abort:
	track->chvol.filter = NULL;
	audio_free(srcbuf->mem);
	return error;
}

// track の chmix ステージを必要に応じて初期化します。
// 成功すれば、chmix ステージが必要なら初期化した上で、いずれにしても
// 更新された last_dst を last_dstp に格納して、0 を返します。
// 失敗すれば last_dstp は更新せずに errno を返します。
/*
 * audio_track_init_chmix:
 *	Initialize the chmix stage of this track as necessary.
 *	If successful, it initializes the chmix stage as necessary, stores
 *	updated last_dst in *last_dstp in any case, and returns 0.
 *	Otherwise, it returns errno without modifying *last_dstp.
 */
static int
audio_track_init_chmix(audio_track_t *track, audio_ring_t **last_dstp)
{
	audio_ring_t *last_dst;
	audio_ring_t *srcbuf;
	audio_format2_t *srcfmt;
	audio_format2_t *dstfmt;
	audio_filter_arg_t *arg;
	int srcch;
	int dstch;
	int len;
	int error;

	KASSERT(track);

	last_dst = *last_dstp;
	dstfmt = &last_dst->fmt;
	srcfmt = &track->inputfmt;
	srcbuf = &track->chmix.srcbuf;
	error = 0;

	srcch = srcfmt->channels;
	dstch = dstfmt->channels;
	if (srcch != dstch) {
		track->chmix.dst = last_dst;

		if (srcch >= 2 && dstch == 1) {
			track->chmix.filter = audio_track_chmix_mixLR;
		} else if (srcch == 1 && dstch >= 2) {
			track->chmix.filter = audio_track_chmix_dupLR;
		} else if (srcch > dstch) {
			track->chmix.filter = audio_track_chmix_shrink;
		} else {
			track->chmix.filter = audio_track_chmix_expand;
		}

		srcbuf->fmt = *dstfmt;
		srcbuf->fmt.channels = srcch;

		srcbuf->head = 0;
		srcbuf->used = 0;
		// バッファサイズは計算で決められるはずだけど。とりあえず。
		srcbuf->capacity = frame_per_block_roundup(track->mixer, &srcbuf->fmt);
		len = audio_ring_bytelen(srcbuf);
		srcbuf->mem = audio_realloc(srcbuf->mem, len);
		if (srcbuf->mem == NULL) {
			DPRINTF(1, "%s: malloc(%d) failed\n", __func__, len);
			error = ENOMEM;
			goto abort;
		}

		arg = &track->chmix.arg;
		arg->srcfmt = &srcbuf->fmt;
		arg->dstfmt = dstfmt;
		arg->context = NULL;

		*last_dstp = srcbuf;
		return 0;
	}

abort:
	track->chmix.filter = NULL;
	audio_free(srcbuf->mem);
	return error;
}

// track の freq ステージを必要に応じて初期化します。
// 成功すれば、freq ステージが必要なら初期化した上で、いずれにしても
// 更新された last_dst を last_dstp に格納して、0 を返します。
// 失敗すれば last_dstp は更新せずに errno を返します。
/*
 * audio_track_init_freq:
 *	Initialize the freq stage of this track as necessary.
 *	If successful, it initializes the freq stage as necessary, stores
 *	updated last_dst in *last_dstp in any case, and returns 0.
 *	Otherwise, it returns errno without modifying *last_dstp.
 */
static int
audio_track_init_freq(audio_track_t *track, audio_ring_t **last_dstp)
{
	audio_ring_t *last_dst;
	audio_ring_t *srcbuf;
	audio_format2_t *srcfmt;
	audio_format2_t *dstfmt;
	audio_filter_arg_t *arg;
	uint32_t srcfreq;
	uint32_t dstfreq;
	int len;
	int error;

	KASSERT(track);

	last_dst = *last_dstp;
	dstfmt = &last_dst->fmt;
	srcfmt = &track->inputfmt;
	srcbuf = &track->freq.srcbuf;
	error = 0;

	srcfreq = srcfmt->sample_rate;
	dstfreq = dstfmt->sample_rate;
	if (srcfreq != dstfreq) {
		track->freq.dst = last_dst;

		memset(track->freq_prev, 0, sizeof(track->freq_prev));
		memset(track->freq_curr, 0, sizeof(track->freq_curr));

		// freq_step は dstfreq を 65536 とした時の src/dst 比
		track->freq_step = (uint64_t)srcfreq * 65536 / dstfreq;

		// freq_leap は1ブロックごとの freq_step の補正値
		// を四捨五入したもの。
		int dst_capacity = frame_per_block_roundup(track->mixer,
		    dstfmt);
		int mod = (uint64_t)srcfreq * 65536 % dstfreq;
		track->freq_leap = (mod * dst_capacity + dstfreq / 2) / dstfreq;

		if (track->freq_step < 65536) {
			track->freq.filter = audio_track_freq_up;
			// 初回に繰り上がりを起こすため 0 ではなく 65536 で初期化
			track->freq_current = 65536;
		} else {
			track->freq.filter = audio_track_freq_down;
			// こっちは 0 からでいい
			track->freq_current = 0;
		}

		srcbuf->fmt = *dstfmt;
		srcbuf->fmt.sample_rate = srcfreq;

		srcbuf->head = 0;
		srcbuf->used = 0;
		srcbuf->capacity = frame_per_block_roundup(track->mixer, &srcbuf->fmt);
		len = audio_ring_bytelen(srcbuf);
		srcbuf->mem = audio_realloc(srcbuf->mem, len);
		if (srcbuf->mem == NULL) {
			DPRINTF(1, "%s: malloc(%d) failed\n", __func__, len);
			error = ENOMEM;
			goto abort;
		}

		arg = &track->freq.arg;
		arg->srcfmt = &srcbuf->fmt;
		arg->dstfmt = dstfmt;/*&last_dst->fmt;*/
		arg->context = track;

		*last_dstp = srcbuf;
		return 0;
	}

abort:
	track->freq.filter = NULL;
	audio_free(srcbuf->mem);
	return error;
}

/*
 * When playing back: (e.g. if codec and freq stage are valid)
 *
 *               write
 *                | uiomove
 *                v
 *  usrbuf      [...............]  byte ring buffer (mmap-able)
 *                | memcpy
 *                v
 *  codec.srcbuf[....]             1 block (ring) buffer   <-- stage input
 *       .dst ----+
 *                | convert
 *                v
 *  freq.srcbuf [....]             1 block (ring) buffer
 *      .dst  ----+
 *                | convert
 *                v
 *  outputbuf   [...............]  NBLKOUT blocks ring buffer
 *
 *
 * When recording:
 *
 *  freq.srcbuf [...............]  NBLKOUT blocks ring buffer <-- stage input
 *      .dst  ----+
 *                | convert
 *                v
 *  codec.srcbuf[.....]            1 block (ring) buffer
 *       .dst ----+
 *                | convert
 *                v
 *  outputbuf   [.....]            1 block (ring) buffer
 *                | memcpy
 *                v
 *  usrbuf      [...............]  byte ring buffer (mmap-able *)
 *                | uiomove
 *                v
 *               read
 *
 *    *: recoding usrbuf is also mmap-able due to symmetry with playback
 *       but for now it will not be mmapped.
 */

// トラックのユーザランド側フォーマットを設定します。
// 変換用内部バッファは一度破棄されます。
// 成功すれば 0、失敗すれば errno を返します。
// uvm 関連のルーチンが非 intr_lock 状態であることを要求するため、必ず
// intr_lock でない状態で呼び出してください。
// ただし outputbuf を解放・再取得する可能性があるため、track が sc_files
// 上にある場合 (audio_file_setinfo_set() から呼ばれる時) は、in_use を
// セットしてから呼び出してください。
/*
 * audio_track_set_format:
 *	Set the userland format of this track.
 *	It will release and reallocate all internal conversion buffers.
 *	It returns 0 if successful.  Otherwise it returns errno with clearing
 *	all internal buffers.
 *	It must be called without intr_lock since uvm_* routines require
 *	non intr_lock state.
 *	It must be called with setting track->in_use if the track is within
 *	the file structure linked from sc->sc_files (as called from
 *	audio_file_setinfo_set), since it may release and reallocate outpubuf.
 */
int
audio_track_set_format(audio_track_t *track, audio_format2_t *usrfmt)
{
	int error;
	int newbufsize;
	u_int oldblksize;
	int len;

	KASSERT(track);
	KASSERT(audio_format2_is_valid(usrfmt));

	// 入力値チェック
	audio_check_params2(usrfmt);

	// ユーザランド側バッファ
	// 大きさは (ブロックサイズ * AUMINNOBLK) か 64KB の大きいほうにする。
	// ただし usrbuf は基本この fmt を参照せずに、バイトバッファとして扱う。
	track->usrbuf.fmt = *usrfmt;

	// 参考: 1ブロック(40msec)は
	// mulaw/8kHz/1ch で 320 byte    = 204 blocks/64KB
	// s16/48kHz/2ch  で 7680 byte   = 8 blocks/64KB
	// s16/48kHz/8ch  で 30720 byte  = 90KB/3blocks
	// s16/96kHz/8ch  で 61440 byte  = 180KB/3blocks
	// s32/192kHz/8ch で 245760 byte = 720KB/3blocks
	//
	// ちなみに N7 は 64KB を if->round_buffersize() したものとしている。
	// ブロックサイズが 64KB を越えるケースはどうなるか未調査。
	/*
	 * For example,
	 * 1) If usrbuf_blksize = 7056 (s16/44.1k/2ch) and PAGE_SIZE = 8192,
	 *     newbufsize = rounddown(65536 / 7056) = 63504
	 *     newvsize = roundup2(63504, PAGE_SIZE) = 65536
	 *    Therefore it maps 8 * 8K pages and usrbuf->capacity = 63504.
	 *
	 * 2) If usrbuf_blksize = 7680 (s16/48k/2ch) and PAGE_SIZE = 4096,
	 *     newbufsize = rounddown(65536 / 7680) = 61440
	 *     newvsize = roundup2(61440, PAGE_SIZE) = 61440 (= 15 pages)
	 *    Therefore it maps 15 * 4K pages and usrbuf->capacity = 61440.
	 */
	// XXX 64KB と言っておいて 15ページしかとらないケースがあるの大丈夫かな
	// XXX 初期値の mulaw でも 64KB 取るので、/dev/audio 開いて 48k に設定
	//     すると 16page 確保してすぐ解放して 15page 再確保ってなるけど
	//     どうなん…。
	oldblksize = track->usrbuf_blksize;
	track->usrbuf_blksize = frametobyte(&track->usrbuf.fmt,
	    frame_per_block_roundup(track->mixer, &track->usrbuf.fmt));
	track->usrbuf.head = 0;
	track->usrbuf.used = 0;
	newbufsize = MAX(track->usrbuf_blksize * AUMINNOBLK, 65536);
	newbufsize = rounddown(newbufsize, track->usrbuf_blksize);
	error = audio_realloc_usrbuf(track, newbufsize);
	if (error) {
		DPRINTF(1, "%s: malloc usrbuf(%d) failed\n", __func__,
		    newbufsize);
		goto error;
	}

	/* Recalc water mark. */
	if (track->usrbuf_blksize != oldblksize) {
		if (audio_track_is_playback(track)) {
			/* Set high at 100%, low at 75%.  */
			track->usrbuf_usedhigh = track->usrbuf.capacity;
			track->usrbuf_usedlow = track->usrbuf.capacity * 3 / 4;
		} else {
			/* Set high at 100% minus 1block(?), low at 0% */
			track->usrbuf_usedhigh = track->usrbuf.capacity -
			    track->usrbuf_blksize;
			track->usrbuf_usedlow = 0;
		}
	}

	/* Stage buffer */
	audio_ring_t *last_dst = &track->outputbuf;
	if (audio_track_is_playback(track)) {
		/* On playback, initialize from the mixer side in order. */
		track->inputfmt = *usrfmt;
		track->outputbuf.fmt =  track->mixer->track_fmt;

		if ((error = audio_track_init_freq(track, &last_dst)) != 0)
			goto error;
		if ((error = audio_track_init_chmix(track, &last_dst)) != 0)
			goto error;
		if ((error = audio_track_init_chvol(track, &last_dst)) != 0)
			goto error;
		if ((error = audio_track_init_codec(track, &last_dst)) != 0)
			goto error;
	} else {
		/* On recording, initialize from userland side in order. */
		track->inputfmt = track->mixer->track_fmt;
		track->outputbuf.fmt = *usrfmt;

		if ((error = audio_track_init_codec(track, &last_dst)) != 0)
			goto error;
		if ((error = audio_track_init_chvol(track, &last_dst)) != 0)
			goto error;
		if ((error = audio_track_init_chmix(track, &last_dst)) != 0)
			goto error;
		if ((error = audio_track_init_freq(track, &last_dst)) != 0)
			goto error;
	}
#if 0
	/* debug */
	if (track->freq.filter) {
		audio_print_format2("freq src", &track->freq.srcbuf.fmt);
		audio_print_format2("freq dst", &track->freq.dst->fmt);
	}
	if (track->chmix.filter) {
		audio_print_format2("chmix src", &track->chmix.srcbuf.fmt);
		audio_print_format2("chmix dst", &track->chmix.dst->fmt);
	}
	if (track->chvol.filter) {
		audio_print_format2("chvol src", &track->chvol.srcbuf.fmt);
		audio_print_format2("chvol dst", &track->chvol.dst->fmt);
	}
	if (track->codec.filter) {
		audio_print_format2("codec src", &track->codec.srcbuf.fmt);
		audio_print_format2("codec dst", &track->codec.dst->fmt);
	}
#endif

	/* Stage input buffer */
	track->input = last_dst;
	// 録音時は先頭のステージをリングバッファにする
	// XXX もっとましな方法でやったほうがいい
	if (audio_track_is_record(track)) {
		track->input->capacity = NBLKOUT *
		    frame_per_block_roundup(track->mixer, &track->input->fmt);
		len = audio_ring_bytelen(track->input);
		track->input->mem = audio_realloc(track->input->mem, len);
		if (track->input->mem == NULL) {
			DPRINTF(1, "%s: malloc input(%d) failed\n", __func__,
			    len);
			error = ENOMEM;
			goto error;
		}
	}

	// 出力フォーマットに従って outputbuf を作る
	// 再生側 outputbuf は NBLKOUT 分。録音側は1ブロックでいい。
	/*
	 * Output buffer.
	 * On the playback track, its capacity is NBLKOUT blocks.
	 * On the recording track, its capacity is 1 block.
	 */
	track->outputbuf.head = 0;
	track->outputbuf.used = 0;
	track->outputbuf.capacity = frame_per_block_roundup(track->mixer, &track->outputbuf.fmt);
	if (audio_track_is_playback(track))
		track->outputbuf.capacity *= NBLKOUT;
	len = audio_ring_bytelen(&track->outputbuf);
	track->outputbuf.mem = audio_realloc(track->outputbuf.mem, len);
	if (track->outputbuf.mem == NULL) {
		DPRINTF(1, "%s: malloc outbuf(%d) failed\n", __func__, len);
		error = ENOMEM;
		goto error;
	}

#if AUDIO_DEBUG > 2
	struct audio_track_debugbuf m;

	memset(&m, 0, sizeof(m));
	snprintf(m.outbuf, sizeof(m.outbuf), " out=%d",
	    track->outputbuf.capacity *
	    frametobyte(&track->outputbuf.fmt, 1));
	if (track->freq.filter)
		snprintf(m.freq, sizeof(m.freq), " freq=%d",
		    track->freq.srcbuf.capacity *
		    frametobyte(&track->freq.srcbuf.fmt, 1));
	if (track->chmix.filter)
		snprintf(m.chmix, sizeof(m.chmix), " chmix=%d",
		    track->chmix.srcbuf.capacity *
		    frametobyte(&track->chmix.srcbuf.fmt, 1));
	if (track->chvol.filter)
		snprintf(m.chvol, sizeof(m.chvol), " chvol=%d",
		    track->chvol.srcbuf.capacity *
		    frametobyte(&track->chvol.srcbuf.fmt, 1));
	if (track->codec.filter)
		snprintf(m.codec, sizeof(m.codec), " codec=%d",
		    track->codec.srcbuf.capacity *
		    frametobyte(&track->codec.srcbuf.fmt, 1));
	snprintf(m.usrbuf, sizeof(m.usrbuf),
	    " usr=%d", track->usrbuf.capacity);

	if (audio_track_is_playback(track)) {
		TRACET(track, "bufsize%s%s%s%s%s%s",
		    m.outbuf, m.freq, m.chmix, m.chvol, m.codec, m.usrbuf);
	} else {
		TRACET(track, "bufsize%s%s%s%s%s%s",
		    m.freq, m.chmix, m.chvol, m.codec, m.outbuf, m.usrbuf);
	}
#endif
	return 0;

error:
	audio_free_usrbuf(track);
	audio_free(track->codec.srcbuf.mem);
	audio_free(track->chvol.srcbuf.mem);
	audio_free(track->chmix.srcbuf.mem);
	audio_free(track->freq.srcbuf.mem);
	audio_free(track->outputbuf.mem);
	return error;
}

// ring が空でなく 1 ブロックに満たない時、1ブロックまで無音を追加します。
// 追加したフレーム数を返します。
/*
 * audio_append_silence:
 *	Fill the silence frames (in the internal format) up to 1 block if
 *	the ring is less than 1 block and not empty.
 *	It returns the number of appended frames.
 */
static int
audio_append_silence(audio_track_t *track, audio_ring_t *ring)
{
	KASSERT(track);
	KASSERT(audio_format2_is_internal(&ring->fmt));

	// XXX n の計算あれでいいんだろうか。最初から contig_free じゃなくて?
	// XXX memset の長さ frametobyte じゃなくて?

	if (ring->used == 0) return 0;

	int fpb = frame_per_block_roundup(track->mixer, &ring->fmt);
	if (ring->used >= fpb) {
		return 0;
	}

	int n = (ring->capacity - ring->used) % fpb;

	KASSERT(audio_ring_get_contig_free(ring) >= n);

	memset(audio_ring_tailptr_aint(ring), 0,
	    n * ring->fmt.channels * sizeof(aint_t));
	audio_ring_push(ring, n);
	return n;
}

// どこに書くのがいいか分からんけど、フィルタの仕様みたいなもの。
//
// 概要
//	変換フィルタは以下に分類できます。
//	  1. ユーザランド側フィルタ
//	   1a. 周波数変換フィルタ
//	   1b. それ以外のフィルタ
//	  2. HW フィルタ
//
//	1 のユーザランド側フィルタはすべて audio layer が責任を持っています
//	ので MD ドライバは通常使用することはありません。ただし HW フォーマット
//	が mulaw なデバイスのように、audio layer が持っている
//	mulaw <-> aint_t 変換関数をそのまま利用できる場合にはこれを使用
//	することが可能です。
//
//	audio layer が MD ドライバに受け渡すフォーマットは内部形式と呼ぶもので、
//	slinear_NE、16 bit、HW channels、HW frequency です。HW がこれをそのまま
//	扱えるのなら MD フィルタは不要、encoding/precision を変換する必要が
//	あれば MD フィルタが必要です。MD フィルタは通常 slinear_NE、16bit と
//	HW エンコーディングの変換だけを受け持ちます。
//
//	変換関数(フィルタ)は以下のプロトタイプを持ちます。
//
//	  typedef struct {
//		const void *src;
//		const audio_format2_t *srcfmt;
//		void *dst;
//		const audio_format2_t *dstfmt;
//		int count;
//		void *context;
//	  } audio_filter_arg_t;
//
//	  void filter(audio_filter_arg_t *arg);
//
//	変換に必要なパラメータは arg として渡されます。filter() は arg->src
//	から arg->count 個のフレームを読み込んで変換し arg->dst から
//	arg->count 個のフレームに出力します。arg->src および arg->dst は
//	arg->count フレームの読み書きが連続して行えることを保証しています。
//
//	arg->count 個を全部処理する前の早期終了や途中でのエラー終了は今の所
//	認められていません。また arg->src、arg->dst ポインタの進み具合によって
//	呼び出し元に実際の読み込み数や書き込み数を通知することは出来ませんので
//	arg->src、arg->dst は filter() 側で破壊(変更)しても構いません。
//
//	同様に arg->count も filter() 側で破壊(変更)可能です。
//
//	arg->srcfmt, arg->dstfmt には入出力のフォーマットが記述されています。
//	通常フィルタは自分自身が何から何への変換なのかを知っているので入出力
//	フォーマットをチェックする必要はないでしょう (例えば mulaw から
//	aint_t への変換、など)。一方リニア PCM から aint_t への変換を
//	すべて受け持つフィルタの場合は srcfmt をチェックする必要があるでしょう。
//
//	context はフィルタ自身が自由に使用可能なポインタです。audio layer は
//	この値について一切関与しません。
//
// 周波数変換フィルタ
//	周波数変換フィルタだけ入出力のフレーム数が変化するため、いろいろと
//	特別対応が必要です。まず入出力リングバッファは再生/録音の両方向とも
//	freq.srcbuf、freq.dst なのでそれはもう直接使ってしまうことにします。
//
//	つまり filter() 呼び出し時点で要求出力フレーム数は arg->count フレーム、
//	入力可能なフレーム数は freq.srcbuf.used フレームです。
//
//	変換処理によって消費した入力フレーム数は freq.srcbuf を、出力した
//	フレーム数は freq.dst を filter() 側が更新してください。
//	arg->count は破壊(変更)して構いません。
//

// 変換ステージを実行します。
// stage->filter != NULL の時だけ呼び出してください。
// stage から arg を用意して stage->filter を処理します。
// 周波数変換なら src, dst のカウンタはフィルタ側で進めてください。
// 周波数変換以外なら src, dst のカウンタはここで進めます。
/*
 * audio_apply_stage:
 *	Execute the conversion stage.
 *	It prepares arg from this stage and executes stage->filter.
 *	It must be called only if stage->filter is not NULL.
 *
 *	For stages other than frequency conversion, the src and dst counters
 *	are incremented here.  On the other hand, for frequency conversion
 *	stage, it's the responsibility of the filter side to increment src
 *	and dst counters.
 */
static void
audio_apply_stage(audio_track_t *track, audio_stage_t *stage, bool isfreq)
{
	KASSERT(track);
	KASSERT(stage->filter);

	int srccount = audio_ring_get_contig_used(&stage->srcbuf);
	int dstcount = audio_ring_get_contig_free(stage->dst);
	int count;

	if (isfreq) {
		KASSERTMSG(srccount > 0,
		    "freq but srccount == %d", srccount);
		count = min(dstcount, track->mixer->frames_per_block);
	} else {
		count = min(srccount, dstcount);
	}

	if (count > 0) {
		audio_filter_arg_t *arg = &stage->arg;

		arg->src = audio_ring_headptr(&stage->srcbuf);
		arg->dst = audio_ring_tailptr(stage->dst);
		arg->count = count;

		stage->filter(arg);

		if (!isfreq) {
			audio_ring_take(&stage->srcbuf, count);
			audio_ring_push(stage->dst, count);
		}
	}
}

// 再生時の入力データを変換してトラックバッファに投入します。
// usrbuf が空でないことは呼び出し側でチェックしてから呼んでください。
// outputbuf に1ブロック以上の空きがあることは呼び出し側でチェックしてから
// 呼んでください。
void
audio_track_play(audio_track_t *track)
{
	audio_ring_t *usrbuf;
	audio_ring_t *input;
	int count;
	int framesize;	// input の1フレームのバイト数
	int bytes;		// usrbuf から input に転送するバイト数
	u_int dropcount;

	KASSERT(track);
	TRACET(track, "start pstate=%d", track->pstate);

	// この時点で usrbuf は空ではない。
	/* At this point usrbuf should not be empty. */
	KASSERT(track->usrbuf.used > 0);
	// また outputbuf に1ブロック以上の空きがある。
	/* Also, outputbuf should be available at least one block. */
	count = audio_ring_get_contig_free(&track->outputbuf);
	KASSERTMSG(count >= frame_per_block_roundup(track->mixer, &track->outputbuf.fmt),
	    "count=%d fpb=%d",
	    count, frame_per_block_roundup(track->mixer, &track->outputbuf.fmt));

	int track_count_0 = track->outputbuf.used;

	usrbuf = &track->usrbuf;
	input = track->input;
	dropcount = 0;

	// 入力(usrfmt) に 4bit は来ないので1フレームは必ず1バイト以上ある
	/*
	 * framesize is always 1 byte or more since all formats supported as
	 * usrfmt(=input) have 8bit or more stride.
	 */
	framesize = frametobyte(&input->fmt, 1);
	KASSERT(framesize >= 1);

	// usrbuf の次段(input) も空いてるはず
	/* The next stage of usrbuf (=input) should be available. */
	KASSERT(audio_ring_get_contig_free(input) > 0);

	// usrbuf の最大1ブロックを input へコピー
	// count は usrbuf からコピーするフレーム数。
	// bytes は usrbuf からコピーするバイト数。
	// ただし1フレーム未満のバイトはコピーしない。
	/*
	 * Copy usrbuf up to 1block to input buffer.
	 * count is the number of frames to copy from usrbuf.
	 * bytes is the number of bytes to copy from usrbuf.  However it is
	 * not copied less than 1 frame.
	 */
	count = min(usrbuf->used, track->usrbuf_blksize) / framesize;
	bytes = count * framesize;

	// 今回処理するバイト数(bytes) が1ブロックに満たない場合、
	//  drain AUMODE
	//  ----- ------
	//  no    PLAY  : 溜まっていないのでここで帰る
	//  no    SYNC  : dropframes加算する。  リングバッファリセット。
	//  yes   PLAY  : dropframes加算しない。リングバッファリセット。
	//  yes   SYNC  : dropframes加算しない。リングバッファリセット。
	if (count < track->usrbuf_blksize / framesize) {
		dropcount = track->usrbuf_blksize / framesize - count;

		if (track->pstate != AUDIO_STATE_DRAINING) {
			if ((track->mode & AUMODE_PLAY_ALL)) {
				// PLAY_ALL なら溜まるまで待つ
				/* For PLAY_ALL, wait until filled. */
				TRACET(track, "not enough; return");
				return;
			} else {
				// ここで1ブロックに満たなければ
				// 落ちる(落ちた)ということ
				/* Drop occurred. */

				// playdrop は今落とし中のフレーム数
				// 回復運転のために数えておく必要がある
				/*
				 * playdrop is the number of frames currently
				 * being dropped.  It is necessary to remember
				 * due to recover sync.
				 */
				track->playdrop += dropcount;
				// dropframes は数え上げるだけ。
				// 1回でも落ちれば play.error を立てるため
				/*
				 * dropframes only counts up due to set
				 * play.error if drops even once.
				 */
				track->dropframes += dropcount;
			}
		}
	}

	// stamp はハードウェアで再生したバイト数に相当するので
	// 無音挿入分も入れてここでカウントする。
	// この時点で必ず1ブロック分になってる気がする。
	track->usrbuf_stamp += count * framesize;

	if (usrbuf->head + bytes < usrbuf->capacity) {
		memcpy((uint8_t *)input->mem +
		        audio_ring_tail(input) * framesize,
		    (uint8_t *)usrbuf->mem + usrbuf->head,
		    bytes);
		audio_ring_push(input, count);
		audio_ring_take(usrbuf, bytes);
	} else {
		int bytes1 = audio_ring_get_contig_used(usrbuf);
		KASSERT(bytes1 % framesize == 0);
		memcpy((uint8_t *)input->mem +
		        audio_ring_tail(input) * framesize,
		    (uint8_t *)usrbuf->mem + usrbuf->head,
		    bytes1);
		audio_ring_push(input, bytes1 / framesize);
		audio_ring_take(usrbuf, bytes1);

		int bytes2 = bytes - bytes1;
		memcpy((uint8_t *)input->mem +
		        audio_ring_tail(input) * framesize,
		    (uint8_t *)usrbuf->mem + usrbuf->head,
		    bytes2);
		audio_ring_push(input, bytes2 / framesize);
		audio_ring_take(usrbuf, bytes2);
	}

	// エンコーディング変換
	/* Encoding conversion */
	if (track->codec.filter)
		audio_apply_stage(track, &track->codec, false);

	// チャンネルボリューム
	/* Channel volume */
	if (track->chvol.filter)
		audio_apply_stage(track, &track->chvol, false);

	// チャンネルミキサ
	/* Channel mix */
	if (track->chmix.filter)
		audio_apply_stage(track, &track->chmix, false);

	// 周波数変換
	// 1ブロックごとに誤差補正があるので必ず1ブロックにする
	/* Frequency conversion */
	/*
	 * Since the frequency conversion needs correction for each block,
	 * it rounds up to 1 block.
	 */
	if (track->freq.filter) {
		int n = 0;
		n = audio_append_silence(track, &track->freq.srcbuf);
		if (n > 0) {
			TRACET(track,
			    "freq.srcbuf add silence %d -> %d/%d/%d",
			    n,
			    track->freq.srcbuf.head,
			    track->freq.srcbuf.used,
			    track->freq.srcbuf.capacity);
		}
		if (track->freq.srcbuf.used > 0) {
			audio_apply_stage(track, &track->freq, true);
		}
	}

	if (dropcount != 0) {
		// 変換が1ブロックぴったりでない場合、変換バッファのポインタが
		// バッファ途中を指すことになるのでこれをクリアする。
		// ここの変換バッファは、前後のリングバッファとの対称性で
		// リングバッファの形にしてあるが運用上はただのバッファなので、
		// ポインタが途中を指されていると困る。
		// これが起きるのは PLAY_SYNC か drain 中の時。
		/*
		 * Clear all conversion buffer pointer if the conversion was
		 * not exactly one block.  These conversion stage buffers are
		 * certainly circular buffers because of symmetry with the
		 * previous and next stage buffer.  However, since they are
		 * treated as simple contiguous buffers in operation, so head
		 * always should point 0.  This may happen during PLAY(_SYNC)
		 * or drain-age.
		 */
		TRACET(track, "reset stage");
		if (track->codec.filter) {
			KASSERT(track->codec.srcbuf.used == 0);
			track->codec.srcbuf.head = 0;
		}
		if (track->chvol.filter) {
			KASSERT(track->chvol.srcbuf.used == 0);
			track->chvol.srcbuf.head = 0;
		}
		if (track->chmix.filter) {
			KASSERT(track->chmix.srcbuf.used == 0);
			track->chmix.srcbuf.head = 0;
		}
		if (track->freq.filter) {
			KASSERT(track->freq.srcbuf.used == 0);
			track->freq.srcbuf.head = 0;
		}
	}

	if (track->input == &track->outputbuf) {
		track->outputcounter = track->inputcounter;
	} else {
		track->outputcounter += track->outputbuf.used - track_count_0;
	}

#if AUDIO_DEBUG > 2
	struct audio_track_debugbuf m;
	audio_track_bufstat(track, &m);
	TRACET(track, "end%s%s%s%s%s%s",
	    m.outbuf, m.freq, m.chvol, m.chmix, m.codec, m.usrbuf);
#endif
}

// 録音時、ミキサーによってトラックの input に渡されたブロックを
// usrbuf まで変換します。
void
audio_track_record(audio_track_t *track)
{
	audio_ring_t *outputbuf;
	audio_ring_t *usrbuf;
	int count;
	int bytes;
	int framesize;

	KASSERT(track);

	// 処理するフレーム数
	/* number of frames to process */
	count = audio_ring_get_contig_used(track->input);
	count = min(count, track->mixer->frames_per_block);
	if (count == 0) {
		TRACET(track, "count == 0");
		return;
	}

	// 周波数変換
	/* Frequency conversion */
	if (track->freq.filter) {
		if (track->freq.srcbuf.used > 0) {
			audio_apply_stage(track, &track->freq, true);
			// XXX freq の入力は先頭からでなくてよいか?
		}
	}

	// チャンネルミキサ
	/* Channel Mix */
	if (track->chmix.filter)
		audio_apply_stage(track, &track->chmix, false);

	// チャンネルボリューム
	/* Channel Volume */
	if (track->chvol.filter)
		audio_apply_stage(track, &track->chvol, false);

	// エンコーディング変換
	/* Encoding Conversion */
	if (track->codec.filter)
		audio_apply_stage(track, &track->codec, false);

	// outputbuf から usrbuf へ
	/* Copy from outputbuf to usrbuf */
	outputbuf = &track->outputbuf;
	usrbuf = &track->usrbuf;
	// 出力(outputbuf)に 4bit は来ないので1フレームは必ず1バイト以上ある
	/*
	 * framesize is always 1 byte or more since all formats supported as
	 * usrfmt(=output) have 8bit or more stride.
	 */
	framesize = frametobyte(&outputbuf->fmt, 1);
	KASSERT(framesize >= 1);
	// count は usrbuf にコピーするフレーム数。
	// bytes は usrbuf にコピーするバイト数。
	/*
	 * count is the number of frames to copy to usrbuf.
	 * bytes is the number of bytes to copy to usrbuf.
	 */
	count = outputbuf->used;
	count = min(count,
	    (track->usrbuf_usedhigh - usrbuf->used) / framesize);
	bytes = count * framesize;
	if (audio_ring_tail(usrbuf) + bytes < usrbuf->capacity) {
		memcpy((uint8_t *)usrbuf->mem + audio_ring_tail(usrbuf),
		    (uint8_t *)outputbuf->mem + outputbuf->head * framesize,
		    bytes);
		audio_ring_push(usrbuf, bytes);
		audio_ring_take(outputbuf, count);
	} else {
		int bytes1, bytes2;

		bytes1 = audio_ring_get_contig_used(usrbuf);
		KASSERT(bytes1 % framesize == 0);
		memcpy((uint8_t *)usrbuf->mem + audio_ring_tail(usrbuf),
		    (uint8_t *)outputbuf->mem + outputbuf->head * framesize,
		    bytes1);
		audio_ring_push(usrbuf, bytes1);
		audio_ring_take(outputbuf, bytes1 / framesize);

		bytes2 = bytes - bytes1;
		memcpy((uint8_t *)usrbuf->mem + audio_ring_tail(usrbuf),
		    (uint8_t *)outputbuf->mem + outputbuf->head * framesize,
		    bytes2);
		audio_ring_push(usrbuf, bytes2);
		audio_ring_take(outputbuf, bytes2 / framesize);
	}

	// XXX カウンタ

#if AUDIO_DEBUG > 2
	struct audio_track_debugbuf m;
	audio_track_bufstat(track, &m);
	TRACET(track, "end%s%s%s%s%s%s",
	    m.freq, m.chvol, m.chmix, m.codec, m.outbuf, m.usrbuf);
#endif
}

// blktime は1ブロックの時間 [msec]。
//
// 例えば HW freq = 44100 に対して、
// blktime 50 msec は 2205 frame/block でこれは割りきれるので問題ないが、
// blktime 25 msec は 1102.5 frame/block となり、フレーム数が整数にならない。
// この場合 frame/block を切り捨てるなり切り上げるなりすれば整数にはなる。
// 例えば切り捨てて 1102 frame/block とするとこれに相当する1ブロックの時間は
// 24.9886… [msec] と割りきれなくなる。周波数がシステム中で1つしかなければ
// これでも構わないが、AUDIO2 ではブロック単位で周波数変換を行うため極力
// 整数にしておきたい (整数にしておいても割りきれないケースは出るが後述)。
//
// ここではより多くの周波数に対して frame/block が整数になりやすいよう
// AUDIO_BLK_MS の初期値を 40 msec に設定してある。
//   8000 [Hz] * 40 [msec] = 320 [frame/block] (8000Hz - 48000Hz 系)
//  11025 [Hz] * 40 [msec] = 441 [frame/block] (44100Hz 系)
//  15625 [Hz] * 40 [msec] = 625 [frame/block]
//
// これにより主要な周波数についてはわりと誤差(端数)なく周波数変換が行える。
// 例えば 44100 [Hz] を 48000 [Hz] に変換する場合 40 [msec] ブロックなら
//  44100 [Hz] * 40 [msec] = 1764 [frame/block]
//                           1920 [frame/block] = 48000 [Hz] * 40 [msec]
// となり、1764 フレームを 1920 フレームに変換すればよいことになる。
// ただし、入力周波数も HW 周波数も任意であるため、周波数変換前後で
// frame/block が必ずしもきりのよい値になるとは限らないが、そこはどのみち
// 仕方ない。(あくまで、主要な周波数で割り切れやすい、ということ)
//
// また、いくつかの変態ハードウェアではさらに手当てが必要。
//
// 1) vs(4) x68k MSM6258 ADPCM
//  vs(4) は 15625 Hz、4bit、1channel である。このため
//  blktime 40 [msec] は 625 [frame/block] と割りきれる値になるが、これは
//  同時に 312.5 [byte/block] であり、バイト数が割りきれないため、これは不可。
//  blktime 80 [msec] であれば 1250 [frame/block] = 625 [byte/block] となる
//  のでこれなら可。
//  vs(4) 以外はすべて stride が 8 の倍数なので、この「frame/block は割り
//  切れるのに byte/block にすると割りきれない」問題は起きない。
//
//  # 世の中には 3bit per frame とかいう ADPCM もあるにはあるが、
//  # 現行 NetBSD はこれをサポートしておらず、今更今後サポートするとも
//  # 思えないのでこれについては考慮しない。やりたい人がいたら頑張って。
//
// 2) aucc(4) amiga
//  周波数が変態だが詳細未調査。

// mixer(.hwbuf.fmt) から blktime [msec] を計算します。
/*
 * audio_mixer_calc_blktime:
 *	Calcurate blktime [msec] from mixer(.hwbuf.fmt).
 */
static u_int
audio_mixer_calc_blktime(audio_trackmixer_t *mixer)
{
	audio_format2_t *fmt;
	u_int blktime;
	u_int frames_per_block;

	fmt = &mixer->hwbuf.fmt;

	// XXX とりあえず手抜き実装。あとでかんがえる

	blktime = AUDIO_BLK_MS;

	// 8 の倍数以外の stride は今のところ 4 しかない。
	if (fmt->stride == 4) {
		frames_per_block = fmt->sample_rate * blktime / 1000;
		if ((frames_per_block & 1) != 0)
			blktime *= 2;
	}
#ifdef DIAGNOSTIC
	else if (fmt->stride % NBBY != 0) {
		panic("unsupported HW stride %d", fmt->stride);
	}
#endif

	return blktime;
}

// ミキサを初期化します。
// mixer はゼロフィルされているものとします。
// mode は再生なら AUMODE_PLAY、録音なら AUMODE_RECORD を指定します。
// 単に録音再生のどちら側かだけなので AUMODE_PLAY_ALL は関係ありません。
/*
 * audio_mixer_init:
 *	Initialize the mixer.
 *	This function returns 0 on sucessful.  Otherwise returns errno.
 *	'mixer' should be zero-filled.
 *	For 'mode', specify AUMODE_PLAY for playback, AUMODE_RECORD for
 *	record.  AUMODE_PLAY_ALL does not matter here.
 */
int
audio_mixer_init(struct audio_softc *sc, audio_trackmixer_t *mixer, int mode)
{
	int len;
	int blksize;
	int capacity;
	size_t bufsize;
	int error;

	error = 0;
	mixer->sc = sc;
	mixer->mode = mode;

	// XXX とりあえず
	if (mode == AUMODE_PLAY)
		mixer->hwbuf.fmt = sc->sc_phwfmt;
	else
		mixer->hwbuf.fmt = sc->sc_rhwfmt;

	mixer->blktime_d = 1000;
	mixer->blktime_n = audio_mixer_calc_blktime(mixer);
	mixer->hwblks = NBLKHW;

	mixer->frames_per_block = frame_per_block_roundup(mixer, &mixer->hwbuf.fmt);
	blksize = frametobyte(&mixer->hwbuf.fmt, mixer->frames_per_block);
	if (sc->hw_if->round_blocksize) {
		int rounded;
		audio_params_t p = format2_to_params(&mixer->hwbuf.fmt);
		mutex_enter(sc->sc_lock);
		rounded = sc->hw_if->round_blocksize(sc->hw_hdl, blksize, mode, &p);
		mutex_exit(sc->sc_lock);
		// 違っていても困る?
		if (rounded != blksize) {
			if ((rounded * NBBY) % (mixer->hwbuf.fmt.stride * mixer->hwbuf.fmt.channels) != 0) {
				aprint_error_dev(sc->dev, "blksize not configured"
					" %d -> %d\n", blksize, rounded);
				return ENXIO;
			}
			// 再計算
			mixer->frames_per_block = rounded * NBBY / (mixer->hwbuf.fmt.stride * mixer->hwbuf.fmt.channels);
		}
	}
	mixer->blktime_n = mixer->frames_per_block;
	mixer->blktime_d = mixer->hwbuf.fmt.sample_rate;

	capacity = mixer->frames_per_block * mixer->hwblks;
	bufsize = frametobyte(&mixer->hwbuf.fmt, capacity);
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
		mixer->hwbuf.mem = sc->hw_if->allocm(sc->hw_hdl, mode, bufsize);
		if (mixer->hwbuf.mem == NULL) {
			aprint_error_dev(sc->dev, "%s: allocm(%zu) failed",
			    __func__, bufsize);
			return ENOMEM;
		}
	} else {
		mixer->hwbuf.mem = kern_malloc(bufsize, M_NOWAIT);
		if (mixer->hwbuf.mem == NULL) {
			aprint_error_dev(sc->dev, "%s: malloc(%zu) failed",
			    __func__, bufsize);
			return ENOMEM;
		}
	}

	mixer->track_fmt.encoding = AUDIO_ENCODING_SLINEAR_NE;
	mixer->track_fmt.precision = AUDIO_INTERNAL_BITS;
	mixer->track_fmt.stride = AUDIO_INTERNAL_BITS;
	mixer->track_fmt.channels = mixer->hwbuf.fmt.channels;
	mixer->track_fmt.sample_rate = mixer->hwbuf.fmt.sample_rate;

	if (mode == AUMODE_PLAY) {
		// 合成バッファ
		mixer->mixfmt = mixer->track_fmt;
		mixer->mixfmt.precision *= 2;
		mixer->mixfmt.stride *= 2;
		// XXX マクロとか使えないんだっけ
		len = mixer->frames_per_block * mixer->mixfmt.channels * mixer->mixfmt.stride / NBBY;
		mixer->mixsample = audio_realloc(mixer->mixsample, len);
		if (mixer->mixsample == NULL) {
			aprint_error_dev(sc->dev, "%s: malloc(%d) failed",
			    __func__, len);
			error = ENOMEM;
			goto abort;
		}
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
	if (mixer->codec) {
		mixer->codecarg.context = reg->context;
		if (mode == AUMODE_PLAY) {
			mixer->codecarg.srcfmt = &mixer->track_fmt;
			mixer->codecarg.dstfmt = &mixer->hwbuf.fmt;
		} else {
			mixer->codecarg.srcfmt = &mixer->hwbuf.fmt;
			mixer->codecarg.dstfmt = &mixer->track_fmt;
		}
		mixer->codecbuf.fmt = mixer->track_fmt;
		mixer->codecbuf.capacity = mixer->frames_per_block;
		len = audio_ring_bytelen(&mixer->codecbuf);
		mixer->codecbuf.mem = audio_realloc(mixer->codecbuf.mem, len);
		if (mixer->codecbuf.mem == NULL) {
			aprint_error_dev(sc->dev, "%s: malloc(%d) failed",
			    __func__, len);
			error = ENOMEM;
			goto abort;
		}
	}

	mixer->volume = 256;

	/* draincv is used only for playback */
	if (mode == AUMODE_PLAY)
		cv_init(&mixer->draincv, "audiodr");

	return 0;

abort:
	audio_mixer_destroy(sc, mixer);
	return error;
}

// ミキサを終了しリソースを解放します。
// mixer 自身のメモリは解放しません。
/*
 * audio_mixer_destroy:
 *	Release all resources of 'mixer'.
 *	Note that it does not release the memory area of 'mixer' itself.
 */
void
audio_mixer_destroy(struct audio_softc *sc, audio_trackmixer_t *mixer)
{
	int mode;

	mode = mixer->mode;
	KASSERT(mode == AUMODE_PLAY || mode == AUMODE_RECORD);

	if (mixer->hwbuf.mem != NULL) {
		if (sc->hw_if->freem) {
			sc->hw_if->freem(sc->hw_hdl, mixer->hwbuf.mem, mode);
		} else {
			kern_free(mixer->hwbuf.mem);
		}
		mixer->hwbuf.mem = NULL;
	}

	audio_free(mixer->codecbuf.mem);
	audio_free(mixer->mixsample);

	if (mode == AUMODE_PLAY)
		cv_destroy(&mixer->draincv);
}

// 再生ミキサを起動します。
// 割り込みコンテキストから呼び出してはいけません。
/*
 * audio_pmixer_start:
 *	Starts playback mixer.
 *	It must not be called from the interrupt context.
 */
void
audio_pmixer_start(struct audio_softc *sc, bool force)
{
	audio_trackmixer_t *mixer;
	int minimum;

	KASSERT(mutex_owned(sc->sc_lock));

#if defined(_KERNEL)
	// すでに再生ミキサが起動していたら、true を返す
	if (sc->sc_pbusy)
		return;
#else
	// ユーザランドエミュレーション側では割り込みがないので
	// 毎回スタートさせて start_output を呼んでいる。
#endif

	mixer = sc->sc_pmixer;
	TRACE("begin mixseq=%d hwseq=%d hwbuf=%d/%d/%d%s",
		(int)mixer->mixseq, (int)mixer->hwseq,
		mixer->hwbuf.head, mixer->hwbuf.used, mixer->hwbuf.capacity,
		force ? " force" : "");

	// 通常は2ブロック貯めてから再生開始したい。
	// force ならそういうわけにいかないので1ブロックで再生開始する。
	minimum = (force) ? 1 : 2;
	while (mixer->hwbuf.used < mixer->frames_per_block * minimum) {
		audio_pmixer_process(sc, false);
	}

	// トラックミキサ出力開始
	/* Start output */
	mutex_enter(sc->sc_intr_lock);
	audio_pmixer_output(sc);
	mutex_exit(sc->sc_intr_lock);

	TRACE("end   mixseq=%d hwseq=%d hwbuf=%d/%d/%d",
		(int)mixer->mixseq, (int)mixer->hwseq,
		mixer->hwbuf.head, mixer->hwbuf.used, mixer->hwbuf.capacity);
}

/*
 * When playing back with MD filter:
 *
 *           track track ...
 *               v v
 *                +  mix (with aint2_t)
 *                |  master volume (with aint2_t)
 *                v
 *    mixsample [::::]                  wide-int 1 block (ring) buffer
 *                |
 *                |  convert aint2_t -> aint_t
 *                v
 *    codecbuf  [....]                  1 block (ring) buffer
 *                |
 *                |  convert to hw format
 *                v
 *    hwbuf     [............]          NBLKHW blocks ring buffer
 *
 * When playingback without MD filter:
 *
 *    mixsample [::::]                  wide-int 1 block (ring) buffer
 *                |
 *                |  convert aint2_t -> aint_t
 *                v
 *    hwbuf     [............]          NBLKHW blocks ring buffer
 *
 * mixsample: slinear_NE, wide internal precision, HW ch, HW freq.
 * codecbuf:  slinear_NE, internal precision,      HW ch, HW freq.
 * hwbuf:     HW encoding, HW precision,           HW ch, HW freq.
 */

// 全トラックを倍精度ミキシングバッファで合成し、
// 倍精度ミキシングバッファから hwbuf への変換を行います。
// (hwbuf からハードウェアへの転送はここでは行いません)
// 呼び出し時の sc_intr_lock の状態はどちらでもよく、hwbuf へのアクセスを
// sc_intr_lock でこの関数が保護します。
// intr が true なら割り込みコンテキストからの呼び出し、
// false ならプロセスコンテキストからの呼び出しを示す。
/*
 * audio_pmixer_process:
 *	Perform mixing and convert it to hwbuf.
 *	Note that this function doesn't transfer from hwbuf to HW.
 *	If 'isintr' is true, it indicates a call from the interrupt context.
 *	If false, it indicates a call from process context.
 *	It can be called with or without sc_intr_lock.
 */
void
audio_pmixer_process(struct audio_softc *sc, bool isintr)
{
	audio_trackmixer_t *mixer;
	int hw_free_count;
	int frame_count;
	int sample_count;
	int mixed;
	int lock_owned;
	int i;
	aint2_t *m;
	aint_t *h;

	mixer = sc->sc_pmixer;

	// 今回取り出すフレーム数を決定
	// 実際には hwbuf はブロック単位で変動するはずなので
	// count は1ブロック分になるはず
	hw_free_count = audio_ring_get_contig_free(&mixer->hwbuf);
	frame_count = min(hw_free_count, mixer->frames_per_block);
	if (frame_count <= 0) {
		TRACE("count too short: hw_free=%d frames_per_block=%d",
		    hw_free_count, mixer->frames_per_block);
		return;
	}
	sample_count = frame_count * mixer->mixfmt.channels;

	mixer->mixseq++;

	// 全トラックを合成
	mixed = audio_pmixer_mixall(sc, isintr);
	if (mixed == 0) {
		// 無音
		memset(mixer->mixsample, 0,
		    frametobyte(&mixer->mixfmt, frame_count));
	} else {
		aint2_t ovf_plus;
		aint2_t ovf_minus;
		int vol;

		// オーバーフロー検出
		/* Overflow detection */
		ovf_plus = AINT_T_MAX;
		ovf_minus = AINT_T_MIN;
		m = mixer->mixsample;
		for (i = 0; i < sample_count; i++) {
			aint2_t val;

			val = *m++;
			if (val > ovf_plus)
				ovf_plus = val;
			else if (val < ovf_minus)
				ovf_minus = val;
		}

		// マスタボリュームの自動制御
		/* Master Volume Auto Adjust */
		vol = mixer->volume;
		if (ovf_plus > (aint2_t)AINT_T_MAX
		 || ovf_minus < (aint2_t)AINT_T_MIN) {
			aint2_t ovf;
			int vol2;

			// TODO: AINT2_T_MIN チェック?
			ovf = ovf_plus;
			if (ovf < -ovf_minus)
				ovf = -ovf_minus;

			// オーバーフローしてたら少なくとも今回はボリュームを
			// 下げる
			/* Turn down the volume if overflow occured. */
			vol2 = (int)((aint2_t)AINT_T_MAX * 256 / ovf);
			if (vol2 < vol)
				vol = vol2;

			if (vol < mixer->volume) {
				// 128 までは自動でマスタボリュームを下げる
				// 今の値の 95% ずつに下げていってみる
				/* Turn down gradually to 128. */
				if (mixer->volume > 128) {
					mixer->volume = mixer->volume * 95 / 100;
					aprint_normal_dev(sc->dev,
					    "auto volume adjust: volume %d\n",
					    mixer->volume);
				}
			}
		}

		// マスタボリューム適用
		/* Apply Master Volume. */
		if (vol != 256) {
			m = mixer->mixsample;
			for (i = 0; i < sample_count; i++) {
#if defined(AUDIO_USE_C_IMPLEMENTATION_DEFINED_BEHAVIOR) && defined(__GNUC__)
				*m = *m * vol >> 8;
#else
				*m = *m * vol / 256;
#endif
				m++;
			}
		}
	}

	// ここから ハードウェアチャンネル

	// ハードウェアバッファへ転送
	lock_owned = mutex_tryenter(sc->sc_intr_lock);

	m = mixer->mixsample;
	// MD 側フィルタがあれば aint2_t -> aint_t を codecbuf へ
	if (mixer->codec) {
		h = audio_ring_tailptr_aint(&mixer->codecbuf);
	} else {
		h = audio_ring_tailptr_aint(&mixer->hwbuf);
	}

	for (i = 0; i < sample_count; i++) {
		*h++ = *m++;
	}

	// MD 側フィルタ
	if (mixer->codec) {
		audio_ring_push(&mixer->codecbuf, frame_count);
		mixer->codecarg.src = audio_ring_headptr(&mixer->codecbuf);
		mixer->codecarg.dst = audio_ring_tailptr(&mixer->hwbuf);
		mixer->codecarg.count = frame_count;
		mixer->codec(&mixer->codecarg);
		audio_ring_take(&mixer->codecbuf, mixer->codecarg.count);
	}

	audio_ring_push(&mixer->hwbuf, frame_count);

	TRACE("done mixseq=%d hwbuf=%d/%d/%d%s",
	    (int)mixer->mixseq,
	    mixer->hwbuf.head, mixer->hwbuf.used, mixer->hwbuf.capacity,
	    (mixed == 0) ? " silent" : "");

	if (lock_owned) {
		mutex_exit(sc->sc_intr_lock);
	}
}

// 全トラックを 1ブロック分合成します。
// 合成されたトラック数を返します。
/*
 * audio_pmixer_mixall:
 *	Mix all tracks one block.
 *	It returns the number of mixed tracks.
 */
static int
audio_pmixer_mixall(struct audio_softc *sc, bool isintr)
{
	audio_trackmixer_t *mixer;
	audio_file_t *f;
	int req;
	int mixed = 0;

	mixer = sc->sc_pmixer;

	// XXX frames_per_block そのままのほうが分かりやすいような
	req = mixer->frames_per_block;

	SLIST_FOREACH(f, &sc->sc_files, entry) {
		audio_track_t *track = f->ptrack;

		if (track == NULL)
			continue;

		// 協調的ロックされているトラックは、今回ミキシングしない。
		if (isintr && track->in_use) {
			TRACET(track, "skip; in use");
			continue;
		}

		if (track->is_pause) {
			TRACET(track, "skip; paused");
			continue;
		}

		// mmap トラックならここで入力があったことにみせかける
		if (track->mmapped) {
			// XXX push じゃなく直接操作してウィンドウを移動みたいに
			// したほうがいいんじゃないか。
			audio_ring_push(&track->usrbuf, track->usrbuf_blksize);
			TRACET(track, "mmap; usr=%d/%d/C%d",
			    track->usrbuf.head,
			    track->usrbuf.used,
			    track->usrbuf.capacity);
		}

		if (track->outputbuf.used < req && track->usrbuf.used > 0) {
			TRACET(track, "process");
			audio_track_play(track);
		}

		if (track->outputbuf.used == 0) {
			TRACET(track, "skip; empty");
			continue;
		}
		// 合成
		mixed = audio_pmixer_mix_track(mixer, track, req, mixed);
	}
	return mixed;
}

// トラックバッファから取り出し、ミキシングします。
// mixed には呼び出し時点までの合成済みトラック数を渡します。
// 戻り値はこの関数終了時での合成済みトラック数)です。
// つまりこのトラックを合成すれば mixed + 1 を返します。
/*
 * audio_pmixer_mix_track:
 *	Mix one track.
 *	'mixed' specifies the number of tracks mixed so far.
 *	It returns the number of tracks mixed.  In other words,
 *	it returns mixed+1 if this track is mixed.
 */
int
audio_pmixer_mix_track(audio_trackmixer_t *mixer, audio_track_t *track,
	int req, int mixed)
{
	int count;
	int sample_count;
	int i;
	const aint_t *s;
	aint2_t *d;

	// 現時点で outputbuf に溜まってるやつを最大1ブロック分処理する。

	// このトラックが処理済みならなにもしない
	if (mixer->mixseq < track->seq) return mixed;

	count = audio_ring_get_contig_used(&track->outputbuf);
	count = min(count, mixer->frames_per_block);

	s = audio_ring_headptr_aint(&track->outputbuf);
	d = mixer->mixsample;

	// 整数倍精度へ変換し、トラックボリュームを適用して加算合成
	sample_count = count * mixer->mixfmt.channels;
	if (mixed == 0) {
		// 最初のトラック合成は代入
		if (track->volume == 256) {
			for (i = 0; i < sample_count; i++) {
				*d++ = ((aint2_t)*s++);
			}
		} else {
			for (i = 0; i < sample_count; i++) {
#if defined(AUDIO_USE_C_IMPLEMENTATION_DEFINED_BEHAVIOR) && defined(__GNUC__)
				*d++ = ((aint2_t)*s++) * track->volume >> 8;
#else
				*d++ = ((aint2_t)*s++) * track->volume / 256;
#endif
			}
		}
	} else {
		// 2本め以降なら加算合成
		if (track->volume == 256) {
			for (i = 0; i < sample_count; i++) {
				*d++ += ((aint2_t)*s++);
			}
		} else {
			for (i = 0; i < sample_count; i++) {
#if defined(AUDIO_USE_C_IMPLEMENTATION_DEFINED_BEHAVIOR) && defined(__GNUC__)
				*d++ += ((aint2_t)*s++) * track->volume >> 8;
#else
				*d++ += ((aint2_t)*s++) * track->volume / 256;
#endif
			}
		}
	}

	// outputbuf が1ブロック未満であっても、カウンタはブロック境界に
	// いなければならないため、count ではなく frames_per_block を足す。
	audio_ring_take(&track->outputbuf, mixer->frames_per_block);

	// トラックバッファを取り込んだことを反映
	// mixseq はこの時点ではまだ前回の値なのでトラック側へは +1
	track->seq = mixer->mixseq + 1;

	// audio_write() に空きが出来たことを通知
	cv_broadcast(&track->outchan);

	TRACET(track, "broadcast; trseq=%d out=%d/%d/%d", (int)track->seq,
	    track->outputbuf.head, track->outputbuf.used, track->outputbuf.capacity);

	// usrbuf が空いたら(lowat を下回ったら) シグナルを送る
	// XXX ここで usrbuf が空いたかどうかを見るのもどうかと思うが
	if (track->usrbuf.used <= track->usrbuf_usedlow && !track->is_pause) {
		if (track->sih_wr) {
			kpreempt_disable();
			softint_schedule(track->sih_wr);
			kpreempt_enable();
		}
	}

	return mixed + 1;
}

// ハードウェアバッファから 1 ブロック出力します。
// sc_intr_lock で呼び出してください。
/*
 * audio_pmixer_output:
 *	Output one block from hwbuf to HW.
 *	It must be called with sc_intr_lock.
 */
void
audio_pmixer_output(struct audio_softc *sc)
{
	audio_trackmixer_t *mixer;
	audio_params_t params;
	void *start;
	void *end;
	int blksize;
	int error;

	mixer = sc->sc_pmixer;
	TRACE("pbusy=%d hwbuf=%d/%d/%d",
	    sc->sc_pbusy,
	    mixer->hwbuf.head, mixer->hwbuf.used, mixer->hwbuf.capacity);
	KASSERT(mixer->hwbuf.used >= mixer->frames_per_block);

	blksize = frametobyte(&mixer->hwbuf.fmt, mixer->frames_per_block);

	if (sc->hw_if->trigger_output) {
		/* trigger (at once) */
		if (!sc->sc_pbusy) {
			start = mixer->hwbuf.mem;
			end = (uint8_t *)start + audio_ring_bytelen(&mixer->hwbuf);
			// TODO: params 作る
			params = format2_to_params(&mixer->hwbuf.fmt);

			error = sc->hw_if->trigger_output(sc->hw_hdl,
			    start, end, blksize, audio_pintr, sc, &params);
			if (error) {
				DPRINTF(1, "%s trigger_output failed: %d\n",
				    __func__, error);
				return;
			}
		}
	} else {
		/* start (everytime) */
		start = audio_ring_headptr(&mixer->hwbuf);

		error = sc->hw_if->start_output(sc->hw_hdl,
		    start, blksize, audio_pintr, sc);
		if (error) {
			DPRINTF(1, "%s start_output failed: %d\n",
			    __func__, error);
			return;
		}
	}
	sc->sc_pbusy = true;
}

// 割り込みハンドラです。
// sc_intr_lock で呼び出されます。
/*
 * audio_pintr:
 *	This is an interrupt handler for playback.
 *	It is called with sc_intr_lock.
 */
void
audio_pintr(void *arg)
{
	struct audio_softc *sc;
	audio_trackmixer_t *mixer;

	sc = arg;
	KASSERT(mutex_owned(sc->sc_intr_lock));

	mixer = sc->sc_pmixer;
	mixer->hw_complete_counter += mixer->frames_per_block;
	mixer->hwseq++;

	audio_ring_take(&mixer->hwbuf, mixer->frames_per_block);

	TRACE("HW_INT ++hwseq=%d cmplcnt=%d hwbuf=%d/%d/%d",
		(int)mixer->hwseq,
		(int)mixer->hw_complete_counter,
		mixer->hwbuf.head, mixer->hwbuf.used, mixer->hwbuf.capacity);

#if !defined(_KERNEL)
	// ユーザランドエミュレーションは割り込み駆動ではないので
	// 処理はここまで。
	return;
#endif

#if defined(AUDIO_HW_DOUBLE_BUFFER)
	// ブロック0 の出力が終わったら用意されているはずのブロック1 を
	// すぐに出力しておいて、その後ブロック2 の作成に取りかかる。
	// これで遅マシンでも再生を途切れさせずに HW ブロックが作成できるが、
	// その代わりレイテンシは1ブロック分上がる。

	// まず出力待ちのシーケンスを出力
	if (mixer->hwbuf.used >= mixer->frames_per_block) {
		audio_pmixer_output(sc);
	}

	bool later = false;

	if (mixer->hwbuf.used < mixer->frames_per_block) {
		later = true;
	}

	// 次のバッファを用意する
	audio_pmixer_process(sc, true);

	if (later) {
		audio_pmixer_output(sc);
	}
#else
	// その場で次のブロックを作成して再生
	// レイテンシは下げられるが、マシンパワーがないと再生が途切れる。

	// バッファを作成
	audio_pmixer_process(sc, true);

	// 出力
	audio_pmixer_output(sc);
#endif

	// drain 待ちしている人のために通知
	cv_broadcast(&mixer->draincv);
}

// 録音ミキサを起動します。
// 割り込みコンテキストから呼び出してはいけません。
/*
 * audio_rmixer_start:
 *	Starts record mixer.
 *	It must not be called from the interrupt context.
 */
void
audio_rmixer_start(struct audio_softc *sc)
{
	KASSERT(mutex_owned(sc->sc_lock));

	// すでに再生ミキサが起動していたら、true を返す
	if (sc->sc_rbusy)
		return;

	TRACE("begin");

	mutex_enter(sc->sc_intr_lock);
	audio_rmixer_input(sc);
	mutex_exit(sc->sc_intr_lock);
}

/*
 * When recording with MD filter:
 *
 *    hwbuf     [............]          NBLKHW blocks ring buffer
 *                |
 *                | convert from hw format
 *                v
 *    codecbuf  [....]                  1 block (ring) buffer
 *               |  |
 *               v  v
 *            track track ...
 *
 * When recording without MD filter:
 *
 *    hwbuf     [............]          NBLKHW blocks ring buffer
 *               |  |
 *               v  v
 *            track track ...
 *
 * hwbuf:     HW encoding, HW precision, HW ch, HW freq.
 * codecbuf:  slinear_NE, internal precision, HW ch, HW freq.
 */

// 録音できた hwbuf のブロックを全録音トラックへ分配します。
/*
 * audio_rmixer_process:
 *	Distribute a recorded block to all recoding tracks.
 */
void
audio_rmixer_process(struct audio_softc *sc)
{
	audio_trackmixer_t *mixer;
	audio_ring_t *mixersrc;
	audio_file_t *f;
	int count;
	int bytes;

	mixer = sc->sc_rmixer;

	// 今回取り出すフレーム数を決定
	// 実際には hwbuf はブロック単位で変動するはずなので
	// count は1ブロック分になるはず
	count = audio_ring_get_contig_used(&mixer->hwbuf);
	count = min(count, mixer->frames_per_block);
	if (count <= 0) {
		TRACE("count %d: too short", count);
		return;
	}
	bytes = frametobyte(&mixer->track_fmt, count);

	// MD 側フィルタ
	if (mixer->codec) {
		mixer->codecarg.src = audio_ring_headptr(&mixer->hwbuf);
		mixer->codecarg.dst = audio_ring_tailptr(&mixer->codecbuf);
		mixer->codecarg.count = count;
		mixer->codec(&mixer->codecarg);
		audio_ring_take(&mixer->hwbuf, mixer->codecarg.count);
		audio_ring_push(&mixer->codecbuf, mixer->codecarg.count);
		mixersrc = &mixer->codecbuf;
	} else {
		mixersrc = &mixer->hwbuf;
	}

	// 全トラックへ分配
	SLIST_FOREACH(f, &sc->sc_files, entry) {
		audio_track_t *track = f->rtrack;
		audio_ring_t *input;

		if (track == NULL)
			continue;

		if (track->in_use) {
			TRACET(track, "skip; in use");
			continue;
		}

		if (track->is_pause) {
			TRACET(track, "skip; paused");
			continue;
		}

		// 空いてなければ古い方から捨てる?
		input = track->input;
		if (input->capacity - input->used < mixer->frames_per_block) {
			int drops = mixer->frames_per_block -
			    (input->capacity - input->used);
			track->dropframes += drops;
			TRACET(track, "drop %d frames: inp=%d/%d/%d",
			    drops,
			    input->head, input->used, input->capacity);
			audio_ring_take(input, drops);
		}
		KASSERT(input->used % mixer->frames_per_block == 0);

		memcpy(audio_ring_tailptr_aint(input),
		    audio_ring_headptr_aint(mixersrc),
		    bytes);
		audio_ring_push(input, count);

		// XXX シーケンスいるんだっけ

		// audio_read() にブロックが来たことを通知
		cv_broadcast(&track->outchan);

		TRACET(track, "broadcast; inp=%d/%d/%d",
		    input->head, input->used, input->capacity);
	}

	audio_ring_take(mixersrc, count);

	// SIGIO を通知(する必要があるかどうかは向こうで判断する)
	softint_schedule(sc->sc_sih_rd);
}

// ハードウェアバッファに1ブロック入力を開始します。
// sc_intr_lock で呼び出してください。
/*
 * audio_rmixer_input:
 *	Input one block from HW to hwbuf.
 *	It must be called with sc_intr_lock.
 */
static void
audio_rmixer_input(struct audio_softc *sc)
{
	audio_trackmixer_t *mixer;
	audio_params_t params;
	void *start;
	void *end;
	int blksize;
	int error;

	mixer = sc->sc_rmixer;
	blksize = frametobyte(&mixer->hwbuf.fmt, mixer->frames_per_block);

	if (sc->hw_if->trigger_input) {
		/* trigger (at once) */
		if (!sc->sc_rbusy) {
			start = mixer->hwbuf.mem;
			end = (uint8_t *)start + audio_ring_bytelen(&mixer->hwbuf);
			// TODO: params 作る
			params = format2_to_params(&mixer->hwbuf.fmt);

			error = sc->hw_if->trigger_input(sc->hw_hdl,
			    start, end, blksize, audio_rintr, sc, &params);
			if (error) {
				DPRINTF(1, "%s trigger_input failed: %d\n",
				    __func__, error);
				return;
			}
		}
	} else {
		/* start (everytime) */
		start = audio_ring_tailptr(&mixer->hwbuf);

		error = sc->hw_if->start_input(sc->hw_hdl,
		    start, blksize, audio_rintr, sc);
		if (error) {
			DPRINTF(1, "%s start_input failed: %d\n",
			    __func__, error);
			return;
		}
	}
	sc->sc_rbusy = true;
}

// 割り込みハンドラです。
// sc_intr_lock で呼び出されます。
/*
 * audio_rintr:
 *	This is an interrupt handler for recording.
 *	It is called with sc_intr_lock.
 */
void
audio_rintr(void *arg)
{
	struct audio_softc *sc;
	audio_trackmixer_t *mixer;

	sc = arg;
	KASSERT(mutex_owned(sc->sc_intr_lock));

	mixer = sc->sc_rmixer;
	mixer->hw_complete_counter += mixer->frames_per_block;
	mixer->hwseq++;

	audio_ring_push(&mixer->hwbuf, mixer->frames_per_block);

	TRACE("HW_INT ++hwseq=%d cmplcnt=%d hwbuf=%d/%d/%d",
		(int)mixer->hwseq,
		(int)mixer->hw_complete_counter,
		mixer->hwbuf.head, mixer->hwbuf.used, mixer->hwbuf.capacity);

	// このバッファを分配する
	audio_rmixer_process(sc);

	// 次のバッファを要求
	audio_rmixer_input(sc);
}

// 再生ミキサを停止します。
// 関連するパラメータもクリアするため、基本的には halt_output を
// 直接呼び出すのではなく、こちらを呼んでください。
/*
 * audio_pmixer_halt:
 *	Halt playback mixer.
 *	This function also clears related parameters, so call this function
 *	instead of calling halt_output directly.
 */
int
audio_pmixer_halt(struct audio_softc *sc)
{
	int error;

	TRACE("");
	KASSERT(mutex_owned(sc->sc_lock));
	KASSERT(mutex_owned(sc->sc_intr_lock));

	error = sc->hw_if->halt_output(sc->hw_hdl);
	// エラーが起きても停止は停止する
	sc->sc_pbusy = false;
	sc->sc_pmixer->hwbuf.head = 0;
	sc->sc_pmixer->hwbuf.used = 0;
	sc->sc_pmixer->mixseq = 0;
	sc->sc_pmixer->hwseq = 0;

	return error;
}

// 録音ミキサを停止します。
// 関連するパラメータもクリアするため、基本的には halt_input を
// 直接呼び出すのではなく、こちらを呼んでください。
/*
 * audio_rmixer_halt:
 *	Halt recording mixer.
 *	This function also clears related parameters, so call this function
 *	instead of calling halt_input directly.
 */
int
audio_rmixer_halt(struct audio_softc *sc)
{
	int error;

	TRACE("");
	KASSERT(mutex_owned(sc->sc_lock));
	KASSERT(mutex_owned(sc->sc_intr_lock));

	error = sc->hw_if->halt_input(sc->hw_hdl);
	// エラーが起きても停止は停止する
	sc->sc_rbusy = false;
	sc->sc_rmixer->hwbuf.head = 0;
	sc->sc_rmixer->hwbuf.used = 0;
	sc->sc_rmixer->mixseq = 0;
	sc->sc_rmixer->hwseq = 0;

	return error;
}

// トラックをフラッシュします。
// 現在の動作を停止し、すべてのキューとバッファをクリアし、
// エラーカウンタをリセットします。
// これでええんかなあ。
void
audio_track_clear(struct audio_softc *sc, audio_track_t *track)
{

	KASSERT(track);
	TRACET(track, "clear");

	KASSERT(mutex_owned(sc->sc_lock));

	track->usrbuf.used = 0;
	// 内部情報も全部クリア
	if (track->codec.filter) {
		track->codec.srcbuf.used = 0;
		track->codec.srcbuf.head = 0;
	}
	if (track->chvol.filter) {
		track->chvol.srcbuf.used = 0;
		track->chvol.srcbuf.head = 0;
	}
	if (track->chmix.filter) {
		track->chmix.srcbuf.used = 0;
		track->chmix.srcbuf.head = 0;
	}
	if (track->freq.filter) {
		track->freq.srcbuf.used = 0;
		track->freq.srcbuf.head = 0;
		if (track->freq_step < 65536)
			track->freq_current = 65536;
		else
			track->freq_current = 0;
		memset(track->freq_prev, 0, sizeof(track->freq_prev));
		memset(track->freq_curr, 0, sizeof(track->freq_curr));
	}
	// バッファをクリアすれば動作は自然と停止する
	mutex_enter(sc->sc_intr_lock);
	track->outputbuf.used = 0;
	mutex_exit(sc->sc_intr_lock);

	// カウンタクリア
	track->dropframes = 0;
	track->playdrop = 0;
}

// errno を返します。
/*
 * audio_track_drain:
 *	Drain the track.
 *	If successful, it returns 0.  Otherwise returns errno.
 */
int
audio_track_drain(audio_track_t *track)
{
	audio_trackmixer_t *mixer;
	struct audio_softc *sc;
	int error;

	KASSERT(track);
	TRACET(track, "start");
	mixer = track->mixer;
	sc = mixer->sc;
	KASSERT(mutex_owned(sc->sc_lock));

	// pause 中なら今溜まってるものは全部無視してこのまま終わってよし
	if (track->is_pause) {
		TRACET(track, "pause -> clear");
		track->pstate = AUDIO_STATE_CLEAR;
	}
	// トラックにデータがなくても drain は ミキサのループが数回回って
	// 問題なく終わるが、クリーンならさすがに早期終了しても
	// いいんじゃなかろうか。
	if (track->pstate == AUDIO_STATE_CLEAR) {
		TRACET(track, "no need to drain");
		return 0;
	}
	track->pstate = AUDIO_STATE_DRAINING;

	if (sc->sc_pbusy == false) {
		// トラックバッファが空になっても、ミキサ側で処理中のデータが
		// あるかもしれない。
		// トラックミキサが動作していないときは、動作させる。
		audio_pmixer_start(sc, true);
	}

	for (;;) {
		// 終了条件判定の前に表示したい
		TRACET(track, "trkseq=%d hwseq=%d out=%d/%d/%d",
		    (int)track->seq, (int)mixer->hwseq,
		    track->outputbuf.head, track->outputbuf.used,
		    track->outputbuf.capacity);

		if (track->outputbuf.used == 0 && track->seq <= mixer->hwseq)
			break;

		error = cv_wait_sig(&mixer->draincv, sc->sc_lock);
		if (error) {
			TRACET(track, "cv_wait_sig failed %d", error);
			return error;
		}
		if (sc->sc_dying)
			return EIO;
	}

	track->pstate = AUDIO_STATE_CLEAR;
	TRACET(track, "done trk_inp=%d trk_out=%d",
		(int)track->inputcounter, (int)track->outputcounter);
	return 0;
}

// track の usrbuf に tail から len バイトを uiomove します。
// リングバッファの折り返しはしません。
static inline int
audio_write_uiomove(audio_track_t *track, int tail, int len, struct uio *uio)
{
	audio_ring_t *usrbuf;
	int error;

	usrbuf = &track->usrbuf;
	error = uiomove((uint8_t *)usrbuf->mem + tail, len, uio);
	if (error) {
		TRACET(track, "uiomove(len=%d) failed: %d", len, error);
		return error;
	}
	audio_ring_push(usrbuf, len);
	track->useriobytes += len;
	TRACET(track, "uiomove(len=%d) usrbuf=%d/%d/C%d",
	    len,
	    usrbuf->head, usrbuf->used, usrbuf->capacity);
	return 0;
}

int
audio_write(struct audio_softc *sc, struct uio *uio, int ioflag,
	audio_file_t *file)
{
	audio_track_t *track;
	audio_ring_t *usrbuf;
	int error;
	int framesize;
	int count;

	track = file->ptrack;
	KASSERT(track);
	TRACET(track, "begin");

	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->hw_if == NULL)
		return ENXIO;

	// N8 までは EINVAL だったがこっちのほうがよかろう
	if (track->mmapped)
		return EPERM;

	if (uio->uio_resid == 0) {
		track->eofcounter++;
		return 0;
	}

#ifdef AUDIO_PM_IDLE
	if (device_is_active(&sc->dev) || sc->sc_idle)
		device_active(&sc->dev, DVA_SYSTEM);
#endif

	// out_thres は usrbuf から読み出す際の閾値。
	// trkbuf.used が out_thres より大きければ変換処理を行う。
	// o PLAY なら常に変換処理をしたいので 1[フレーム] に設定
	// o PLAY_ALL なら1ブロック溜まってから処理なので block size を設定
	//
	// force は 1ブロックに満たない場合にミキサを開始するか否か。
	// o PLAY なら1ブロック未満でも常に開始するため true
	// o PLAY_ALL なら1ブロック貯まるまで開始しないので false
	usrbuf = &track->usrbuf;
	int out_thres;
	if ((track->mode & AUMODE_PLAY_ALL) != 0) {
		/* PLAY_ALL */
		out_thres = frametobyte(&track->inputfmt,
		    frame_per_block_roundup(track->mixer, &track->inputfmt));
	} else {
		/* PLAY */
		out_thres = frametobyte(&track->inputfmt, 1);
	}
	TRACET(track, "resid=%zd out_thres=%d", uio->uio_resid, out_thres);

	track->pstate = AUDIO_STATE_RUNNING;
	error = 0;
	while (uio->uio_resid > 0 && error == 0) {
		int bytes;

		TRACET(track, "while resid=%zd usrbuf=%d/%d/H%d",
		    uio->uio_resid,
		    usrbuf->head, usrbuf->used, track->usrbuf_usedhigh);

		// XXX 実際には待つ前に drop を計算してしまったほうがいいはず

		// usrbuf が一杯ならここで待つ
		while (usrbuf->used >= track->usrbuf_usedhigh) {
			if ((ioflag & IO_NDELAY)) {
				error = EWOULDBLOCK;
				goto abort;
			}
			TRACET(track, "sleep usrbuf=%d/H%d",
			    usrbuf->used, track->usrbuf_usedhigh);
			error = audio_waitio(sc, track);
			if (error)
				goto abort;
		}

		/* Write to the conversion stage as much as possible. */
		// usrbuf にコピー
		bytes = min(track->usrbuf_usedhigh - usrbuf->used,
		    uio->uio_resid);
		int tail = audio_ring_tail(usrbuf);
		if (tail + bytes <= usrbuf->capacity) {
			error = audio_write_uiomove(track, tail, bytes, uio);
			if (error)
				break;
		} else {
			int bytes1;
			int bytes2;

			bytes1 = usrbuf->capacity - tail;
			error = audio_write_uiomove(track, tail, bytes1, uio);
			if (error)
				break;

			bytes2 = bytes - bytes1;
			error = audio_write_uiomove(track, 0, bytes2, uio);
			if (error)
				break;
		}

		// 前回落としたフレーム分を回復運転のため取り去る
		// write されるバイトバッファがフレーム境界に揃っている
		// 保証はないので、usrbuf にコピーした後でなければならない
		if ((track->mode & AUMODE_PLAY_ALL) == 0 &&
		    track->playdrop != 0) {
			framesize = frametobyte(&track->inputfmt, 1);
			count = min(usrbuf->used / framesize, track->playdrop);
			audio_ring_take(usrbuf, count * framesize);
			track->playdrop -= count * framesize;
			TRACET(track, "drop %d -> usr=%d/%d/H%d",
			    count * framesize,
			    usrbuf->head, usrbuf->used, track->usrbuf_usedhigh);
		}

		mutex_enter(sc->sc_intr_lock);
		if (track->usrbuf.used >= out_thres &&
		    track->outputbuf.used < track->mixer->frames_per_block) {
			track->in_use = true;
			mutex_exit(sc->sc_intr_lock);
			audio_track_play(track);
			mutex_enter(sc->sc_intr_lock);
			track->in_use = false;
		}
		mutex_exit(sc->sc_intr_lock);

		if (!track->is_pause) {
			bool force = ((track->mode & AUMODE_PLAY_ALL) == 0);
			audio_pmixer_start(sc, force);
		}
	}

abort:
	TRACET(track, "done error=%d", error);
	return error;
}

// track の usrbuf の head から len バイトを uiomove します。
// リングバッファの折り返しはしません。
static inline int
audio_read_uiomove(audio_track_t *track, int head, int len, struct uio *uio)
{
	audio_ring_t *usrbuf;
	int error;

	usrbuf = &track->usrbuf;
	error = uiomove((uint8_t *)usrbuf->mem + head, len, uio);
	if (error) {
		TRACET(track, "uiomove(len=%d) failed: %d", len, error);
		return error;
	}
	audio_ring_take(usrbuf, len);
	track->useriobytes += len;
	TRACET(track, "uiomove(len=%d) usrbuf=%d/%d/C%d",
	    len,
	    usrbuf->head, usrbuf->used, usrbuf->capacity);
	return 0;
}

int
audio_read(struct audio_softc *sc, struct uio *uio, int ioflag,
	audio_file_t *file)
{
	audio_track_t *track;
	int error;

	track = file->rtrack;
	KASSERT(track);
	TRACET(track, "resid=%u", (int)uio->uio_resid);

	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->hw_if == NULL)
		return ENXIO;

	// N8 までは EINVAL だったがこっちのほうがよかろう
	if (track->mmapped)
		return EPERM;

#ifdef AUDIO_PM_IDLE
	if (device_is_active(&sc->dev) || sc->sc_idle)
		device_active(&sc->dev, DVA_SYSTEM);
#endif

	// 自分が O_RDWR でオープンしてあれば read() を発行することは出来るが、
	// HW が half-duplex で自分か他の誰かによって play が選択されていると
	// この read() はエラー。
	if ((file->mode & AUMODE_RECORD) == 0) {
		return EBADF;
	}

	TRACET(track, "resid=%zd", uio->uio_resid);

	error = 0;
	while (uio->uio_resid > 0 && error == 0) {
		audio_ring_t *usrbuf;
		int bytes;

		usrbuf = &track->usrbuf;

		TRACET(track, "while resid=%zd input=%d/%d/%d usrbuf=%d/%d/H%d",
		    uio->uio_resid,
		    track->input->head, track->input->used, track->input->capacity,
		    usrbuf->head, usrbuf->used, track->usrbuf_usedhigh);

		mutex_enter(sc->sc_intr_lock);
		if (track->input->used == 0 && track->usrbuf.used == 0) {
			// バッファが空ならここで待機
			mutex_exit(sc->sc_intr_lock);

			audio_rmixer_start(sc);
			if (ioflag & IO_NDELAY)
				return EWOULDBLOCK;

			error = audio_waitio(sc, track);
			if (error)
				return error;
			continue;
		}

		track->in_use = true;
		mutex_exit(sc->sc_intr_lock);
		audio_track_record(track);
		mutex_enter(sc->sc_intr_lock);
		track->in_use = false;
		mutex_exit(sc->sc_intr_lock);

		bytes = min(usrbuf->used, uio->uio_resid);
		int head = usrbuf->head;
		if (head + bytes <= usrbuf->capacity) {
			error = audio_read_uiomove(track, head, bytes, uio);
			if (error)
				break;
		} else {
			int bytes1;
			int bytes2;

			bytes1 = usrbuf->capacity - head;
			error = audio_read_uiomove(track, head, bytes1, uio);
			if (error)
				break;

			bytes2 = bytes - bytes1;
			error = audio_read_uiomove(track, 0, bytes2, uio);
			if (error)
				break;
		}
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

	TRACET(track, "wait");
	/* Wait for pending I/O to complete. */
	error = cv_wait_sig(&track->outchan, sc->sc_lock);
	if (sc->sc_dying)
		error = EIO;
	TRACET(track, "error=%d", error);
	return error;
}
