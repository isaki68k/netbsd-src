
#include <stdint.h>
#include <stdbool.h>
#include "compat.h"

#include "auring.h"
#include "auformat.h"
#include "aucodec.h"

/*
* ***** 変換関数 *****
* 変換関数では、
* arg->src の arg->count 個の有効フレームと
* arg->dst の arg->count 個の空きフレームに
* アンラウンディングでアクセス出来ることを呼び出し側が保証します。
* 変換に伴い、arg->src, arg->dst の ring ポインタを進めてください。
* src から読み取られたフレーム数を count にセットしてください。(等しい場合は何もしなくていい)
*/


/*
* [US]LINEAR(?,stride=8){BE|LE} から internal への変換
*/
int
linear8_to_internal(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(is_LINEAR(arg->src_fmt));
	KASSERT(arg->src_fmt->stride == 8);
	KASSERT(is_internal_format(arg->dst_fmt));
	KASSERT(arg->src_fmt->channels == arg->dst_fmt->channels);

	const uint8_t *sptr = arg->src;
	internal_t *dptr = arg->dst;
	int sample_count = arg->count * arg->src_fmt->channels;

	int src_lsl = AUDIO_INTERNAL_BITS - arg->src_fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->src_fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	for (int i = 0; i < sample_count; i++) {
		internal_t s;
		s = *sptr++;
		s <<= src_lsl;
		s ^= xor;
		*dptr++ = s;
	}
	return arg->count;
}

/*
* internal から [US]LINEAR(?,stride=8){BE|LE} への変換
*/
int
internal_to_linear8(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(is_LINEAR(arg->dst_fmt));
	KASSERT(arg->dst_fmt->stride == 8);
	KASSERT(is_internal_format(arg->src_fmt));
	KASSERT(arg->src_fmt->channels == arg->dst_fmt->channels);

	const internal_t *sptr = arg->src;
	uint8_t *dptr = arg->dst;
	int sample_count = arg->count * arg->src_fmt->channels;

	int src_lsr = AUDIO_INTERNAL_BITS - arg->dst_fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->dst_fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	for (int i = 0; i < sample_count; i++) {
		internal_t s;
		s = *sptr++;
		s ^= xor;
		s >>= src_lsr;
		*dptr++ = (uint8_t)s;
	}
	return arg->count;
}

/*
* [US]LINEAR(?,stride=16){BE|LE} から internal への変換
*/
int
linear16_to_internal(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(is_LINEAR(arg->src_fmt));
	KASSERT(arg->src_fmt->stride == 16);
	KASSERT(is_internal_format(arg->dst_fmt));
	KASSERT(arg->src_fmt->channels == arg->dst_fmt->channels);

	const uint16_t *sptr = arg->src;
	internal_t *dptr = arg->dst;
	int sample_count = arg->count * arg->src_fmt->channels;

	int src_lsl = AUDIO_INTERNAL_BITS - arg->src_fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->src_fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	/* 16bit だけ高速化するとかでも良いかもしれない。 */
	if (data_ENDIAN(arg->src_fmt) == BYTE_ORDER) {
		if (src_lsl == 0) {
			if (xor == 0) {
				memcpy(dptr, sptr, sample_count * 2);
			} else {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = (*sptr++) ^ xor;
				}
			}
		} else {
			if (xor == 0) {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = (*sptr++) << src_lsl;
				}
			} else {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = (*sptr++ << src_lsl) ^ xor;
				}
			}
		}
	} else {
		if (src_lsl == 0) {
			if (xor == 0) {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = __builtin_bswap16(*sptr++);
				}
			} else {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = __builtin_bswap16(*sptr++) ^ xor;
				}
			}
		} else {
			if (xor == 0) {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = __builtin_bswap16(*sptr++) << src_lsl;
				}
			} else {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = (__builtin_bswap16(*sptr++) << src_lsl) ^ xor;
				}
			}
		}
	}
	return arg->count;
}

/*
* internal から [US]LINEAR(?,stride=16){BE|LE} への変換
*/
int
internal_to_linear16(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(is_LINEAR(arg->dst_fmt));
	KASSERT(arg->dst_fmt->stride == 16);
	KASSERT(is_internal_format(arg->src_fmt));
	KASSERT(arg->src_fmt->channels == arg->dst_fmt->channels);

	const internal_t *sptr = arg->src;
	uint16_t *dptr = arg->dst;
	int sample_count = arg->count * arg->src_fmt->channels;

	int src_lsr = AUDIO_INTERNAL_BITS - arg->dst_fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->dst_fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	/* 16bit だけ高速化するとかでも良いかもしれない。 */
	if (data_ENDIAN(arg->src_fmt) == BYTE_ORDER) {
		if (src_lsr == 0) {
			if (xor == 0) {
				memcpy(dptr, sptr, sample_count * 2);
			} else {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = (uint16_t)((*sptr++) ^ xor);
				}
			}
		} else {
			if (xor == 0) {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = (uint16_t)((*sptr++) >> src_lsr);
				}
			} else {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = (uint16_t)((*sptr++ ^ xor) >> src_lsr);
				}
			}
		}
	} else {
		if (src_lsr == 0) {
			if (xor == 0) {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = __builtin_bswap16(*sptr++);
				}
			} else {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = __builtin_bswap16(*sptr++ ^ xor);
				}
			}
		} else {
			if (xor == 0) {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = __builtin_bswap16(*sptr++ >> src_lsr);
				}
			} else {
				for (int i = 0; i < sample_count; i++) {
					*dptr++ = __builtin_bswap16((*sptr++ ^ xor) >> src_lsr);
				}
			}
		}
	}
	return arg->count;
}

