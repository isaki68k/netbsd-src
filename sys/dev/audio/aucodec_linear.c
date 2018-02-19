#if !defined(_KERNEL)
#include <stdint.h>
#include <stdbool.h>
#include "compat.h"
#include "auring.h"
#include "auformat.h"
#include "aucodec.h"
#endif // !_KERNEL

// [US]LINEAR(?,stride=8){BE|LE} から internal への変換
void
linear8_to_internal(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->srcfmt));
	KASSERT(arg->srcfmt->stride == 8);
	KASSERT(is_internal_format(arg->dstfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	const uint8_t *sptr = arg->src;
	aint_t *dptr = arg->dst;
	int sample_count = arg->count * arg->srcfmt->channels;

	int src_lsl = AUDIO_INTERNAL_BITS - arg->srcfmt->precision;
	/* unsigned -> signed */
	auint_t xor = audio_format2_is_signed(arg->srcfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	for (int i = 0; i < sample_count; i++) {
		aint_t s;
		s = *sptr++;
		s <<= src_lsl;
		s ^= xor;
		*dptr++ = s;
	}
}

// internal から [US]LINEAR(?,stride=8){BE|LE} への変換
void
internal_to_linear8(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->dstfmt));
	KASSERT(arg->dstfmt->stride == 8);
	KASSERT(is_internal_format(arg->srcfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	const aint_t *sptr = arg->src;
	uint8_t *dptr = arg->dst;
	int sample_count = arg->count * arg->srcfmt->channels;

	int src_lsr = AUDIO_INTERNAL_BITS - arg->dstfmt->precision;
	/* unsigned -> signed */
	auint_t xor = audio_format2_is_signed(arg->dstfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	for (int i = 0; i < sample_count; i++) {
		aint_t s;
		s = *sptr++;
		s ^= xor;
		s >>= src_lsr;
		*dptr++ = (uint8_t)s;
	}
}

// [US]LINEAR(?,stride=16){BE|LE} から internal への変換
void
linear16_to_internal(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->srcfmt));
	KASSERT(arg->srcfmt->stride == 16);
	KASSERT(is_internal_format(arg->dstfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	const uint16_t *sptr = arg->src;
	aint_t *dptr = arg->dst;
	int sample_count = arg->count * arg->srcfmt->channels;

	int src_lsl = AUDIO_INTERNAL_BITS - arg->srcfmt->precision;
	/* unsigned -> signed */
	auint_t xor = audio_format2_is_signed(arg->srcfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	// 16bit だけ高速化するとかでも良いかもしれない。
	if (audio_format2_endian(arg->srcfmt) == BYTE_ORDER) {
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
}

// internal から [US]LINEAR(?,stride=16){BE|LE} への変換
void
internal_to_linear16(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->dstfmt));
	KASSERT(arg->dstfmt->stride == 16);
	KASSERT(is_internal_format(arg->srcfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	const aint_t *sptr = arg->src;
	uint16_t *dptr = arg->dst;
	int sample_count = arg->count * arg->srcfmt->channels;

	int src_lsr = AUDIO_INTERNAL_BITS - arg->dstfmt->precision;
	/* unsigned -> signed */
	auint_t xor = audio_format2_is_signed(arg->dstfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	// 16bit だけ高速化するとかでも良いかもしれない。
	if (audio_format2_endian(arg->dstfmt) == BYTE_ORDER) {
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
}

#if defined(AUDIO_SUPPORT_LINEAR24)
// [US]LINEAR(?,stride=24){BE|LE} から internal への変換
void
linear24_to_internal(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->srcfmt));
	KASSERT(arg->srcfmt->stride == 24);
	KASSERT(is_internal_format(arg->dstfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	const uint8_t *sptr = arg->src;
	aint_t *dptr = arg->dst;
	int sample_count = arg->count * arg->srcfmt->channels;

	// 一旦 32bit にする
	int src_lsl = 32 - arg->srcfmt->precision;
	/* unsigned -> signed */
	auint_t xor = audio_format2_is_signed(arg->srcfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	// 遅くてもコードサイズを優先して目をつぶる

	bool is_src_LE = (audio_format2_endian(arg->srcfmt) == LITTLE_ENDIAN);

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
		aint_t s = u;
		s ^= xor;
		*dptr++ = s;
	}
}

// internal から [US]LINEAR(?,stride=24){BE|LE} への変換
void
internal_to_linear24(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->dstfmt));
	KASSERT(arg->dstfmt->stride == 24);
	KASSERT(is_internal_format(arg->srcfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	const aint_t *sptr = arg->src;
	uint8_t *dptr = arg->dst;
	int sample_count = arg->count * arg->srcfmt->channels;

	// 一旦 32bit にする
	int src_lsr = 32 - arg->dstfmt->precision;
	/* unsigned -> signed */
	auint_t xor = audio_format2_is_signed(arg->dstfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	// 遅くてもコードサイズを優先して目をつぶる

	bool is_dst_LE = (audio_format2_endian(arg->dstfmt) == LITTLE_ENDIAN);

	for (int i = 0; i < sample_count; i++) {
		aint_t s = *sptr++;
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
}
#endif /* AUDIO_SUPPORT_LINEAR24 */

// [US]LINEAR(?,stride=32){BE|LE} から internal への変換
void
linear32_to_internal(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->srcfmt));
	KASSERT(arg->srcfmt->stride == 32);
	KASSERT(is_internal_format(arg->dstfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	const uint32_t *sptr = arg->src;
	aint_t *dptr = arg->dst;
	int sample_count = arg->count * arg->srcfmt->channels;

	int src_lsl = 32 - arg->srcfmt->precision;
	/* unsigned -> signed */
	auint_t xor = audio_format2_is_signed(arg->srcfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	// 遅くてもコードサイズを優先して目をつぶる

	bool is_src_HE = (audio_format2_endian(arg->srcfmt) == BYTE_ORDER);

	for (int i = 0; i < sample_count; i++) {
		uint32_t u = *sptr++;
		if (!is_src_HE) {
			u = __builtin_bswap32(u);
		}
		u <<= src_lsl;
#if AUDIO_INTERNAL_BITS == 16
		u >>= 16;
#endif
		aint_t s = u;
		s ^= xor;
		*dptr++ = s;
	}
}

// internal から [US]LINEAR(?,stride=32){BE|LE} への変換
void
internal_to_linear32(audio_filter_arg_t *arg)
{
	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->dstfmt));
	KASSERT(arg->dstfmt->stride == 32);
	KASSERT(is_internal_format(arg->srcfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	const aint_t *sptr = arg->src;
	uint32_t *dptr = arg->dst;
	int sample_count = arg->count * arg->srcfmt->channels;

	int src_lsr = 32 - arg->dstfmt->precision;
	/* unsigned -> signed */
	auint_t xor = audio_format2_is_signed(arg->dstfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	// 遅くてもコードサイズを優先して目をつぶる

	bool is_dst_HE = (audio_format2_endian(arg->dstfmt) == BYTE_ORDER);

	for (int i = 0; i < sample_count; i++) {
		aint_t s = *sptr++;
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
}
