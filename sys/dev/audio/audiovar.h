// vi:set ts=8:

#ifndef _SYS_DEV_AUDIOVAR2_H_
#define _SYS_DEV_AUDIOVAR2_H_

#if defined(_KERNEL)
#include <sys/condvar.h>
#include <sys/proc.h>
#include <sys/queue.h>

#include <dev/audio_if.h>
#include <dev/auconv.h>

#include <dev/audio/aufilter.h>
#else
#include <stdint.h>
#include <stdbool.h>
#include "compat.h"
#include "userland.h"
#include "aufilter.h"
#endif // _KERNEL

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

// デバッグ用なんちゃってメモリログ。
#define AUDIO_DEBUG_MLOG

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
#define AUDIO_ENCODING_SLINEAR_OE AUDIO_ENCODING_SLINEAR_BE
#define AUDIO_ENCODING_ULINEAR_OE AUDIO_ENCODING_ULINEAR_BE
#else
#define AUDIO_ENCODING_SLINEAR_NE AUDIO_ENCODING_SLINEAR_BE
#define AUDIO_ENCODING_ULINEAR_NE AUDIO_ENCODING_ULINEAR_BE
#define AUDIO_ENCODING_SLINEAR_OE AUDIO_ENCODING_SLINEAR_LE
#define AUDIO_ENCODING_ULINEAR_OE AUDIO_ENCODING_ULINEAR_LE
#endif

#define AUOPEN_READ	0x01
#define AUOPEN_WRITE	0x02

/* Interfaces for audiobell. */
int audiobellopen(dev_t, int, int, struct lwp *, struct file **);
int audiobellclose(struct file *);
int audiobellwrite(struct file *, off_t *, struct uio *, kauth_cred_t, int);
int audiobellioctl(struct file *, u_long, void *);

// 前方参照
typedef struct audio_track audio_track_t;
typedef struct audio_trackmixer audio_trackmixer_t;
typedef struct audio_file audio_file_t;

/* ring buffer */
typedef struct {
	audio_format2_t fmt;	/* format */
	int  capacity;		/* capacity by frame */
	int  head;		/* head position in frame */
	int  used;		/* used frame count */
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

#define AUDIO_N_PORTS 4

struct au_mixer_ports {
	int	index;		/* index of port-selector mixerctl */
	int	master;		/* index of master mixerctl */
	int	nports;		/* number of selectable ports */
	bool	isenum;		/* selector is enum type */
	u_int	allports;	/* all aumasks or'd */
	u_int	aumask[AUDIO_N_PORTS];	/* exposed value of "ports" */
	int	misel [AUDIO_N_PORTS];	/* ord of port, for selector */
	int	miport[AUDIO_N_PORTS];	/* index of port's mixerctl */
	bool	isdual;		/* has working mixerout */
	int	mixerout;	/* ord of mixerout, for dual case */
	int	cur_port;	/* the port that gain actually controls when
				   mixerout is selected, for dual case */
};

struct audio_softc {
	device_t	dev;

	// NULL なら sc はあるが autoconfig に失敗したので無効になってる
	// audio1 at ... attached
	// audio1: ... config failed
	// みたいな状態
	const struct audio_hw_if *hw_if; /* Hardware interface */
	void		*hw_hdl;	/* Hardware driver handle */
	device_t	sc_dev;		/* Hardware device struct */

	SLIST_HEAD(, audio_file) sc_files;	/* list of open descriptor */

	audio_trackmixer_t *sc_pmixer;	/* null if play not supported by hw */
	audio_trackmixer_t *sc_rmixer;	/* null if rec not supported by hw */

	audio_filter_reg_t sc_xxx_pfilreg;
	audio_filter_reg_t sc_xxx_rfilreg;

	bool		sc_can_playback;	/* device can playback */
	bool		sc_can_capture;		/* device can capture */

	int sc_popens;
	int sc_ropens;
	bool			sc_pbusy;	/* output DMA in progress */
	bool			sc_rbusy;	/* input DMA in progress */

	// この4つが /dev/sound で引き継がれる non-volatile パラメータ
	audio_format2_t sc_pparams;	/* play encoding parameters */
	audio_format2_t sc_rparams;	/* record encoding parameters */
	bool 		sc_ppause;
	bool		sc_rpause;

	struct audio_info sc_ai;	/* recent info for /dev/sound */

	struct	selinfo sc_wsel; /* write selector */
	struct	selinfo sc_rsel; /* read selector */
	void		*sc_sih_rd;
	struct	mixer_asyncs {
		struct mixer_asyncs *next;
		pid_t	pid;
	} *sc_async_mixer;  /* processes who want mixer SIGIO */

	/* Locks and sleep channels for reading, writing and draining. */
	kmutex_t	*sc_intr_lock;
	kmutex_t	*sc_lock;
	bool		sc_dying;

	kauth_cred_t sc_cred;

	struct sysctllog *sc_log;

	mixer_ctrl_t	*sc_mixer_state;
	int		sc_nmixer_states;
	int		sc_static_nmixer_states;
	struct au_mixer_ports sc_inports;
	struct au_mixer_ports sc_outports;
	int		sc_monitor_port;
	u_int	sc_lastgain;
};

extern void audio_vtrace(const char *funcname, const char *header,
	const char *fmt, va_list ap);
extern void audio_trace(const char *funcname, const char *fmt, ...)
	__attribute__((__format__(printf, 2, 3)));
extern void audio_tracet(const char *funcname, audio_track_t *track,
	const char *fmt, ...)
	__attribute__((__format__(printf, 3, 4)));
extern void audio_tracef(const char *funcname, audio_file_t *file,
	const char *fmt, ...)
	__attribute__((__format__(printf, 3, 4)));

int audio_track_init(struct audio_softc *sc, audio_track_t **track, int mode);
void audio_track_destroy(audio_track_t *track);
int audio_track_set_format(audio_track_t *track, audio_format2_t *track_fmt);
void audio_track_play(audio_track_t *track);
int audio_track_drain(struct audio_softc *, audio_track_t *track);
void audio_track_record(audio_track_t *track);
void audio_track_clear(struct audio_softc *sc, audio_track_t *track);

int audio_mixer_init(struct audio_softc *sc, int mode, const audio_format2_t *);
void audio_mixer_destroy(struct audio_softc *sc, audio_trackmixer_t *mixer);
void audio_pmixer_start(struct audio_softc *sc, bool force);
void audio_pmixer_process(struct audio_softc *sc, bool isintr);
int  audio_pmixer_mix_track(audio_trackmixer_t *mixer, audio_track_t *track, int req, int mixed);
void audio_rmixer_start(struct audio_softc *sc);
void audio_rmixer_process(struct audio_softc *sc);
void audio_pintr(void *arg);
void audio_rintr(void *arg);

int  audio_pmixer_halt(struct audio_softc *sc);
int  audio_rmixer_halt(struct audio_softc *sc);

/* glue layer */
int audio_write(struct audio_softc *sc, struct uio *uio, int ioflag, audio_file_t *file); /* write の MI 側 */
int audio_read(struct audio_softc *sc, struct uio *uio, int ioflag,
	audio_file_t *file);

#if defined(AUDIO_DEBUG_MLOG)
void audio_mlog_init(void);
void audio_mlog_free(void);
void audio_mlog_flush(void);
#else
#define audio_mlog_flush()	/**/
#endif

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

// not used anymore
// どうするかね
#if 0
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
#endif // 0

#if defined(_KERNEL)
#include <dev/audio/auring.h>
#else
#include "auring.h"
#endif

#endif /* _SYS_DEV_AUDIOVAR2_H_ */
