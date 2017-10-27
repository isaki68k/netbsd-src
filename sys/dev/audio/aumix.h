#pragma once

#include <stdint.h>
#include "compat.h"
#include "queue.h"
#include <stdbool.h>
#include "audiovar.h"

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


void audio_track_init(audio_track_t *track, audio_trackmixer_t *mixer);
void audio_track_set_format(audio_track_t *track, audio_format_t *track_fmt);
void audio_track_play(audio_track_t *track);
void audio_track_play_drain(audio_track_t *track);

void audio_mixer_init(audio_trackmixer_t *mixer, audio_softc_t *sc, int mode);
void audio_mixer_play(audio_trackmixer_t *);
void audio_mixer_play_period(audio_trackmixer_t *mixer);
void audio_mixer_play_mix_track(audio_trackmixer_t *mixer, audio_track_t *track);
void audio_trackmixer_intr(audio_trackmixer_t *mixer, int count);

/* glue layer */
int audio_write(audio_softc_t *sc, struct uio *uio, int ioflag, audio_file_t *file); /* write の MI 側 */

/* system call emulation */
audio_file_t *sys_open(audio_softc_t *sc, int mode);
int/*ssize_t*/ sys_write(audio_file_t *file, void* buf, size_t len);	/* write syscall の Userland 側エミュレート */

/* XXX: 分類未定 */
int audio_softc_get_hw_capacity(audio_softc_t *sc);
audio_format_t audio_softc_get_hw_format(audio_softc_t *sc, int mode);
void* audio_softc_allocm(audio_softc_t *sc, int n);
void audio_softc_play_start(audio_softc_t *sc);

