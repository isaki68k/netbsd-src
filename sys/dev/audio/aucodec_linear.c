
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
void
linear8_to_internal(audio_convert_arg_t *arg)
{
	KASSERT(is_valid_convert_arg(arg));
	KASSERT(is_LINEAR(arg->src->fmt));
	KASSERT(arg->src->fmt->stride == 8);
	KASSERT(is_internal_format(arg->dst->fmt));
	KASSERT(arg->src->fmt->channels == arg->dst->fmt->channels);

	uint8_t *sptr = RING_TOP(uint8_t, arg->src);
	internal_t *dptr = RING_BOT(internal_t, arg->dst);
	int sample_count = arg->count * arg->src->fmt->channels;

	int src_lsl = AUDIO_INTERNAL_BITS - arg->src->fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->src->fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	for (int i = 0; i < sample_count; i++) {
		internal_t s;
		s = *sptr++;
		s <<= src_lsl;
		s ^= xor;
		*dptr++ = s;
	}
	audio_ring_appended(arg->dst, arg->count);
	audio_ring_tookfromtop(arg->src, arg->count);
}

/*
* internal から [US]LINEAR(?,stride=8){BE|LE} への変換
*/
void
internal_to_linear8(audio_convert_arg_t *arg)
{
	KASSERT(is_valid_convert_arg(arg));
	KASSERT(is_LINEAR(arg->dst->fmt));
	KASSERT(arg->dst->fmt->stride == 8);
	KASSERT(is_internal_format(arg->src->fmt));
	KASSERT(arg->src->fmt->channels == arg->dst->fmt->channels);

	internal_t *sptr = RING_TOP(internal_t, arg->src);
	uint8_t *dptr = RING_BOT(uint8_t, arg->dst);
	int sample_count = arg->count * arg->src->fmt->channels;

	int src_lsr = AUDIO_INTERNAL_BITS - arg->dst->fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->dst->fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	for (int i = 0; i < sample_count; i++) {
		internal_t s;
		s = *sptr++;
		s ^= xor;
		s >>= src_lsr;
		*dptr++ = (uint8_t)s;
	}
	audio_ring_appended(arg->dst, arg->count);
	audio_ring_tookfromtop(arg->src, arg->count);
}

