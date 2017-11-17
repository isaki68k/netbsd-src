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

typedef void(*audio_filter_t)(audio_filter_arg_t *arg);

// フィルタ登録用
// hw_if->set_params2() からフィルタを登録するときに使う構造体
typedef struct audio_filter_reg {
	audio_filter_t codec;
	void *context;
} audio_filter_reg_t;
