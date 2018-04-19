#ifndef _SYS_DEV_AUDIO_AUDIODEF_H_
#define _SYS_DEV_AUDIO_AUDIODEF_H_

// audiodef.h: audio.c からのみ include されるヘッダ
// ヘッダである意味はないような気もするけど、とりあえず

// 出力バッファのブロック数
/* Number of output buffer's blocks.  Must be != NBLKHW */
#define NBLKOUT	(4)

// ハードウェアバッファのブロック数
/* Number of HW buffer's blocks. */
#define NBLKHW (3)

// ユーザバッファの最小ブロック数
/* Minimum number of usrbuf's blocks. */
#define AUMINNOBLK	(3)

// 1 ブロックの時間 [msec]
// 40ms の場合は (1/40ms) = 25 = 5^2 なので 100 の倍数の周波数のほか、
// 15.625kHz でもフレーム数が整数になるので、40 を基本にする。
#if !defined(AUDIO_BLK_MS)
#if defined(x68k)
// x68k では 40msec だと長い曲でアンダーランするので伸ばしておく
#define AUDIO_BLK_MS 320
#else
#define AUDIO_BLK_MS 40
#endif
#endif

// ミキサをシングルバッファにするかどうか。
// オン(シングルバッファ)にすると、レイテンシは1ブロック減らせるがマシン
// パワーがないと HW 再生が途切れる(かもしれない、速ければへーきへーき?)。
// オフ(ダブるバッファ)にすると、レイテンシは1ブロック増えるが HW 再生が
// 途切れることはなくなる。
//#define AUDIO_HW_SINGLE_BUFFER

// C の実装定義動作を使用する。
#define AUDIO_USE_C_IMPLEMENTATION_DEFINED_BEHAVIOR

/* conversion stage */
typedef struct {
	audio_filter_t filter;
	audio_filter_arg_t arg;
	audio_ring_t *dst;
	audio_ring_t srcbuf;
} audio_stage_t;

typedef enum {
	AUDIO_STATE_CLEAR,	/* no data, no need to drain */
	AUDIO_STATE_RUNNING,	/* need to drain */
	AUDIO_STATE_DRAINING,	/* now draining */
} audio_state_t;

typedef struct audio_track {
	// このトラックの再生/録音モード。AUMODE_*
	// 録音トラックなら AUMODE_RECORD。
	// 再生トラックなら AUMODE_PLAY は必ず立っている。
	// 再生トラックで PLAY モードなら AUMODE_PLAY のみ。
	// 再生トラックで PLAY_ALL モードなら AUMODE_PLAY | AUMODE_PLAY_ALL。
	// file->mode は録再トラックの mode を OR したものと一致しているはず。
	int mode;

	audio_ring_t	usrbuf;		/* user i/o buffer */
	u_int		usrbuf_blksize;	/* usrbuf block size in bytes */
	struct uvm_object *uobj;
	bool		mmapped;	/* device is mmap()-ed */
	u_int		usrbuf_stamp;	/* transferred bytes from/to stage */
	u_int		usrbuf_stamp_last; /* last stamp */
	u_int		usrbuf_usedhigh;/* high water mark in bytes */
	u_int		usrbuf_usedlow;	/* low water mark in bytes */

	audio_format2_t	inputfmt;	/* track input format. */
					/* userfmt for play, mixerfmt for rec */
	audio_ring_t	*input;		/* ptr to input stage buffer */

	audio_ring_t	outbuf;		/* track output buffer */
	kcondvar_t	outchan;	// I/O ready になったことの通知用

	audio_stage_t	codec;		/* encoding conversion stage */
	audio_stage_t	chvol;		/* channel volume stage */
	audio_stage_t	chmix;		/* channel mix stage */
	audio_stage_t	freq;		/* frequency conversion stage */

	u_int		freq_step;	// 周波数変換用、周期比
	u_int		freq_current;	// 周波数変換用、現在のカウンタ
	u_int		freq_leap;	// 周波数変換用、補正値
	aint_t		freq_prev[AUDIO_MAX_CHANNELS];	// 前回値
	aint_t		freq_curr[AUDIO_MAX_CHANNELS];	// 直近値

	uint16_t ch_volume[AUDIO_MAX_CHANNELS];	/* channel volume(0..256) */
	u_int		volume;		/* track volume (0..256) */

	audio_trackmixer_t *mixer;	/* connected track mixer */

	// トラックミキサが引き取ったシーケンス番号
	uint64_t	seq;		/* seq# picked up by track mixer */

	audio_state_t	pstate;		/* playback state */
	int		playdrop;	/* current drop frames */
	bool		is_pause;

	void		*sih_wr;	/* softint cookie for write */

	uint64_t	inputcounter;	/* トラックに入力されたフレーム数 */
	uint64_t	outputcounter;	/* トラックから出力されたフレーム数 */
	uint64_t	useriobytes;	// ユーザランド入出力されたバイト数
	uint64_t	dropframes;	/* # of dropped frames */
	int		eofcounter;	/* # of zero sized write */

	// プロセスコンテキストが track を使用中なら true。
	volatile bool	in_use;		/* track cooperative lock */

	int		id;		/* track id for debug */
} audio_track_t;

