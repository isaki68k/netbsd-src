#ifndef _SYS_DEV_AUDIO_AUCODEC_H_
#define _SYS_DEV_AUDIO_AUCODEC_H_

extern void audio_internal_to_linear8(audio_filter_arg_t *);
extern void audio_internal_to_linear16(audio_filter_arg_t *);
extern void audio_internal_to_linear24(audio_filter_arg_t *);
extern void audio_internal_to_linear32(audio_filter_arg_t *);
extern void audio_linear8_to_internal(audio_filter_arg_t *);
extern void audio_linear16_to_internal(audio_filter_arg_t *);
extern void audio_linear24_to_internal(audio_filter_arg_t *);
extern void audio_linear32_to_internal(audio_filter_arg_t *);

#endif /* !_SYS_DEV_AUDIO_AUCODEC_H_ */
