#pragma once
/*
* ***** audio_format2 *****
*/

/*
* フォーマットがおおむね有効かどうかを返します。
*/
static inline bool
is_valid_format(const audio_format2_t *fmt)
{
	if (fmt == NULL) return false;
	/* XXX:この条件どうするか検討 (MSM6258)*/
	if (fmt->encoding == AUDIO_ENCODING_MSM6258) {
		if (fmt->stride != 4) return false;
	} else {
		if (fmt->stride % 8 != 0) return false;
	}
	if (fmt->precision > fmt->stride) return false;
	if (fmt->channels <= 0) return false;
	if (fmt->channels > AUDIO_MAX_CHANNELS) return false;

	/* XXX: NO CHECK FOR ENCODING */
	return true;
}

/*
* 内部フォーマットかどうかを返します。
* ただし、周波数とチャンネル数はチェックしません。
*/
static inline bool
is_internal_format(const audio_format2_t *fmt)
{
	if (!is_valid_format(fmt)) return false;
	if (fmt->encoding != AUDIO_ENCODING_SLINEAR_HE) return false;
	if (fmt->precision != AUDIO_INTERNAL_BITS) return false;
	if (fmt->stride != AUDIO_INTERNAL_BITS) return false;
	return true;
}

// いずれかの LINEAR なら true
static inline bool
is_LINEAR(const audio_format2_t *fmt)
{
	return
		(fmt->encoding == AUDIO_ENCODING_SLINEAR_LE)
		|| (fmt->encoding == AUDIO_ENCODING_SLINEAR_BE)
		|| (fmt->encoding == AUDIO_ENCODING_ULINEAR_LE)
		|| (fmt->encoding == AUDIO_ENCODING_ULINEAR_BE)
		;
}

// いずれかの SLINEAR なら true
static inline bool
is_SIGNED(const audio_format2_t *fmt)
{
	return
		(fmt->encoding == AUDIO_ENCODING_SLINEAR_LE)
		|| (fmt->encoding == AUDIO_ENCODING_SLINEAR_BE)
		;
}

// ENDIAN を返す
static inline int
data_ENDIAN(const audio_format2_t *fmt)
{
	if (fmt->stride == 8) {
		/* HOST ENDIAN */
		return BYTE_ORDER;
	}

	if (fmt->encoding == AUDIO_ENCODING_SLINEAR_LE
		|| fmt->encoding == AUDIO_ENCODING_ULINEAR_LE) {
		return LITTLE_ENDIAN;
	}
	if (fmt->encoding == AUDIO_ENCODING_SLINEAR_BE
		|| fmt->encoding == AUDIO_ENCODING_ULINEAR_BE) {
		return BIG_ENDIAN;
	}
	return BYTE_ORDER;
}

static inline int
frametobyte(const audio_format2_t *fmt, int frames)
{
	return frames * fmt->channels * fmt->stride / 8;
}

static inline int
frame_per_block_roundup(const audio_trackmixer_t *mixer, const audio_format2_t *fmt)
{
	return (fmt->sample_rate * mixer->blktime_n + mixer->blktime_d - 1) / mixer->blktime_d;
}