typedef struct audio_file {
	struct audio_softc *sc;

	// ptrack, rtrack はトラックが無効なら NULL。
	// mode に AUMODE_{PLAY,RECORD} が立っていても該当側の ptrack, rtrack
	// が NULL という状態はありえる。例えば、上位から PLAY を指定されたが
	// 何らかの理由で ptrack が有効に出来なかった、あるいはクローズに
	// 向かっている最中ですでにこのトラックが解放された、とか。
	audio_track_t	*ptrack;	/* play track (if available) */
	audio_track_t	*rtrack;	/* record track (if available) */

	// この file の再生/録音モード。AUMODE_* (PLAY_ALL も含む)
	// ptrack.mode は (mode & (AUMODE_PLAY | AUMODE_PLAY_ALL)) と、
	// rtrack.mode は (mode & AUMODE_RECORD) と等しいはず。
	int		mode;
	dev_t		dev;		// デバイスファイルへのバックリンク

	pid_t		async_audio;	/* process who wants audio SIGIO */

	SLIST_ENTRY(audio_file) entry;
} audio_file_t;

struct audio_trackmixer {
	int		mode;		/* AUMODE_PLAY or AUMODE_RECORD */
	audio_format2_t	track_fmt;	/* track <-> trackmixer format */

	int		frames_per_block; /* number of frames in 1 block */

	u_int		volume;		/* output SW master volume (0..256) */

	audio_format2_t	mixfmt;
	void		*mixsample;	/* wide-int-sized mixing buf */

	audio_filter_t	codec;		/* MD codec */
	audio_filter_arg_t codecarg;	/* and its argument */
	audio_ring_t	codecbuf;	/* also used for wide->int conversion */

	audio_ring_t	hwbuf;		/* HW I/O buf */
	int		hwblks;		/* number of blocks in hwbuf */
	kcondvar_t	draincv;	// drain 用に割り込みを通知する?

	uint64_t	mixseq;		/* seq# currently being mixed */
	uint64_t	hwseq;		/* seq# HW output completed */
				// ハードウェア出力完了したシーケンス番号

	struct audio_softc *sc;

	/* initial blktime n/d = AUDIO_BLK_MS / 1000 */
	int		blktime_n;	/* blk time numerator */
	int		blktime_d;	/* blk time denominator */

	// 以下未定

	// ハードウェアが出力完了したフレーム数
	uint64_t	hw_complete_counter;
};

/*
 * Audio Ring Buffer.
 */

#ifdef DIAGNOSTIC
#define DIAGNOSTIC_ring(ring)	audio_diagnostic_ring(__func__, (ring))
extern void audio_diagnostic_ring(const char *, const audio_ring_t *);
#else
#define DIAGNOSTIC_ring(ring)
#endif

static inline int
frametobyte(const audio_format2_t *fmt, int frames)
{
	return frames * fmt->channels * fmt->stride / NBBY;
}

// 周波数が fmt(.sample_rate) で表されるエンコーディングの
// 1ブロックのフレーム数を返します。
static inline int
frame_per_block(const audio_trackmixer_t *mixer, const audio_format2_t *fmt)
{
	return (fmt->sample_rate * mixer->blktime_n + mixer->blktime_d - 1) /
	    mixer->blktime_d;
}

// idx をラウンディングします。
// 加算方向で、加算量が ring->capacity 以下のケースのみサポートします。
/*
 * Round idx.  idx must be non-negative and less than 2 * capacity.
 * This is a private function.
 */
static inline int
auring_round(const audio_ring_t *ring, int idx)
{
	DIAGNOSTIC_ring(ring);
	KASSERT(idx >= 0);
	KASSERT(idx < ring->capacity * 2);

	if (idx < ring->capacity) {
		return idx;
	} else {
		return idx - ring->capacity;
	}
}

// ring の tail 位置(head+used位置) を返します。
// この位置は、最終有効フレームの次のフレーム位置に相当します。
/*
 * Return ring's tail (= head + used) position.
 */
static inline int
auring_tail(const audio_ring_t *ring)
{
	return auring_round(ring, ring->head + ring->used);
}

