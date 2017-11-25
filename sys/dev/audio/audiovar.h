#pragma once

#if defined(_KERNEL)
#include <dev/audio/aufilter.h>
#else
#include <stdint.h>
#include <stdbool.h>
#include "queue.h"
#include "compat.h"
#include "uio.h"
#include "aufilter.h"

/* アサートするとき定義 */
#define AUDIO_ASSERT

#ifdef AUDIO_ASSERT
#define KASSERT(expr)	do {\
	if (!(expr)) panic(#expr);\
} while (0)
#define KASSERTMSG(expr, fmt, ...)	do {\
	if (!(expr)) panic(#expr);\
} while (0)
#else
#define KASSERT(expr)	/**/
#endif
#endif // _KERNEL

// 出力バッファのブロック数
#define NBLKOUT	(16)

#if defined(_KERNEL)
#define AUDIO_ENCODING_MULAW		AUDIO_ENCODING_ULAW
#define AUDIO_ENCODING_MSM6258		AUDIO_ENCODING_ADPCM
#else
#define AUMODE_PLAY		(0x01)
#define AUMODE_RECORD	(0x02)

#define AUDIO_ENCODING_MULAW		0
#define AUDIO_ENCODING_SLINEAR_LE	1
#define AUDIO_ENCODING_SLINEAR_BE	2
#define AUDIO_ENCODING_ULINEAR_LE	3
#define AUDIO_ENCODING_ULINEAR_BE	4
#define AUDIO_ENCODING_MSM6258		6258
#define AUDIO_ENCODING_RAWBYTE		32767

/* サポートする最大のチャンネル数 */
#define AUDIO_MAX_CHANNELS	18
#endif // _KERNEL

// softintr を使うとき ON にしてください
//#define AUDIO_SOFTINTR

/* 1 ブロックの時間サイズ 40ms */
/* 40ms の場合は (1/40ms)=25=5^2 なので 100 の倍数の周波数のほか、15.625kHz でもフレーム数が整数になる */
#if defined(_KERNEL)
// XXX とりあえず
// x68k では 40 (-> 80msec) ではアンダーランが発生するので
// 80 (-> 160msec) に増やしておく。どうするかはまた。
#if defined(x68k)
#define AUDIO_BLK_MS 160
#else
#define AUDIO_BLK_MS 40
#endif
#else
// XXX: エミュレーション出来ないので 400 にしておく。
#define AUDIO_BLK_MS 400
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

/*
* 簡易帯分数表現
*/
typedef struct audio_rational {
	int i;
	int n;
} audio_rational_t;

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
	int mode;								/* AUMODE_PLAY or AUMODE_RECORD */
	int                subframe_buf_used;	/* 1フレーム未満の使用バイト数 */

	audio_format2_t     inputfmt;		// このトラックに入力するフォーマット
	audio_ring_t       *input;			// このトラックに入力するとき使用するバッファへのポインタ

	audio_ring_t        outputbuf;		/* トラックの出力バッファ */
	kcondvar_t          outchan;		// 出力バッファが空いたことの通知用

	audio_stage_t       codec;			// エンコーディング変換ステージ
	audio_stage_t       chvol;			// チャンネルボリュームステージ
	audio_stage_t       chmix;			// チャンネルミックスステージ
	audio_stage_t       freq;			// 周波数変換ステージ

	audio_rational_t   freq_step;		/* 周波数変換用分数 (変換元周波数 / 変換先周波数) */
	audio_rational_t   freq_current;	/* 周波数変換用 現在のカウンタ */
	int32_t            freq_coef;		// 周波数変換用係数

	uint16_t  ch_volume[AUDIO_MAX_CHANNELS];	/* チャンネルバランス用 チャンネルボリューム */
	uint16_t           volume;			/* トラックボリューム。トラックボリュームはトラックミキサで処理。 */

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

	uint16_t       volume;				/* 出力マスタボリューム */

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
	audio_track_t   ptrack;			/* 再生トラック */
	audio_track_t   rtrack;			/* 録音トラック */

	int mode;						/* AUMODE_* (incl. AUMODE_PLAY_ALL) */
#if defined(_KERNEL)
	dev_t dev;						/* デバイスファイルへのバックリンク */
#endif

	SLIST_ENTRY(audio_file) entry;
};


extern const char *fmt_tostring(const audio_format2_t *);
extern int debug;

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
