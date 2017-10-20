#pragma once

bool is_valid_convert_arg(audio_convert_arg_t *arg);


void audio_codec_initialize(audio_codec_t *codec, audio_format_t *fmt);
void audio_codec_finalize(audio_codec_t *codec);

