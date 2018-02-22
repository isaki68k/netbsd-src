#if !defined(_KERNEL)
#include <stdint.h>
#include <stdbool.h>
#include "compat.h"
#include "auring.h"
#include "auformat.h"
#include "aucodec.h"
#endif // !_KERNEL

/*
 * linear8_to_internal:
 *	This filter performs conversion from [US]LINEAR8 to internal format.
 */
void
linear8_to_internal(audio_filter_arg_t *arg)
{
	const uint8_t *s;
	aint_t *d;
	auint_t xor;
	u_int sample_count;
	u_int i;

	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->srcfmt));
	KASSERT(arg->srcfmt->precision == 8);
	KASSERT(arg->srcfmt->stride == 8);
	KASSERT(is_internal_format(arg->dstfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	s = arg->src;
	d = arg->dst;
	sample_count = arg->count * arg->srcfmt->channels;
	xor = audio_format2_is_signed(arg->srcfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	for (i = 0; i < sample_count; i++) {
		aint_t val;

		val = *s++;
		val <<= AUDIO_INTERNAL_BITS - 8;
		val ^= xor;
		*d++ = val;
	}
}

/*
 * internal_to_linear8:
 *	This filter performs conversion from internal format to [US]LINEAR8.
 */
void
internal_to_linear8(audio_filter_arg_t *arg)
{
	const aint_t *s;
	uint8_t *d;
	auint_t xor;
	u_int sample_count;
	u_int i;

	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->dstfmt));
	KASSERT(arg->dstfmt->precision == 8);
	KASSERT(arg->dstfmt->stride == 8);
	KASSERT(is_internal_format(arg->srcfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	s = arg->src;
	d = arg->dst;
	sample_count = arg->count * arg->srcfmt->channels;
	xor = audio_format2_is_signed(arg->dstfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	for (i = 0; i < sample_count; i++) {
		aint_t val;
		val = *s++;
		val ^= xor;
		val >>= AUDIO_INTERNAL_BITS - 8;
		*d++ = (uint8_t)val;
	}
}

/*
 * linear16_to_internal:
 *	This filter performs conversion from [US]LINEAR16{LE,BE} to internal
 *	format.
 */
void
linear16_to_internal(audio_filter_arg_t *arg)
{
	const uint16_t *s;
	aint_t *d;
	auint_t xor;
	u_int sample_count;
	u_int src_lsl;
	u_int i;

	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->srcfmt));
	KASSERT(arg->srcfmt->precision == 16);
	KASSERT(arg->srcfmt->stride == 16);
	KASSERT(is_internal_format(arg->dstfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	s = arg->src;
	d = arg->dst;
	sample_count = arg->count * arg->srcfmt->channels;

	src_lsl = AUDIO_INTERNAL_BITS - arg->srcfmt->precision;
	xor = audio_format2_is_signed(arg->srcfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	// 16bit だけ高速化するとかでも良いかもしれない。
	if (audio_format2_endian(arg->srcfmt) == BYTE_ORDER) {
		if (src_lsl == 0) {
			if (xor == 0) {
				memcpy(d, s, sample_count * 2);
			} else {
				for (i = 0; i < sample_count; i++) {
					*d++ = (*s++) ^ xor;
				}
			}
		} else {
			if (xor == 0) {
				for (i = 0; i < sample_count; i++) {
					*d++ = (*s++) << src_lsl;
				}
			} else {
				for (i = 0; i < sample_count; i++) {
					*d++ = (*s++ << src_lsl) ^ xor;
				}
			}
		}
	} else {
		if (src_lsl == 0) {
			if (xor == 0) {
				for (i = 0; i < sample_count; i++) {
					*d++ = __builtin_bswap16(*s++);
				}
			} else {
				for (i = 0; i < sample_count; i++) {
					*d++ = __builtin_bswap16(*s++) ^ xor;
				}
			}
		} else {
			if (xor == 0) {
				for (i = 0; i < sample_count; i++) {
					*d++ = __builtin_bswap16(*s++) << src_lsl;
				}
			} else {
				for (i = 0; i < sample_count; i++) {
					*d++ = (__builtin_bswap16(*s++) << src_lsl) ^ xor;
				}
			}
		}
	}
}

/*
 * internal_to_linear16:
 *	This filter performs conversion from internal format to
 *	[US]LINEAR16{LE,BE}.
 */
void
internal_to_linear16(audio_filter_arg_t *arg)
{
	const aint_t *s;
	uint16_t *d;
	auint_t xor;
	u_int sample_count;
	u_int src_lsr;
	u_int i;

	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->dstfmt));
	KASSERT(arg->dstfmt->precision == 16);
	KASSERT(arg->dstfmt->stride == 16);
	KASSERT(is_internal_format(arg->srcfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	s = arg->src;
	d = arg->dst;
	sample_count = arg->count * arg->srcfmt->channels;

	src_lsr = AUDIO_INTERNAL_BITS - arg->dstfmt->precision;
	xor = audio_format2_is_signed(arg->dstfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	// 16bit だけ高速化するとかでも良いかもしれない。
	if (audio_format2_endian(arg->dstfmt) == BYTE_ORDER) {
		if (src_lsr == 0) {
			if (xor == 0) {
				memcpy(d, s, sample_count * 2);
			} else {
				for (i = 0; i < sample_count; i++) {
					*d++ = (uint16_t)((*s++) ^ xor);
				}
			}
		} else {
			if (xor == 0) {
				for (i = 0; i < sample_count; i++) {
					*d++ = (uint16_t)((*s++) >> src_lsr);
				}
			} else {
				for (i = 0; i < sample_count; i++) {
					*d++ = (uint16_t)((*s++ ^ xor) >> src_lsr);
				}
			}
		}
	} else {
		if (src_lsr == 0) {
			if (xor == 0) {
				for (i = 0; i < sample_count; i++) {
					*d++ = __builtin_bswap16(*s++);
				}
			} else {
				for (i = 0; i < sample_count; i++) {
					*d++ = __builtin_bswap16(*s++ ^ xor);
				}
			}
		} else {
			if (xor == 0) {
				for (i = 0; i < sample_count; i++) {
					*d++ = __builtin_bswap16(*s++ >> src_lsr);
				}
			} else {
				for (i = 0; i < sample_count; i++) {
					*d++ = __builtin_bswap16((*s++ ^ xor) >> src_lsr);
				}
			}
		}
	}
}

#if defined(AUDIO_SUPPORT_LINEAR24)
/*
 * linear24_to_internal:
 *	This filter performs conversion from [US]LINEAR24/24{LE,BE} to
 *	internal format.  It's rerely used so it's size optimization.
 */
void
linear24_to_internal(audio_filter_arg_t *arg)
{
	const uint8_t *s;
	aint_t *d;
	auint_t xor;
	u_int sample_count;
	u_int i;
	bool is_src_LE;

	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->srcfmt));
	KASSERT(arg->srcfmt->precision == 24);
	KASSERT(arg->srcfmt->stride == 24);
	KASSERT(is_internal_format(arg->dstfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	s = arg->src;
	d = arg->dst;
	sample_count = arg->count * arg->srcfmt->channels;
	xor = audio_format2_is_signed(arg->srcfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	is_src_LE = (audio_format2_endian(arg->srcfmt) == LITTLE_ENDIAN);

	for (i = 0; i < sample_count; i++) {
		uint32_t val;
		if (is_src_LE) {
			val = s[0] | (s[1] << 8) | (s[2] << 16);
		} else {
			val = (s[0] << 16) | (s[1] << 8) | s[2];
		}
		s += 3;

#if AUDIO_INTERNAL_BITS < 24
		val >>= 24 - AUDIO_INTERNAL_BITS;
#else
		val <<= AUDIO_INTERNAL_BITS - 24;
#endif
		val ^= xor;
		*d++ = val;
	}
}

/*
 * internal_to_linear24:
 *	This filter performs conversion from internal format to
 *	[US]LINEAR24/24{LE,BE}.  It's rarely used so it's size optimization.
 */
void
internal_to_linear24(audio_filter_arg_t *arg)
{
	const aint_t *s;
	uint8_t *d;
	auint_t xor;
	u_int sample_count;
	u_int i;
	bool is_dst_LE;

	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->dstfmt));
	KASSERT(arg->dstfmt->precision == 24);
	KASSERT(arg->dstfmt->stride == 24);
	KASSERT(is_internal_format(arg->srcfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	s = arg->src;
	d = arg->dst;
	sample_count = arg->count * arg->srcfmt->channels;
	xor = audio_format2_is_signed(arg->dstfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	is_dst_LE = (audio_format2_endian(arg->dstfmt) == LITTLE_ENDIAN);

	for (i = 0; i < sample_count; i++) {
		uint32_t val = *s++;
		val ^= xor;
#if AUDIO_INTERNAL_BITS < 24
		val <<= 24 - AUDIO_INTERNAL_BITS;
#else
		val >>= AUDIO_INTERNAL_BITS - 24;
#endif
		if (is_dst_LE) {
			d[0] = val & 0xff;
			d[1] = (val >> 8) & 0xff;
			d[2] = (val >> 16) & 0xff;
		} else {
			d[0] = (val >> 16) & 0xff;
			d[1] = (val >> 8) & 0xff;
			d[2] = val & 0xff;
		}
		d += 3;
	}
}
#endif /* AUDIO_SUPPORT_LINEAR24 */

/*
 * linear32_to_internal:
 *	This filter performs conversion from [US]LINEAR32{LE,BE} to internal
 *	format.  It's rarely used so it's size optimization.
 */
void
linear32_to_internal(audio_filter_arg_t *arg)
{
	const uint32_t *s;
	aint_t *d;
	auint_t xor;
	u_int sample_count;
	u_int i;
	bool is_src_NE;

	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->srcfmt));
	KASSERT(arg->srcfmt->precision == 32);
	KASSERT(arg->srcfmt->stride == 32);
	KASSERT(is_internal_format(arg->dstfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	s = arg->src;
	d = arg->dst;
	sample_count = arg->count * arg->srcfmt->channels;
	xor = audio_format2_is_signed(arg->srcfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	is_src_NE = (audio_format2_endian(arg->srcfmt) == BYTE_ORDER);

	for (i = 0; i < sample_count; i++) {
		uint32_t val = *s++;
		if (!is_src_NE) {
			val = __builtin_bswap32(val);
		}
		val >>= 32 - AUDIO_INTERNAL_BITS;
		val ^= xor;
		*d++ = val;
	}
}

/*
 * internal_to_linear32:
 *	This filter performs conversion from internal format to
 *	[US]LINEAR32{LE,BE}.  It's rarely used so it's size optimization.
 */
void
internal_to_linear32(audio_filter_arg_t *arg)
{
	const aint_t *s;
	uint32_t *d;
	auint_t xor;
	u_int sample_count;
	u_int i;
	bool is_dst_NE;

	KASSERT(is_valid_filter_arg(arg));
	KASSERT(audio_format2_is_linear(arg->dstfmt));
	KASSERT(arg->dstfmt->precision == 32);
	KASSERT(arg->dstfmt->stride == 32);
	KASSERT(is_internal_format(arg->srcfmt));
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);

	s = arg->src;
	d = arg->dst;
	sample_count = arg->count * arg->srcfmt->channels;
	xor = audio_format2_is_signed(arg->dstfmt)
	    ? 0 : (1 << (AUDIO_INTERNAL_BITS - 1));

	is_dst_NE = (audio_format2_endian(arg->dstfmt) == BYTE_ORDER);

	for (i = 0; i < sample_count; i++) {
		uint32_t val = *s++;
		val ^= xor;
		val <<= 32 - AUDIO_INTERNAL_BITS;
		if (!is_dst_NE) {
			val = __builtin_bswap32(val);
		}
		*d++ = val;
	}
}