// ring の head フレームのポインタを求めます。
/*
 * Return ring's head pointer.
 * This function can be used only if the stride of the 'ring' is equal to
 * the internal stride.  Don't use this for hw buffer.
 */
static inline aint_t *
auring_headptr_aint(const audio_ring_t *ring)
{
	KASSERT(ring->fmt.stride == sizeof(aint_t) * NBBY);

	return (aint_t *)ring->mem + ring->head * ring->fmt.channels;
}

// ring の tail (= head + used、すなわち、最終有効フレームの次) フレームの
// ポインタを求めます。
// hwbuf のポインタはこちらではなく RING_BOT_UINT8() で取得してください。
/*
 * Return ring's tail (= head + used) pointer.
 * This function can be used only if the stride of the 'ring' is equal to
 * the internal stride.  Don't use this for hw buffer.
 */
static inline aint_t *
auring_tailptr_aint(const audio_ring_t *ring)
{
	KASSERT(ring->fmt.stride == sizeof(aint_t) * NBBY);

	return (aint_t *)ring->mem + auring_tail(ring) * ring->fmt.channels;
}

// ring の head フレームのポインタを求めます。
/*
 * Return ring's head pointer.
 * This function can be used even if the stride of the 'ring' is equal to
 * or not equal to the internal stride.
 */
static inline uint8_t *
auring_headptr(const audio_ring_t *ring)
{
	return (uint8_t *)ring->mem +
	    ring->head * ring->fmt.channels * ring->fmt.stride / NBBY;
}

// ring の tail (= head + used、すなわち、最終有効フレームの次) フレームの
// ポインタを求めます。HWbuf は 4bit/sample の可能性があるため RING_BOT() では
// なく必ずこちらを使用してください。
/*
 * Return ring's tail pointer.
 * This function can be used even if the stride of the 'ring' is equal to
 * or not equal to the internal stride.
 */
static inline uint8_t *
auring_tailptr(audio_ring_t *ring)
{
	return (uint8_t *)ring->mem +
	    auring_tail(ring) * ring->fmt.channels * ring->fmt.stride / NBBY;
}

// キャパシティをバイト単位で求めます。
/*
 * Return ring's capacity in bytes.
 */
static inline int
auring_bytelen(const audio_ring_t *ring)
{
	return frametobyte(&ring->fmt, ring->capacity);
}

// ring->head から n 個取り出したことにします。
/*
 * Take out n frames from head of ring.
 * This function only manipurates counters.  It doesn't manipurate any
 * actual buffer data.
 */
static inline void
auring_take(audio_ring_t *ring, int n)
{
	DIAGNOSTIC_ring(ring);
	KASSERTMSG(n >= 0, "%s: n=%d", __func__, n);
	KASSERTMSG(ring->used >= n, "%s: ring->used=%d n=%d",
	    __func__, ring->used, n);

	ring->head = auring_round(ring, ring->head + n);
	ring->used -= n;
}

// ring tail に n 個付け足したことにします。
/*
 * Append n frames into tail of ring.
 * This function only manipurates counters.  It doesn't manipurate any
 * actual buffer data.
 */
static inline void
auring_push(audio_ring_t *ring, int n)
{
	DIAGNOSTIC_ring(ring);
	KASSERT(n >= 0);
	KASSERTMSG(ring->used + n <= ring->capacity,
		"%s: ring->used=%d n=%d ring->capacity=%d",
		__func__, ring->used, n, ring->capacity);

	ring->used += n;
}

// ring->head の位置からの有効フレームにアクセスしようとするとき、
// ラウンディングせずにアクセス出来る個数を返します。
/*
 * Return the number of contiguous frames in used.
 */
static inline int
auring_get_contig_used(const audio_ring_t *ring)
{
	DIAGNOSTIC_ring(ring);

	if (ring->head + ring->used <= ring->capacity) {
		return ring->used;
	} else {
		return ring->capacity - ring->head;
	}
}

// auring_tail の位置から空きフレームにアクセスしようとするとき、
// ラウンディングせずにアクセス出来る、空きフレームの個数を返します。
/*
 * Return the number of contiguous free frames.
 */
static inline int
auring_get_contig_free(const audio_ring_t *ring)
{
	DIAGNOSTIC_ring(ring);

	// ring の折り返し終端まで使用されているときは、
	// 開始位置はラウンディング後なので < が条件
	if (ring->head + ring->used < ring->capacity) {
		return ring->capacity - (ring->head + ring->used);
	} else {
		return ring->capacity - ring->used;
	}
}

#endif /* !_SYS_DEV_AUDIO_AUDIODEF_H_ */
