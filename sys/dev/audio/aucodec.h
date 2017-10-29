#pragma once

bool is_valid_filter_arg(audio_filter_arg_t *arg);

audio_filter_t audio_MI_codec_filter_init(audio_filter_arg_t *arg);
void audio_MI_filter_finalize(audio_filter_arg_t *arg);

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