/*
* [US]LINEAR(?,stride=24){BE|LE} から internal への変換
*/
int
linear24_to_internal(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(is_LINEAR(arg->src_fmt));
	KASSERT(arg->src_fmt->stride == 24);
	KASSERT(is_internal_format(arg->dst_fmt));
	KASSERT(arg->src_fmt->channels == arg->dst_fmt->channels);

	const uint8_t *sptr = arg->src;
	internal_t *dptr = arg->dst;
	int sample_count = arg->count * arg->src_fmt->channels;

	/* 一旦 32bit にする */
	int src_lsl = 32 - arg->src_fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->src_fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	/* 遅くてもコードサイズを優先して目をつぶる */

	bool is_src_LE = (data_ENDIAN(arg->src_fmt) == LITTLE_ENDIAN);

	for (int i = 0; i < sample_count; i++) {
		uint32_t u;
		if (is_src_LE) {
			u = (uint32_t)sptr[0] + ((uint32_t)sptr[1] << 8) + ((uint32_t)sptr[2] << 16);
		} else {
			u = ((uint32_t)sptr[0] << 16) + ((uint32_t)sptr[1] << 8) + (uint32_t)sptr[2];
		}
		sptr += 3;

		u <<= src_lsl;
#if AUDIO_INTERNAL_BITS == 16
		u >>= 16;
#endif
		internal_t s = u;
		s ^= xor;
		*dptr++ = s;
	}
	return arg->count;
}

/*
* internal から [US]LINEAR(?,stride=24){BE|LE} への変換
*/
int
internal_to_linear24(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(is_LINEAR(arg->dst_fmt));
	KASSERT(arg->dst_fmt->stride == 24);
	KASSERT(is_internal_format(arg->src_fmt));
	KASSERT(arg->src_fmt->channels == arg->dst_fmt->channels);

	const internal_t *sptr = arg->src;
	uint8_t *dptr = arg->dst;
	int sample_count = arg->count * arg->src_fmt->channels;

	/* 一旦 32bit にする */
	int src_lsr = 32 - arg->dst_fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->dst_fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	/* 遅くてもコードサイズを優先して目をつぶる */

	bool is_dst_LE = (data_ENDIAN(arg->dst_fmt) == LITTLE_ENDIAN);

	for (int i = 0; i < sample_count; i++) {
		internal_t s = *sptr++;
		s ^= xor;
		uint32_t u = s;
#if AUDIO_INTERNAL_BITS == 16
		u <<= 16;
#endif
		u >>= src_lsr;
		if (is_dst_LE) {
			dptr[0] = u & 0xff;
			dptr[1] = (u >> 8) & 0xff;
			dptr[2] = (u >> 16) & 0xff;
		} else {
			dptr[0] = (u >> 16) & 0xff;
			dptr[1] = (u >> 8) & 0xff;
			dptr[2] = u & 0xff;
		}
		dptr += 3;
	}
	return arg->count;
}

/*
* [US]LINEAR(?,stride=32){BE|LE} から internal への変換
*/
int
linear32_to_internal(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(is_LINEAR(arg->src_fmt));
	KASSERT(arg->src_fmt->stride == 32);
	KASSERT(is_internal_format(arg->dst_fmt));
	KASSERT(arg->src_fmt->channels == arg->dst_fmt->channels);

	const uint32_t *sptr = arg->src;
	internal_t *dptr = arg->dst;
	int sample_count = arg->count * arg->src_fmt->channels;

	int src_lsl = 32 - arg->src_fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->src_fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	/* 遅くてもコードサイズを優先して目をつぶる */

	bool is_src_HE = (data_ENDIAN(arg->src_fmt) == BYTE_ORDER);

	for (int i = 0; i < sample_count; i++) {
		uint32_t u = *sptr++;
		if (!is_src_HE) {
			u = __builtin_bswap32(u);
		}
		u <<= src_lsl;
#if AUDIO_INTERNAL_BITS == 16
		u >>= 16;
#endif
		internal_t s = u;
		s ^= xor;
		*dptr++ = s;
	}
	return arg->count;
}

/*
* internal から [US]LINEAR(?,stride=32){BE|LE} への変換
*/
int
internal_to_linear32(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(is_LINEAR(arg->dst_fmt));
	KASSERT(arg->dst_fmt->stride == 32);
	KASSERT(is_internal_format(arg->src_fmt));
	KASSERT(arg->src_fmt->channels == arg->dst_fmt->channels);

	const internal_t *sptr = arg->src;
	uint32_t *dptr = arg->dst;
	int sample_count = arg->count * arg->src_fmt->channels;

	int src_lsr = 32 - arg->dst_fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->dst_fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	/* 遅くてもコードサイズを優先して目をつぶる */

	bool is_dst_HE = (data_ENDIAN(arg->dst_fmt) == BYTE_ORDER);

	for (int i = 0; i < sample_count; i++) {
		internal_t s = *sptr++;
		s ^= xor;
		uint32_t u = s;
#if AUDIO_INTERNAL_BITS == 16
		u <<= 16;
#endif
		u >>= src_lsr;
		if (!is_dst_HE) {
			u = __builtin_bswap32(u);
		}
		*dptr++ = u;
	}
	return arg->count;
}
