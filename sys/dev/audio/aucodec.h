#pragma once

bool is_valid_filter_arg(const audio_filter_arg_t *arg);

void audio_mulaw_to_internal(audio_filter_arg_t *);
void audio_internal_to_mulaw(audio_filter_arg_t *);

void audio_internal_to_linear8(audio_filter_arg_t *);
void audio_internal_to_linear16(audio_filter_arg_t *);
void audio_internal_to_linear24(audio_filter_arg_t *);
void audio_internal_to_linear32(audio_filter_arg_t *);
void audio_linear8_to_internal(audio_filter_arg_t *);
void audio_linear16_to_internal(audio_filter_arg_t *);
void audio_linear24_to_internal(audio_filter_arg_t *);
void audio_linear32_to_internal(audio_filter_arg_t *);
