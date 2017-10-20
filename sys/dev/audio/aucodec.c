
#include <stdbool.h>

#include "audiovar.h"
#include "auring.h"
#include "aucodec.h"
#include "auformat.h"

/*
* ***** audio_convert_arg *****
*/

bool
is_valid_convert_arg(audio_convert_arg_t *arg)
{
	if (arg == NULL) return false;
	if (!is_valid_format(arg->src->fmt)) return false;
	if (!is_valid_format(arg->dst->fmt)) return false;
	if (!is_valid_ring(arg->src)) return false;
	if (!is_valid_ring(arg->dst)) return false;
	if (arg->count <= 0) return false;
	if (arg->count > audio_ring_unround_count(arg->src)) return false;
	if (arg->count > audio_ring_unround_free_count(arg->dst)) return false;
	return true;
}

void mulaw_to_internal(audio_convert_arg_t *);
void internal_to_mulaw(audio_convert_arg_t *);

void internal_to_linear8(audio_convert_arg_t *);
void internal_to_linear16(audio_convert_arg_t *);
void internal_to_linear24(audio_convert_arg_t *);
void internal_to_linear32(audio_convert_arg_t *);
void linear8_to_internal(audio_convert_arg_t *);
void linear16_to_internal(audio_convert_arg_t *);
void linear24_to_internal(audio_convert_arg_t *);
void linear32_to_internal(audio_convert_arg_t *);

void *msm6258_context_create();
void msm6258_context_destroy(void *context);
void internal_to_msm6258(audio_convert_arg_t *arg);
void msm6258_to_internal(audio_convert_arg_t *arg);

void
audio_codec_initialize(audio_codec_t *codec, audio_format_t *fmt)
{
	codec->fmt = *fmt;
	codec->context = NULL;

	if (fmt->encoding == AUDIO_ENCODING_MULAW) {
		codec->to_internal = mulaw_to_internal;
		codec->from_internal = internal_to_mulaw;
	} else
	if (fmt->encoding == AUDIO_ENCODING_MSM6258) {
		codec->to_internal = msm6258_to_internal;
		codec->from_internal = internal_to_msm6258;
		codec->context = msm6258_context_create();
	} else
	if (is_LINEAR(fmt)) {
		if (fmt->stride == 8) {
			codec->to_internal = linear8_to_internal;
			codec->from_internal = internal_to_linear8;
		} else if (fmt->stride == 16) {
			codec->to_internal = linear16_to_internal;
			codec->from_internal = internal_to_linear16;
		} else if (fmt->stride == 24) {
			codec->to_internal = linear24_to_internal;
			codec->from_internal = internal_to_linear24;
		} else if (fmt->stride == 32) {
			codec->to_internal = linear32_to_internal;
			codec->from_internal = internal_to_linear32;
		} else {
			panic();
		}
	} else {
		panic();
	}
}

void
audio_codec_finalize(audio_codec_t *codec)
{
	if (codec->fmt.encoding == AUDIO_ENCODING_MSM6258) {
		msm6258_context_destroy(codec->context);
	}
}

