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


int audio_track_init(struct audio_softc *sc, audio_track_t **track, int mode);
void audio_track_destroy(audio_track_t *track);
int audio_track_set_format(audio_track_t *track, audio_format2_t *track_fmt);
void audio_track_play(audio_track_t *track, bool isdrain);
int audio_track_drain(audio_track_t *track);
void audio_track_record(audio_track_t *track);

int audio_mixer_init(struct audio_softc *sc, audio_trackmixer_t *mixer, int mode);
void audio_mixer_destroy(struct audio_softc *sc, audio_trackmixer_t *mixer);
bool audio_pmixer_start(struct audio_softc *sc, bool force);
void audio_pmixer_process(struct audio_softc *sc, bool isintr);
int  audio_pmixer_mix_track(audio_trackmixer_t *mixer, audio_track_t *track, int req, int mixed);
bool audio_rmixer_start(struct audio_softc *sc);
void audio_rmixer_process(struct audio_softc *sc);
void audio_pintr(void *arg);
void audio_rintr(void *arg);

int  audio2_halt_output(struct audio_softc *sc);
int  audio2_halt_input(struct audio_softc *sc);

/* glue layer */
int audio_write(struct audio_softc *sc, struct uio *uio, int ioflag, audio_file_t *file); /* write の MI 側 */
int audio_read(struct audio_softc *sc, struct uio *uio, int ioflag,
	audio_file_t *file);
