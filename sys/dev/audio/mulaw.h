#ifndef _SYS_DEV_AUDIO_MULAW_H_
#define _SYS_DEV_AUDIO_MULAW_H_

#if defined(_KERNEL)
#include <dev/audio_if.h>
#endif

extern void audio_mulaw_to_internal(audio_filter_arg_t *);
extern void audio_internal_to_mulaw(audio_filter_arg_t *);

#endif /* !_SYS_DEV_AUDIO_MULAW_H_ */