/*
* [US]LINEAR(?,stride=16){BE|LE} から internal への変換
*/
void
linear16_to_internal(audio_convert_arg_t *arg)
{
	KASSERT(is_valid_convert_arg(arg));
	KASSERT(is_LINEAR(arg->src->fmt));
	KASSERT(arg->src->fmt->stride == 16);
	KASSERT(is_internal_format(arg->dst->fmt));
	KASSERT(arg->src->fmt->channels == arg->dst->fmt->channels);

	uint16_t *sptr = RING_TOP(uint16_t, arg->src);
	internal_t *dptr = RING_BOT(internal_t, arg->dst);
	int sample_count = arg->count * arg->src->fmt->channels;

	int src_lsl = AUDIO_INTERNAL_BITS - arg->src->fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->src->fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	/* 16bit だけ高速化するとかでも良いかもしれない。 */
	if (data_ENDIAN(arg->src->fmt) == BYTE_ORDER) {
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
	audio_ring_appended(arg->dst, arg->count);
	audio_ring_tookfromtop(arg->src, arg->count);
}

/*
* internal から [US]LINEAR(?,stride=16){BE|LE} への変換
*/
void
internal_to_linear16(audio_convert_arg_t *arg)
{
	KASSERT(is_valid_convert_arg(arg));
	KASSERT(is_LINEAR(arg->dst->fmt));
	KASSERT(arg->dst->fmt->stride == 16);
	KASSERT(is_internal_format(arg->src->fmt));
	KASSERT(arg->src->fmt->channels == arg->dst->fmt->channels);

	internal_t *sptr = RING_TOP(internal_t, arg->src);
	uint16_t *dptr = RING_BOT(uint16_t, arg->dst);
	int sample_count = arg->count * arg->src->fmt->channels;

	int src_lsr = AUDIO_INTERNAL_BITS - arg->dst->fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->dst->fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	/* 16bit だけ高速化するとかでも良いかもしれない。 */
	if (data_ENDIAN(arg->src->fmt) == BYTE_ORDER) {
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
	audio_ring_appended(arg->dst, arg->count);
	audio_ring_tookfromtop(arg->src, arg->count);
}

/*
* [US]LINEAR(?,stride=24){BE|LE} から internal への変換
*/
void
linear24_to_internal(audio_convert_arg_t *arg)
{
	KASSERT(is_valid_convert_arg(arg));
	KASSERT(is_LINEAR(arg->src->fmt));
	KASSERT(arg->src->fmt->stride == 24);
	KASSERT(is_internal_format(arg->dst->fmt));
	KASSERT(arg->src->fmt->channels == arg->dst->fmt->channels);

	uint8_t *sptr = RING_TOP_UINT8(arg->src);
	internal_t *dptr = RING_BOT(internal_t, arg->dst);
	int sample_count = arg->count * arg->src->fmt->channels;

	/* 一旦 32bit にする */
	int src_lsl = 32 - arg->src->fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->src->fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	/* 遅くてもコードサイズを優先して目をつぶる */

	bool is_src_LE = (data_ENDIAN(arg->src->fmt) == LITTLE_ENDIAN);

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
	audio_ring_appended(arg->dst, arg->count);
	audio_ring_tookfromtop(arg->src, arg->count);
}

/*
* internal から [US]LINEAR(?,stride=24){BE|LE} への変換
*/
void
internal_to_linear24(audio_convert_arg_t *arg)
{
	KASSERT(is_valid_convert_arg(arg));
	KASSERT(is_LINEAR(arg->dst->fmt));
	KASSERT(arg->dst->fmt->stride == 24);
	KASSERT(is_internal_format(arg->src->fmt));
	KASSERT(arg->src->fmt->channels == arg->dst->fmt->channels);

	internal_t *sptr = RING_TOP(internal_t, arg->src);
	uint8_t *dptr = RING_BOT_UINT8(arg->dst);
	int sample_count = arg->count * arg->src->fmt->channels;

	/* 一旦 32bit にする */
	int src_lsr = 32 - arg->dst->fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->dst->fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	/* 遅くてもコードサイズを優先して目をつぶる */

	bool is_dst_LE = (data_ENDIAN(arg->dst->fmt) == LITTLE_ENDIAN);

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
	audio_ring_appended(arg->dst, arg->count);
	audio_ring_tookfromtop(arg->src, arg->count);
}

/*
* [US]LINEAR(?,stride=32){BE|LE} から internal への変換
*/
void
linear32_to_internal(audio_convert_arg_t *arg)
{
	KASSERT(is_valid_convert_arg(arg));
	KASSERT(is_LINEAR(arg->src->fmt));
	KASSERT(arg->src->fmt->stride == 32);
	KASSERT(is_internal_format(arg->dst->fmt));
	KASSERT(arg->src->fmt->channels == arg->dst->fmt->channels);

	uint32_t *sptr = RING_TOP(uint32_t, arg->src);
	internal_t *dptr = RING_BOT(internal_t, arg->dst);
	int sample_count = arg->count * arg->src->fmt->channels;

	int src_lsl = 32 - arg->src->fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->src->fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	/* 遅くてもコードサイズを優先して目をつぶる */

	bool is_src_HE = (data_ENDIAN(arg->src->fmt) == BYTE_ORDER);

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
	audio_ring_appended(arg->dst, arg->count);
	audio_ring_tookfromtop(arg->src, arg->count);
}

/*
* internal から [US]LINEAR(?,stride=32){BE|LE} への変換
*/
void
internal_to_linear32(audio_convert_arg_t *arg)
{
	KASSERT(is_valid_convert_arg(arg));
	KASSERT(is_LINEAR(arg->dst->fmt));
	KASSERT(arg->dst->fmt->stride == 32);
	KASSERT(is_internal_format(arg->src->fmt));
	KASSERT(arg->src->fmt->channels == arg->dst->fmt->channels);

	internal_t *sptr = RING_TOP(internal_t, arg->src);
	uint32_t *dptr = RING_BOT(uint32_t, arg->dst);
	int sample_count = arg->count * arg->src->fmt->channels;

	int src_lsr = 32 - arg->dst->fmt->precision;
	/* unsigned convert to signed */
	uinternal_t xor = is_SIGNED(arg->dst->fmt) ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	/* 遅くてもコードサイズを優先して目をつぶる */

	bool is_dst_HE = (data_ENDIAN(arg->dst->fmt) == BYTE_ORDER);

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
	audio_ring_appended(arg->dst, arg->count);
	audio_ring_tookfromtop(arg->src, arg->count);
}
