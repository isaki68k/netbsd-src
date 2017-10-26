
#include <stdbool.h>

#include "audiovar.h"
#include "auring.h"
#include "aucodec.h"
#include "auformat.h"

/*
* ***** audio_filter_arg *****
*/

bool
is_valid_filter_arg(audio_filter_arg_t *arg)
{
	if (arg == NULL) return false;
	if (!is_valid_format(arg->src_fmt)) return false;
	if (!is_valid_format(arg->dst_fmt)) return false;
	if (arg->src == NULL) return false;
	if (arg->dst == NULL) return false;
	if (arg->count <= 0) return false;
	return true;
}

int mulaw_to_internal(audio_filter_arg_t *);
int internal_to_mulaw(audio_filter_arg_t *);

int internal_to_linear8(audio_filter_arg_t *);
int internal_to_linear16(audio_filter_arg_t *);
int internal_to_linear24(audio_filter_arg_t *);
int internal_to_linear32(audio_filter_arg_t *);
int linear8_to_internal(audio_filter_arg_t *);
int linear16_to_internal(audio_filter_arg_t *);
int linear24_to_internal(audio_filter_arg_t *);
int linear32_to_internal(audio_filter_arg_t *);

void *msm6258_context_create();
void msm6258_context_destroy(void *context);
int internal_to_msm6258(audio_filter_arg_t *arg);
int msm6258_to_internal(audio_filter_arg_t *arg);

audio_filter_t
audio_MI_codec_filter_init(audio_filter_arg_t *arg)
{
	arg->context = NULL;

	if (is_internal_format(arg->src_fmt)) {
		if (arg->dst_fmt->encoding == AUDIO_ENCODING_MULAW) {
			return internal_to_mulaw;
		} else
		if (arg->dst_fmt->encoding == AUDIO_ENCODING_MSM6258) {
			arg->context = msm6258_context_create();
			return internal_to_msm6258;
		} else
		if (is_LINEAR(arg->dst_fmt)) {
			if (arg->dst_fmt->stride == 8) {
				return internal_to_linear8;
			} else if (arg->dst_fmt->stride == 16) {
				return internal_to_linear16;
			} else if (arg->dst_fmt->stride == 24) {
				return internal_to_linear24;
			} else if (arg->dst_fmt->stride == 32) {
				return internal_to_linear32;
			} else {
				panic("unsupported stride %d", arg->dst_fmt->stride);
			}
		}
	} else if (is_internal_format(arg->dst_fmt)) {
		if (arg->src_fmt->encoding == AUDIO_ENCODING_MULAW) {
			return mulaw_to_internal;
		} else
		if (arg->src_fmt->encoding == AUDIO_ENCODING_MSM6258) {
			arg->context = msm6258_context_create();
			return msm6258_to_internal;
		} else
		if (is_LINEAR(arg->src_fmt)) {
			if (arg->src_fmt->stride == 8) {
				return linear8_to_internal;
			} else if (arg->src_fmt->stride == 16) {
				return linear16_to_internal;
			} else if (arg->src_fmt->stride == 24) {
				return linear24_to_internal;
			} else if (arg->src_fmt->stride == 32) {
				return linear32_to_internal;
			} else {
				panic("unsupported stride %d", arg->src_fmt->stride);
			}
		}
	} else {
		panic("unsupported encoding");
	}
}

void
audio_MI_filter_finalize(audio_filter_arg_t *arg)
{
	if (arg->context) {
		// まあとりあえず
		msm6258_context_destroy(arg->context);
	}
}

