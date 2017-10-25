#pragma once
/*
* ***** audio_format *****
*/

/*
* フォーマットがおおむね有効かどうかを返します。
*/
static inline bool
is_valid_format(audio_format_t *format)
{
	if (format == NULL) return false;
	/* XXX:この条件どうするか検討 (MSM6258)*/
	if (format->encoding == AUDIO_ENCODING_MSM6258) {
		if (format->stride != 4) return false;
	} else {
		if (format->stride % 8 != 0) return false;
	}
	if (format->precision > format->stride) return false;
	if (format->channels <= 0) return false;
	if (format->channels > AUDIO_MAX_CH) return false;

	/* XXX: NO CHECK FOR ENCODING */
	return true;
}

/*
* 内部フォーマットかどうかを返します。
* ただし、周波数とチャンネル数はチェックしません。
*/
static inline bool
is_internal_format(audio_format_t *fmt)
{
	if (!is_valid_format(fmt)) return false;
	if (fmt->encoding != AUDIO_ENCODING_SLINEAR_HE) return false;
	if (fmt->precision != AUDIO_INTERNAL_BITS) return false;
	if (fmt->stride != AUDIO_INTERNAL_BITS) return false;
	return true;
}

// いずれかの LINEAR なら true
static inline bool
is_LINEAR(audio_format_t *lane_fmt)
{
	return
		(lane_fmt->encoding == AUDIO_ENCODING_SLINEAR_LE)
		|| (lane_fmt->encoding == AUDIO_ENCODING_SLINEAR_BE)
		|| (lane_fmt->encoding == AUDIO_ENCODING_ULINEAR_LE)
		|| (lane_fmt->encoding == AUDIO_ENCODING_ULINEAR_BE)
		;
}

// いずれかの SLINEAR なら true
static inline bool
is_SIGNED(audio_format_t *lane_fmt)
{
	return
		(lane_fmt->encoding == AUDIO_ENCODING_SLINEAR_LE)
		|| (lane_fmt->encoding == AUDIO_ENCODING_SLINEAR_BE)
		;
}

// ENDIAN を返す
static inline int
data_ENDIAN(audio_format_t *lane_fmt)
{
	if (lane_fmt->stride == 8) {
		/* HOST ENDIAN */
		return BYTE_ORDER;
	}

	if (lane_fmt->encoding == AUDIO_ENCODING_SLINEAR_LE
		|| lane_fmt->encoding == AUDIO_ENCODING_ULINEAR_LE) {
		return LITTLE_ENDIAN;
	}
	if (lane_fmt->encoding == AUDIO_ENCODING_SLINEAR_BE
		|| lane_fmt->encoding == AUDIO_ENCODING_ULINEAR_BE) {
		return BIG_ENDIAN;
	}
	return BYTE_ORDER;
}
