#pragma once

// フォーマットがおおむね有効かどうかを返します。
static inline bool
audio_format2_is_valid(const audio_format2_t *fmt)
{
	KASSERT(fmt);

	// XXX:この条件どうするか検討 (MSM6258)
	if (fmt->encoding == AUDIO_ENCODING_ADPCM) {
		if (fmt->stride != 4) {
			printf("%s: fmt->stride=%d\n", __func__, fmt->stride);
			return false;
		}
	} else {
		if ((fmt->stride % NBBY) != 0) {
			printf("%s: fmt->stride=%d\n", __func__, fmt->stride);
			return false;
		}
	}
	if (fmt->precision > fmt->stride) {
		printf("%s: fmt->precision(%d) <= fmt->stride(%d)\n",
		    __func__, fmt->precision, fmt->stride);
		return false;
	}
	if (fmt->channels < 1 || fmt->channels > AUDIO_MAX_CHANNELS) {
		printf("%s: fmt->channels=%d\n", __func__, fmt->channels);
		return false;
	}

	/* XXX: NO CHECK FOR ENCODING */
	return true;
}

/*
 * Return true if 'fmt' is the internal format.
 * It does not check for frequency and number of channels.
 */
static inline bool
audio_format2_is_internal(const audio_format2_t *fmt)
{
	if (!audio_format2_is_valid(fmt))
		return false;
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
