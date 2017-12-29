#pragma once

#if !defined(_KERNEL)
#include <stdint.h>
#include "compat.h"
#include "userland.h"
#include "queue.h"
#include <stdbool.h>
#include "audiovar.h"
#endif

/*
e.g. stereo
+----------------------------------------------------------------------
|    L    |    R    |    L    |    R    | ..... |    L    |    R    |
+----------------------------------------------------------------------
 <sample->
 <------frame------>
 <-----------block-(40msec)----------------------------------------->

 */



/*
 * bidirectional logical shift left
 * shift > 0 : left shift
 * shift == 0 : no shift
 * shift < 0: right shift, as lsr(abs(shift))
 */
static inline uint32_t
bidi_lsl(uint32_t a, int shift)
{
	if (shift > 0) return a << shift;
	if (shift < 0) return a >> -shift;
	return a;
}


int audio_track_init(audio_track_t *track, audio_trackmixer_t *mixer, int mode);
void audio_track_destroy(audio_track_t *track);
int audio_track_set_format(audio_track_t *track, audio_format2_t *track_fmt);
void audio_track_play(audio_track_t *track, bool isdrain);
int audio_track_drain(audio_track_t *track, bool wait);

int audio_mixer_init(struct audio_softc *sc, audio_trackmixer_t *mixer, int mode);
void audio_mixer_destroy(audio_trackmixer_t *mixer, int mode);
bool audio_pmixer_start(audio_trackmixer_t *mixer, bool force);
void audio_pmixer_process(audio_trackmixer_t *mixer);
int  audio_pmixer_mix_track(audio_trackmixer_t *mixer, audio_track_t *track, int req, int mixed);
void audio_pmixer_intr(audio_trackmixer_t *mixer);

int  audio2_halt_output(struct audio_softc *sc);

/* glue layer */
int audio_write(struct audio_softc *sc, struct uio *uio, int ioflag, audio_file_t *file); /* write の MI 側 */

#if !defined(_KERNEL)

/* system call emulation */
audio_file_t *sys_open(struct audio_softc *sc, int mode);
int/*ssize_t*/ sys_write(audio_file_t *file, void* buf, size_t len);	/* write syscall の Userland 側エミュレート */
int sys_ioctl_drain(audio_track_t *track, bool wait);	/* ioctl(AUDIO_DRAIN) */

#endif // _KERNEL

/* XXX: 分類未定 */
int audio_softc_get_hw_capacity(struct audio_softc *sc);
audio_format2_t audio_softc_get_hw_format(struct audio_softc *sc, int mode);
void* audio_softc_allocm(struct audio_softc *sc, int n);
void audio_softc_play_start(struct audio_softc *sc);

