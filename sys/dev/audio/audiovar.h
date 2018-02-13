// vi:set ts=8:
#pragma once

// デバッグレベルは
// 1: open/close/set_param等
// 2: read/write/ioctlシステムコールくらいまでは含む
// 3: TRACEも含む
#define AUDIO_DEBUG	3

#if defined(_KERNEL)
#include <dev/audio/aufilter.h>
#else
#include <stdint.h>
#include <stdbool.h>
#include "queue.h"
#include "compat.h"
#include "userland.h"
#include "uio.h"
#include "aufilter.h"
#endif // _KERNEL

#if AUDIO_DEBUG > 2
#define TRACE(fmt, ...)		audio_trace(__func__, fmt, ## __VA_ARGS__)
#define TRACET(t, fmt, ...)	audio_tracet(__func__, t, fmt, ## __VA_ARGS__)
#define TRACEF(f, fmt, ...)	audio_tracef(__func__, f, fmt, ## __VA_ARGS__)
#else
#define TRACE(fmt, ...)		/**/
#define TRACET(t, fmt, ...)	/**/
#define TRACEF(f, fmt, ...)	/**/
#endif

// 出力バッファのブロック数
/* Number of output buffer's blocks.  Must be != NBLKHW */
#define NBLKOUT	(4)

// ハードウェアバッファのブロック数
/* Number of HW buffer's blocks. */
#define NBLKHW (3)

// ユーザバッファの最小ブロック数
/* Minimum number of usrbuf's blocks. */
#define AUMINNOBLK	(3)

// ユーザランドフォーマットとして [US]Linear24/24 をサポートします。
//#define AUDIO_SUPPORT_LINEAR24

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

// ミキサでダブルバッファを使うかどうか。
// on にするとレイテンシが1ブロック分増える代わりに HW への再生が途切れる
// ことはなくなる。
// off にすると1ブロックのレイテンシは減るがマシンパワーがないと HW 再生が
// 途切れる。(速ければへーきへーきという可能性もある)
#define AUDIO_HW_DOUBLE_BUFFER

// C の実装定義動作を使用する。
#define AUDIO_USE_C_IMPLEMENTATION_DEFINED_BEHAVIOR

// サポートする最大/最小周波数。
// 最小は、実用的に意味があるかはともかく 4kHz 未満をセットできる骨董品も
// 中にはあることを考慮してこのくらいまでは許してやろう。
// ちなみに OpenBSD は下限 4000Hz で現状妥当な数字だと思う。
// 最大は 384000 (hdaudio規格?) でもいいけど確認できないので、とりあえず
// このくらいでたちまち勘弁してもらえないか。
#define AUDIO_MIN_FREQUENCY (1000)
#define AUDIO_MAX_FREQUENCY (192000)

#if BYTE_ORDER == LITTLE_ENDIAN
#define AUDIO_ENCODING_SLINEAR_NE AUDIO_ENCODING_SLINEAR_LE
#define AUDIO_ENCODING_ULINEAR_NE AUDIO_ENCODING_ULINEAR_LE
#else
#define AUDIO_ENCODING_SLINEAR_NE AUDIO_ENCODING_SLINEAR_BE
#define AUDIO_ENCODING_ULINEAR_NE AUDIO_ENCODING_ULINEAR_BE
#endif


// 前方参照
typedef struct audio_track audio_track_t;
typedef struct audio_trackmixer audio_trackmixer_t;
typedef struct audio_file audio_file_t;

/* ring buffer */
typedef struct {
	audio_format2_t fmt;	/* format */
	int  capacity;		/* capacity by frame */
	int  top;		/* top frame position */
	int  count;		/* available frame count */
	void *mem;		/* sample ptr */
} audio_ring_t;

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

struct audio_track {
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

	audio_ring_t	outputbuf;	/* track output buffer */
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
};

struct audio_trackmixer {
	int		mode;		/* AUMODE_PLAY or AUMODE_RECORD */
	audio_format2_t	track_fmt;	/* track <-> trackmixer format */

	int		frames_per_block; /* number of frames in 1 block */

	u_int		volume;		/* output SW master volume (0.256) */

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

struct audio_file {
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
};


extern void audio_trace(const char *funcname, const char *fmt, ...)
	__attribute__((__format__(printf, 2, 3)));
extern void audio_tracet(const char *funcname, audio_track_t *track,
	const char *fmt, ...)
	__attribute__((__format__(printf, 3, 4)));
extern void audio_tracef(const char *funcname, audio_file_t *file,
	const char *fmt, ...)
	__attribute__((__format__(printf, 3, 4)));

static inline struct audio_params
format2_to_params(const audio_format2_t *f2)
{
	audio_params_t p;

	p.sample_rate = f2->sample_rate;
	p.channels = f2->channels;
	p.encoding = f2->encoding;
	p.validbits = f2->precision;
	p.precision = f2->stride;
	return p;
}

static inline audio_format2_t
params_to_format2(const struct audio_params *p)
{
	audio_format2_t f2;

	f2.sample_rate = p->sample_rate;
	f2.channels    = p->channels;
	f2.encoding    = p->encoding;
	f2.precision   = p->validbits;
	f2.stride      = p->precision;
	return f2;
}

// ユーザランドで使用される 0..255 ボリュームを、トラック内の内部表現である
// 0..256 ボリュームに変換します。
static inline u_int
audio_volume_to_inner(u_int v)
{
	return v < 127 ? v : v + 1;
}

// トラック内の内部表現である 0..256 ボリュームを、外部で使用される 0..255
// ボリュームに変換します。
static inline u_int
audio_volume_to_outer(u_int v)
{
	return v < 127 ? v : v - 1;
}
