#pragma once

// 内部フォーマットのビット数
#define AUDIO_INTERNAL_BITS		16
//#define AUDIO_INTERNAL_BITS		32

#if AUDIO_INTERNAL_BITS == 16

typedef int16_t  aint_t;	/* audio          integer */
typedef uint16_t auint_t;	/* audio unsigned integer */
typedef int32_t  aint2_t;	/* audio          wide integer */
typedef uint32_t auint2_t;	/* audio unsigned wide integer */
#define AINT_T_MAX	((aint_t)0x7fff)
#define AINT_T_MIN	((aint_t)0x8000)

#elif AUDIO_INTERNAL_BITS == 32

typedef int32_t  aint_t;
typedef uint32_t auint_t;
typedef int64_t  aint2_t;
typedef uint64_t auint2_t;
#define AINT_T_MAX	((aint_t)0x7fffffff)
#define AINT_T_MIN	((aint_t)0x80000000)

#else
#error Invalid AUDIO_INTERNAL_BITS
#endif

/* format */
typedef struct {
	int32_t  encoding;		/* AUDIO_ENCODING */
	uint32_t sample_rate;		/* Hz */
	uint8_t  channels;		/* 1.. */
	uint8_t  precision;		/* ex.24 (valid bits of sample,
					   must precision <= stride) */
	uint8_t  stride;		/* ex.32 (packing bits of sample) */
} audio_format2_t;

// フィルタに渡されるパラメータ一式です。
typedef struct {
	// 入力サンプルの開始位置です。
	const void *src;
	// 入力形式です。
	const audio_format2_t *srcfmt;

	// 出力サンプル用のバッファです。
	void *dst;
	// 出力形式です。
	const audio_format2_t *dstfmt;

	// 今回のフィルタ呼び出しで出力すべきフレーム数です。
	// フィルタはこのフレーム数を必ず出力してください。
	// 周波数変換フィルタ以外では入力フレーム数も同じになります。
	// フィルタはこの変数 arg->count の値を破壊しても構いません。
	int count;

	// フィルタ固有のデータ用に使用できます。
	void *context;
} audio_filter_arg_t;

typedef void(*audio_filter_t)(audio_filter_arg_t *arg);

// フィルタ登録用
// hw_if->set_params2() からフィルタを登録するときに使う構造体
typedef struct {
	audio_params_t param;	/* HW encoding parameter */
	audio_filter_t codec;	/* conversion function */
	void *context;			/* optional codec's argument */
} audio_filter_reg_t;
