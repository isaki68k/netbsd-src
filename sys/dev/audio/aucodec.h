#pragma once

bool is_valid_filter_arg(const audio_filter_arg_t *arg);

audio_filter_t audio_MI_codec_filter_init(audio_filter_arg_t *arg);
void audio_MI_filter_finalize(audio_filter_arg_t *arg);

void mulaw_to_internal(audio_filter_arg_t *);
void internal_to_mulaw(audio_filter_arg_t *);

void internal_to_linear8(audio_filter_arg_t *);
void internal_to_linear16(audio_filter_arg_t *);
void internal_to_linear24(audio_filter_arg_t *);
void internal_to_linear32(audio_filter_arg_t *);
void linear8_to_internal(audio_filter_arg_t *);
void linear16_to_internal(audio_filter_arg_t *);
void linear24_to_internal(audio_filter_arg_t *);
void linear32_to_internal(audio_filter_arg_t *);
