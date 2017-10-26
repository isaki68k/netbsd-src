#pragma once

bool is_valid_filter_arg(audio_filter_arg_t *arg);

audio_filter_t audio_MI_codec_filter_init(audio_filter_arg_t *arg);
void audio_MI_filter_finalize(audio_filter_arg_t *arg);

