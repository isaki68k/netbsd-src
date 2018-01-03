#if !defined(_KERNEL)
#include <stdbool.h>

#include "audiovar.h"
#include "auring.h"
#include "aucodec.h"
#include "auformat.h"
#endif // !_KERNEL

/*
* ***** audio_filter_arg *****
*/

bool
is_valid_filter_arg(const audio_filter_arg_t *arg)
{
	if (arg == NULL) return false;
	if (!is_valid_format(arg->srcfmt)) return false;
	if (!is_valid_format(arg->dstfmt)) return false;
	if (arg->src == NULL) return false;
	if (arg->dst == NULL) return false;
	if (arg->count <= 0) return false;
	return true;
}

audio_filter_t
audio_MI_codec_filter_init(audio_filter_arg_t *arg)
{
	arg->context = NULL;

	if (is_internal_format(arg->srcfmt)) {
		if (arg->dstfmt->encoding == AUDIO_ENCODING_ULAW) {
			return internal_to_mulaw;
		} else
		if (is_LINEAR(arg->dstfmt)) {
			if (arg->dstfmt->stride == 8) {
				return internal_to_linear8;
			} else if (arg->dstfmt->stride == 16) {
				return internal_to_linear16;
			} else if (arg->dstfmt->stride == 24) {
				return internal_to_linear24;
			} else if (arg->dstfmt->stride == 32) {
				return internal_to_linear32;
			} else {
				panic("unsupported stride %d", arg->dstfmt->stride);
			}
		}
	} else if (is_internal_format(arg->dstfmt)) {
		if (arg->srcfmt->encoding == AUDIO_ENCODING_ULAW) {
			return mulaw_to_internal;
		} else
		if (is_LINEAR(arg->srcfmt)) {
			if (arg->srcfmt->stride == 8) {
				return linear8_to_internal;
			} else if (arg->srcfmt->stride == 16) {
				return linear16_to_internal;
			} else if (arg->srcfmt->stride == 24) {
				return linear24_to_internal;
			} else if (arg->srcfmt->stride == 32) {
				return linear32_to_internal;
			} else {
				panic("unsupported stride %d", arg->srcfmt->stride);
			}
		}
	}
	panic("unsupported encoding");
}

void
audio_MI_filter_finalize(audio_filter_arg_t *arg)
{
}

