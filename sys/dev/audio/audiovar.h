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
#define TRACE0(fmt, ...)	audio_trace0(__func__, fmt, ## __VA_ARGS__)
#define TRACE(t, fmt, ...)	audio_trace(__func__, t, fmt, ## __VA_ARGS__)
#else
#define TRACE0(fmt, ...)	/**/
#define TRACE(t, fmt, ...)	/**/
#endif

// 出力バッファのブロック数
#define NBLKOUT	(4)

// softintr を使うとき ON にしてください
//#define AUDIO_SOFTINTR

// 周波数変換実装
//#define FREQ_ORIG	// 元の実装
#define FREQ_CYCLE2	// 周波数ではなく65536を基準にした周期比にする

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

#if BYTE_ORDER == LITTLE_ENDIAN
#define AUDIO_ENCODING_SLINEAR_HE	AUDIO_ENCODING_SLINEAR_LE
#else
#define AUDIO_ENCODING_SLINEAR_HE	AUDIO_ENCODING_SLINEAR_BE
#endif


/* 前方参照 */
typedef struct audio_ring audio_ring_t;
typedef struct audio_track audio_track_t;
typedef struct audio_trackmixer audio_trackmixer_t;
typedef struct audio_file audio_file_t;
typedef struct audio_convert_arg audio_convert_arg_t;

#if defined(FREQ_ORIG)
/*
* 簡易帯分数表現
*/
typedef struct audio_rational {
	int i;
	int n;
} audio_rational_t;
#endif

// リングバッファ
struct audio_ring
{
	audio_format2_t fmt;	/* フォーマット */
	int  capacity;			/* 容量(フレーム) */
	int  top;				/* 先頭フレーム位置 */
	int  count;				/* 有効フレーム量 */
	void *sample;			/* サンプル */
};

struct audio_stage
{
	audio_filter_t filter;
	audio_filter_arg_t arg;
	audio_ring_t *dst;
	audio_ring_t srcbuf;
};
typedef struct audio_stage audio_stage_t;

struct audio_track
{
	// このトラックの再生/録音モード。AUMODE_*
	// 録音トラックなら AUMODE_RECORD。
	// 再生トラックなら AUMODE_PLAY は必ず立っている。
	// 再生トラックで PLAY モードなら AUMODE_PLAY のみ。
	// 再生トラックで PLAY_ALL モードなら AUMODE_PLAY | AUMODE_PLAY_ALL。
	// file->mode は録再トラックの mode を OR したものと一致しているはず。
	int mode;

	audio_ring_t        usrbuf;			// ユーザ入出力バッファ

	audio_format2_t     inputfmt;		// このトラックに入力するフォーマット
	audio_ring_t       *input;			// このトラックに入力するとき使用するバッファへのポインタ

	audio_ring_t        outputbuf;		/* トラックの出力バッファ */
	kcondvar_t          outchan;		// 出力バッファが空いたことの通知用

	audio_stage_t       codec;			// エンコーディング変換ステージ
	audio_stage_t       chvol;			// チャンネルボリュームステージ
	audio_stage_t       chmix;			// チャンネルミックスステージ
	audio_stage_t       freq;			// 周波数変換ステージ

#if defined(FREQ_CYCLE2)
	unsigned int        freq_step;		/* 周波数変換用、周期比 */
	unsigned int        freq_current;	/* 周波数変換用、現在のカウンタ */
	unsigned int        freq_leap;		/* 周波数変換用、補正値 */
#elif defined(FREQ_ORIG)
	audio_rational_t   freq_step;		/* 周波数変換用分数 (変換元周波数 / 変換先周波数) */
	audio_rational_t   freq_current;	/* 周波数変換用 現在のカウンタ */
	int32_t            freq_coef;		// 周波数変換用係数
#else
#error unknown FREQ_*
#endif

	uint16_t  ch_volume[AUDIO_MAX_CHANNELS];	/* チャンネルバランス用 チャンネルボリューム */
	uint               volume;			/* トラックボリューム。トラックボリュームはトラックミキサで処理。 */

