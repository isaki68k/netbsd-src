#pragma once

#if defined(_KERNEL)
#else
#include <stdint.h>
#include <stdbool.h>
#include "queue.h"
#include "compat.h"
#include "uio.h"

/* アサートするとき定義 */
#define AUDIO_ASSERT

#ifdef AUDIO_ASSERT
#define KASSERT(expr)	do {\
	if (!(expr)) panic(#expr);\
} while (0)
#else
#define KASSERT(expr)	/**/
#endif
#endif // _KERNEL

/* 内部フォーマットのビット数 */
#define AUDIO_INTERNAL_BITS		16
//#define AUDIO_INTERNAL_BITS		32

#if AUDIO_INTERNAL_BITS == 16

typedef int16_t internal_t;
typedef uint16_t uinternal_t;
typedef int32_t internal2_t;
typedef uint32_t uinternal2_t;
#define AUDIO_INTERNAL_T_MAX	((internal_t)0x7fff)
#define AUDIO_INTERNAL_T_MIN	((internal_t)0x8000)

#elif AUDIO_INTERNAL_BITS == 32

typedef int32_t internal_t;
typedef uint32_t uinternal_t;
typedef int64_t internal2_t;
typedef uint64_t uinternal2_t;
#define AUDIO_INTERNAL_T_MAX	((internal_t)0x7fffffff)
#define AUDIO_INTERNAL_T_MIN	((internal_t)0x80000000)

#else
#error Invalid AUDIO_INTERNAL_BITS
#endif

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

/* 1 ブロックの時間サイズ 40ms */
/* 40ms の場合は (1/40ms)=25=5^2 なので 100 の倍数の周波数のほか、15.625kHz でもフレーム数が整数になる */
#if defined(_KERNEL)
#define AUDIO_BLK_MS 40
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
typedef struct audio_format2 audio_format2_t;
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

/* フォーマット */
struct audio_format2
{
	int32_t  encoding;		/* AUDIO_ENCODING */
	uint32_t sample_rate;	/* Hz */
	uint8_t  channels;		/* 1.. */
	uint8_t  precision;		/* ex.24 (valid bits of sample, must precision <= stride) */
	uint8_t  stride;		/* ex.32 (packing bits of sample) */
};

struct audio_ring
{
	audio_format2_t fmt;	/* フォーマット */
	int  capacity;			/* 容量(フレーム) */
	int  top;				/* 先頭フレーム位置 */
	int  count;				/* 有効フレーム量 */
	void *sample;			/* サンプル */
};

typedef struct audio_filter_arg audio_filter_arg_t;
struct audio_filter_arg
{
	const void *src;
	audio_format2_t *srcfmt;

	void *dst;
	audio_format2_t *dstfmt;

	int count;		// 今回のフィルタ呼び出しで入出力可能なフレーム数

	void *context;
};

/*
*/
typedef void(*audio_filter_t)(audio_filter_arg_t *arg);

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

	uint16_t  ch_volume[AUDIO_MAX_CHANNELS];	/* チャンネルバランス用 チャンネルボリューム */
	uint16_t           volume;			/* トラックボリューム。トラックボリュームはトラックミキサで処理。 */

	audio_trackmixer_t  *mixer;			/* 接続されているトラックミキサ */

	uint64_t seq;	// トラックミキサが引き取ったシーケンス番号

	bool is_draining;					/* drain 実行中 */
	bool is_pause;						/* outputbuf への trackmixer からのアクセスを一時停止する */

	uint64_t inputcounter;				/* トラックに入力されたフレーム数 */
	uint64_t outputcounter;				/* トラックから出力されたフレーム数 */

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

	audio_ring_t   mixbuf;				/* 整数倍精度ミキシングバッファ */

	audio_filter_t  codec;				/* mix <-> hw コーデックフィルタ */
	audio_filter_arg_t codec_arg;		/* その引数 */

	audio_ring_t   hwbuf;				/* 物理デバイスの入出力バッファ (malloc ではなく allocm で確保する) */

	uint64_t mixseq;	// ミキシング中のシーケンス番号
	uint64_t hwseq;		// ハードウェア出力完了したシーケンス番号

	struct audio_softc *sc;				/* 論理デバイス */

	bool busy;
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

#if !defined(_KERNEL)
/* Userland から見えるデバイス */
struct audio_softc
{
	SLIST_HEAD(files_head, audio_file) sc_files;		/* 開いているファイルのリスト */
	audio_trackmixer_t  *sc_pmixer;		/* 接続されている再生ミキサ */
	audio_trackmixer_t  *sc_rmixer;		/* 接続されている録音ミキサ */

	kcondvar_t *sc_wchan;
	kcondvar_t *sc_rchan;

	void *phys; // 実物理デバイス
	audio_trackmixer_t pmixer0;
	audio_trackmixer_t rmixer0;
};
#endif // _KERNEL

extern const char *fmt_tostring(const audio_format2_t *);
extern int debug;
