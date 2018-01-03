#include "compat.h"
#include "userland.h"
#include "audiovar.h"
#include "aumix.h"

// この file が再生可能なら true を返します。
bool
audio_file_can_playback(const audio_file_t *file)
{
	return ((file->mode & AUMODE_PLAY) != 0);
}

// この file が録音可能なら true を返します。
bool
audio_file_can_record(const audio_file_t *file)
{
	return ((file->mode & AUMODE_RECORD) != 0);
}

// この track が再生トラックなら true を返します。
bool
audio_track_is_playback(const audio_track_t *track)
{
	return ((track->mode & AUMODE_PLAY) != 0);
}

// この track が録音トラックなら true を返します。
bool
audio_track_is_record(const audio_track_t *track)
{
	return ((track->mode & AUMODE_RECORD) != 0);
}

static const struct audio_format2 audio_default = {
	.sample_rate = 8000,
	.encoding = AUDIO_ENCODING_ULAW,
	.precision = 8,
	.stride = 8,
	.channels = 1,
};

void
audio_softc_init(struct audio_softc *sc)
{
	sc->sc_pmixer = malloc(sizeof(audio_trackmixer_t));
	sc->sc_rmixer = malloc(sizeof(audio_trackmixer_t));
	sc->sc_lock = &sc->sc_lock0;
	sc->sc_intr_lock = &sc->sc_intr_lock0;
	sc->hw_if = &sc->hw_if0;
	sc->sc_pparams = audio_default;
	sc->sc_pparams = audio_default;

	audio_mixer_init(sc, sc->sc_pmixer, AUMODE_PLAY);
	audio_mixer_init(sc, sc->sc_rmixer, AUMODE_RECORD);
}