	audio_trackmixer_t  *mixer;			/* 接続されているトラックミキサ */

	uint64_t seq;	// トラックミキサが引き取ったシーケンス番号

	bool is_draining;					/* drain 実行中 */
	bool is_pause;						/* outputbuf への trackmixer からのアクセスを一時停止する */

	uint64_t inputcounter;				/* トラックに入力されたフレーム数 */
	uint64_t outputcounter;				/* トラックから出力されたフレーム数 */

#if !defined(AUDIO_SOFTINTR)
	volatile uint32_t track_cl;			// track cooperative lock
#endif

	/* できるかどうか未知 */
	uint64_t track_mixer_counter;		/* outputbuf のトラックミキサ側読み書きフレーム数 */
	uint64_t mixer_hw_counter; /* mixer <-> hw 入出力フレーム数 */
	uint64_t hw_complete_counter; /* ハードウェア I/O が完了したフレーム数 */

	/* デバッグ用 */
	int id;								/* トラック識別子 */
};

struct audio_trackmixer
{
	int mode;							/* AUMODE_PLAY or AUMODE_RECORD */
	audio_format2_t track_fmt;			/* track <-> trackmixer フォーマット */

	int frames_per_block;				/* 内部周波数での 1 ブロックのフレーム数 */

	uint           volume;				/* 出力マスタボリューム */

	audio_format2_t mixfmt;				// PLAY 合成用整数倍精度フォーマット
	void *mixsample;					// PLAY 合成用整数倍精度バッファ

	audio_filter_t  codec;				// MD が要求する追加のコーデック
	audio_filter_arg_t codecarg;		// その引数
	audio_ring_t    codecbuf;			// コーデック用バッファ。ストライド変換の吸収

	audio_ring_t   hwbuf;				/* 物理デバイスの入出力バッファ (malloc ではなく allocm で確保する) */
	int hwblks;							// hwbuf のブロック数
	kcondvar_t     intrcv;				/* 割り込みを通知する? */

	uint64_t mixseq;	// ミキシング中のシーケンス番号
	uint64_t hwseq;		// ハードウェア出力完了したシーケンス番号

	struct audio_softc *sc;				/* 論理デバイス */

	int blktime_n; // ブロックの秒の分子 初期値は AUDIO_BLK_MS
	int blktime_d; // ブロックの秒の分母 初期値は 1000

#if defined(AUDIO_SOFTINTR)
	kmutex_t softintrlock;				// softintr mutex
	void *softintr;						// softintr cookie
#endif
										// 未定

	uint64_t hw_output_counter;			/* ハードウェアへの出力フレーム数 */
	uint64_t hw_complete_counter;		/* ハードウェアが出力完了したフレーム数 */
};

struct audio_file
{
	struct audio_softc *sc;			/* 論理デバイス */

	// ptrack, rtrack はトラックが無効なら NULL。
	// mode に AUMODE_{PLAY,RECORD} が立っていても該当側の ptrack, rtrack
	// が NULL という状態はありえる。例えば、上位から PLAY を指定されたが
	// 何らかの理由で ptrack が有効に出来なかった、あるいはクローズに
	// 向かっている最中ですでにこのトラックが解放された、とか。
	audio_track_t   *ptrack;		/* 再生トラック */
	audio_track_t   *rtrack;		/* 録音トラック */

	// この file の再生/録音モード。AUMODE_* (PLAY_ALL も含む)
	// ptrack.mode は (mode & (AUMODE_PLAY | AUMODE_PLAY_ALL)) と、
	// rtrack.mode は (mode & AUMODE_RECORD) と等しいはず。
	int mode;
#if defined(_KERNEL)
	dev_t dev;						/* デバイスファイルへのバックリンク */
#endif

	SLIST_ENTRY(audio_file) entry;
};


extern void audio_trace0(const char *funcname, const char *fmt, ...)
	__attribute__((__format__(printf, 2, 3)));
extern void audio_trace(const char *funcname, audio_track_t *track,
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
