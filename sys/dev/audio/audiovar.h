#pragma once

#include <stdint.h>
#include "queue.h"
#include "compat.h"

/* アサートするとき定義 */
#define AUDIO_ASSERT

#ifdef AUDIO_ASSERT
#define KASSERT(expr)	do {\
	if (!(expr)) panic(#expr);\
} while (0)
#else
#define KASSERT(expr)	/**/
#endif

/* 内部フォーマットのビット数 */
#define AUDIO_INTERNAL_BITS		16
//#define AUDIO_INTERNAL_BITS		32

#if AUDIO_INTERNAL_BITS == 16

typedef int16_t internal_t;
typedef uint16_t uinternal_t;
typedef int32_t internal2_t;
typedef uint32_t uinternal2_t;
#define AUDIO_INTERNAL_T_MAX	0x7fff
#define AUDIO_INTERNAL_T_MIN	0x8000

#elif AUDIO_INTERNAL_BITS == 32

typedef int32_t internal_t;
typedef uint32_t uinternal_t;
typedef int64_t internal2_t;
typedef uint64_t uinternal2_t;
#define AUDIO_INTERNAL_T_MAX	0x7fffffff
#define AUDIO_INTERNAL_T_MIN	0x80000000

#else
#error Invalid AUDIO_INTERNAL_BITS
#endif


#define AUDIO_PLAY 0x1
#define AUDIO_REC  0x2

#define AUDIO_ENCODING_MULAW		0
#define AUDIO_ENCODING_SLINEAR_LE	1
#define AUDIO_ENCODING_SLINEAR_BE	2
#define AUDIO_ENCODING_ULINEAR_LE	3
#define AUDIO_ENCODING_ULINEAR_BE	4
#define AUDIO_ENCODING_MSM6258		6258
#define AUDIO_ENCODING_RAWBYTE		32767

#if BYTE_ORDER == LITTLE_ENDIAN
#define AUDIO_ENCODING_SLINEAR_HE	AUDIO_ENCODING_SLINEAR_LE
#else
#define AUDIO_ENCODING_SLINEAR_HE	AUDIO_ENCODING_SLINEAR_BE
#endif

/* 1 ブロックの時間サイズ 40ms */
/* 40ms の場合は (1/40ms)=25=5^2 なので 100 の倍数の周波数のほか、15.625kHz でもフレーム数が整数になる */
#define AUDIO_BLOCK_msec 40

/* サポートする最大のチャンネル数 */
#define AUDIO_MAX_CH	18


/* 前方参照 */
typedef struct audio_format audio_format_t;
typedef struct audio_ring audio_ring_t;
typedef struct audio_lane audio_lane_t;
typedef struct audio_lanemixer audio_lanemixer_t;
typedef struct audio_file audio_file_t;
typedef struct audio_softc audio_softc_t;
typedef struct audio_convert_arg audio_convert_arg_t;
typedef struct audio_codec audio_codec_t;

/*
* 簡易帯分数表現
*/
typedef struct audio_rational {
	int i;
	int n;
} audio_rational_t;

/* フォーマット */
struct audio_format
{
	int32_t  encoding;		/* AUDIO_ENCODING */
	uint32_t frequency;		/* Hz */
	uint8_t  channels;		/* 1.. */
	uint8_t  precision;		/* ex.24 (valid bits of sample, must precision <= stride) */
	uint8_t  stride;		/* ex.32 (packing bits of sample) */
};

struct audio_ring
{
	audio_format_t *fmt;	/* フォーマット */
	int  capacity;			/* 容量(フレーム) */
	int  top;				/* 先頭フレーム位置 */
	int  count;				/* 有効フレーム量 */
	void *sample;			/* サンプル */
};

typedef struct audio_filter_arg audio_filter_arg_t;
struct audio_filter_arg
{
	const void *src;
	audio_format_t *src_fmt;

	void *dst;
	audio_format_t *dst_fmt;

	int count;		// 今回のフィルタ呼び出しで入出力可能なフレーム数

	void *context;
};

/*
戻り値には、今回のフィルタの実行で出力したフレーム数を返してください。
*/
typedef int(*audio_filter_t)(audio_filter_arg_t *arg);

struct audio_lane
{
	audio_format_t     userio_fmt;		/* ユーザランドとのやり取りで使用するフォーマット */
	audio_ring_t       userio_buf;		/* ユーザランド側とのやり取りで使用するデータ。
										sample にメモリはアロケートしないでポインタ操作で使用する。 */
										/* XXX: おそらく mmap のときは、アロケートしてそれを公開する */

	int                userio_frames_of_block;	/* ユーザランド周波数での 1 ブロックのフレーム数 */

	uint8_t subframe_buf[AUDIO_MAX_CH * 4];	/* フレーム未満のデータ用を次回の read,write まで保持しておくバイトバッファ */
	int subframe_buf_used;					/* subframe_buf の使用バイト数 */

	uint16_t ch_volume[AUDIO_MAX_CH];	/* チャンネルバランス用 チャンネルボリューム */
	uint16_t           volume;			/* レーンボリューム */

	bool channelmix_all;

