#pragma once

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

/* フォーマット */
typedef struct audio_format2 audio_format2_t;
struct audio_format2
{
	int32_t  encoding;		/* AUDIO_ENCODING */
	uint32_t sample_rate;	/* Hz */
	uint8_t  channels;		/* 1.. */
	uint8_t  precision;		/* ex.24 (valid bits of sample, must precision <= stride) */
	uint8_t  stride;		/* ex.32 (packing bits of sample) */
};

// フィルタに渡されるパラメータ一式です。
typedef struct audio_filter_arg audio_filter_arg_t;
struct audio_filter_arg
{
	// 入力サンプルです。
	const void *src;
	// 入力形式です。
	audio_format2_t *srcfmt;

	// 出力サンプル用のバッファです。
	void *dst;
	// 出力形式です。
	audio_format2_t *dstfmt;

	// 今回のフィルタ呼び出しで入出力可能なフレーム数です。
	// (内部で使用する周波数変換フィルタの場合は出力フレーム数)
	int count;

	// フィルタ固有のデータ用に使用できます。
	void *context;
};

typedef void(*audio_filter_t)(audio_filter_arg_t *arg);

// フィルタ登録用
// hw_if->set_params2() からフィルタを登録するときに使う構造体
typedef struct audio_filter_reg {
	audio_filter_t codec;
	void *context;
} audio_filter_reg_t;
