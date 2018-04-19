#pragma once

#ifdef DIAGNOSTIC
#define DIAGNOSTIC_format2(fmt)	audio_diagnostic_format2(__func__, (fmt))
extern void audio_diagnostic_format2(const char *, const audio_format2_t *);
#else
#define DIAGNOSTIC_format2(fmt)
#endif

/*
 * Return true if 'fmt' is the internal format.
 * It does not check for frequency and number of channels.
 */
static inline bool
audio_format2_is_internal(const audio_format2_t *fmt)
{

	DIAGNOSTIC_format2(fmt);

	if (fmt->encoding != AUDIO_ENCODING_SLINEAR_NE)
		return false;
	if (fmt->precision != AUDIO_INTERNAL_BITS)
		return false;
	if (fmt->stride != AUDIO_INTERNAL_BITS)
		return false;
	return true;
}

/*
 * Return true if fmt's encoding is one of LINEAR.
 */
static inline bool
audio_format2_is_linear(const audio_format2_t *fmt)
{
	return (fmt->encoding == AUDIO_ENCODING_SLINEAR_LE)
	    || (fmt->encoding == AUDIO_ENCODING_SLINEAR_BE)
	    || (fmt->encoding == AUDIO_ENCODING_ULINEAR_LE)
	    || (fmt->encoding == AUDIO_ENCODING_ULINEAR_BE);
}

/*
 * Return true if fmt's encoding is one of SLINEAR.
 */
static inline bool
audio_format2_is_signed(const audio_format2_t *fmt)
{
	return (fmt->encoding == AUDIO_ENCODING_SLINEAR_LE)
	    || (fmt->encoding == AUDIO_ENCODING_SLINEAR_BE);
}

/*
 * Return fmt's endian as LITTLE_ENDIAN or BIG_ENDIAN.
 */
static inline int
audio_format2_endian(const audio_format2_t *fmt)
{
	if (fmt->stride == 8) {
		/* HOST ENDIAN */
		return BYTE_ORDER;
	}

	if (fmt->encoding == AUDIO_ENCODING_SLINEAR_LE ||
	    fmt->encoding == AUDIO_ENCODING_ULINEAR_LE) {
		return LITTLE_ENDIAN;
	}
	if (fmt->encoding == AUDIO_ENCODING_SLINEAR_BE ||
	    fmt->encoding == AUDIO_ENCODING_ULINEAR_BE) {
		return BIG_ENDIAN;
	}
	return BYTE_ORDER;
}