	uint8_t enconvert_mode;
#define AUDIO_LANE_ENCONVERT_THRU		0x00
#define AUDIO_LANE_ENCONVERT_INLINE		0x01
#define AUDIO_LANE_ENCONVERT_BUFFER		0x02
	uint8_t chmix_mode;
#define AUDIO_LANE_CHANNEL_THRU			0x00
#define AUDIO_LANE_CHANNEL_INLINE		0x01
#define AUDIO_LANE_CHANNEL_BUFFER		0x02

#define AUDIO_LANE_CHANNEL_SHRINK		0x00
#define AUDIO_LANE_CHANNEL_EXPAND		0x10
#define AUDIO_LANE_CHANNEL_MIXLR		0x20
#define AUDIO_LANE_CHANNEL_MIXALL		0x30
#define AUDIO_LANE_CHANNEL_DUPLR		0x40
#define AUDIO_LANE_CHANNEL_DUPALL		0x50
#define AUDIO_LANE_CHANNEL_MIX_MASK		0x70

#define AUDIO_LANE_CHANNEL_VOLUME		0x80

	uint8_t freq_mode;
#define AUDIO_LANE_FREQ_THRU			0x00
#define AUDIO_LANE_FREQ_INLINE			0x01
#define AUDIO_LANE_FREQ_BUFFER			0x02
	uint8_t volume_mode;
#define AUDIO_LANE_VOLUME_THRU			0x00
#define AUDIO_LANE_VOLUME_INLINE		0x01

	audio_filter_t     codec;			/* userio <-> lane コーデックフィルタ */
	audio_filter_arg_t codec_arg;		/* とその引数 */

	audio_format_t     enconvert_fmt;
	audio_ring_t       enconvert_buf;	/* エンコーディング変換バッファ userio 周波数、userio チャンネル、他は内部フォーマット */
	audio_ring_t       *step1;			/* エンコーディング変換出力 userio_buf か enconvert_buf を指す */
	void               *enconvert_mem;	/* エンコーディング変換のバッファメモリ */

	audio_format_t     chmix_fmt;
	audio_ring_t       chmix_buf;		/* チャンネル変換用バッファ userio 周波数、他は内部フォーマット */
	audio_ring_t       *step2;			/* チャンネル変換出力 *step1 か chmix_buf を指す */
	void               *chmix_mem;

	audio_ring_t       *freq_tmp;		/* インライン周波数変換時の一時バッファ。NULL か step2 を指す */

	audio_rational_t   freq_step;		/* 周波数変換用分数 (変換元周波数 / 変換先周波数) */
	audio_rational_t   freq_current;	/* 周波数変換用 現在のカウンタ */

	audio_ring_t       lane_buf;		/* レーンミキサとのバッファ */

	audio_lanemixer_t  *mixer;			/* 接続されているレーンミキサ */
	int                mixed_count;		/* レーンミキサのミキサバッファにあって出力を待っているこのレーンのフレーム数 */
	int                hw_count;		/* レーンミキサのハードウェアバッファにあって出力完了を待っているこのレーンのフレーム数 */

	bool is_draining;					/* drain 実行中 */
	bool is_pause;						/* lane_buf への lanemixer からのアクセスを一時停止する */


	/* できるかどうか未知 */
	uint64_t userio_counter;			/* userio_buf の読み書きフレーム数 */
	uint64_t lane_counter;				/* lane_buf のレーン側読み書きフレーム数 */

	uint64_t lane_mixer_counter;		/* lane_buf のレーンミキサ側読み書きフレーム数 */
	uint64_t mixer_hw_counter; /* mixer <-> hw 入出力フレーム数 */
	uint64_t hw_complete_counter; /* ハードウェア I/O が完了したフレーム数 */
};

struct audio_lanemixer
{
	audio_format_t lane_fmt;			/* ミキサのレーン側入出力フォーマット */
										/* precision == stride は保証 */

	int frames_of_block;				/* 内部周波数での 1 ブロックのフレーム数 */

	uint16_t       volume;				/* 出力マスタボリューム */

	audio_format_t mix_fmt;
	audio_ring_t   mix_buf;				/* 整数倍精度ミキシングバッファ */

	audio_filter_t  codec;				/* mix <-> hw コーデックフィルタ */
	audio_filter_arg_t codec_arg;		/* その引数 */

	audio_format_t hw_fmt;
	audio_ring_t   hw_buf;				/* 物理デバイスの入出力バッファ (malloc ではなく allocm で確保する) */
	int  hw_count;						/* 物理デバイス入出力中のフレーム数 */

	audio_softc_t  *sc;					/* 論理デバイス */

										// 未定
	int pending_play_period;
};

struct audio_file
{
	audio_softc_t  *sc;				/* 論理デバイス */
	audio_lane_t   lane_play;		/* 再生レーン */
	audio_lane_t   lane_rec;		/* 録音レーン */

	SLIST_ENTRY(audio_file) entry;
};

/* Userland から見えるデバイス */
struct audio_softc
{
	SLIST_HEAD(files_head, audio_file) files;		/* 開いているファイルのリスト */
	audio_lanemixer_t  mixer_play;		/* 接続されている再生ミキサ */
	audio_lanemixer_t  mixer_rec;		/* 接続されている録音ミキサ */

	void *phys; // 実物理デバイス
};

extern const char *fmt_tostring(audio_format_t *);
extern int debug;
