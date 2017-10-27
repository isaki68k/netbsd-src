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
inline static uint32_t
bidi_lsl(uint32_t a, int shift)
{
	if (shift > 0) return a << shift;
	if (shift < 0) return a >> -shift;
	return a;
}


void audio_lane_init(audio_lane_t *lane, audio_lanemixer_t *mixer);
void audio_lane_set_format(audio_lane_t *lane, audio_format_t *lane_fmt);
void audio_lane_play(audio_lane_t *lane);
void audio_lane_play_drain(audio_lane_t *lane);

void audio_mixer_init(audio_lanemixer_t *mixer, audio_softc_t *sc, int mode);
void audio_mixer_play(audio_lanemixer_t *);
void audio_mixer_play_period(audio_lanemixer_t *mixer);
void audio_mixer_play_mix_lane(audio_lanemixer_t *mixer, audio_lane_t *lane);
void audio_lanemixer_intr(audio_lanemixer_t *mixer, int count);

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

