#pragma once

#ifdef DIAGNOSTIC
#define DIAGNOSTIC_format2(fmt)	audio_diagnostic_format2(__func__, (fmt))
#else
#define DIAGNOSTIC_format2(fmt)
#endif

#ifdef DIAGNOSTIC
static void audio_diagnostic_format2(const char *, const audio_format2_t *);
static void
audio_diagnostic_format2(const char *func, const audio_format2_t *fmt)
{

	KASSERTMSG(fmt, "%s: fmt == NULL", func);

	// XXX:この条件どうするか検討 (MSM6258)
	if (fmt->encoding == AUDIO_ENCODING_ADPCM) {
		KASSERTMSG(fmt->stride == 4,
		    "%s: stride(%d) is invalid", func, fmt->stride);
	} else {
		KASSERTMSG(fmt->stride % NBBY == 0,
		    "%s: stride(%d) is invalid", func, fmt->stride);
	}
	KASSERTMSG(fmt->precision <= fmt->stride,
	    "%s: precision(%d) <= stride(%d)",
	    func, fmt->precision, fmt->stride);
	KASSERTMSG(1 <= fmt->channels && fmt->channels <= AUDIO_MAX_CHANNELS,
	    "%s: channels(%d) is out of range",
	    func, fmt->channels);

	/* XXX: No check for encoding */
}
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
