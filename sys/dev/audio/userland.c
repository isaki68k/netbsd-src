#include "compat.h"
#include "userland.h"
#include "audiovar.h"
#include "aumix.h"
#include <errno.h>

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

static const audio_format2_t audio_default = {
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
	sc->sc_rparams = audio_default;

	audio_mixer_init(sc, sc->sc_pmixer, AUMODE_PLAY);
	audio_mixer_init(sc, sc->sc_rmixer, AUMODE_RECORD);
}

/*
 * ***** audio_file *****
 */
int//ssize_t
sys_write(audio_file_t *file, void* buf, size_t len)
{
	KASSERT(buf);

	if (len > INT_MAX) {
		errno = EINVAL;
		return -1;
	}

	struct uio uio = buf_to_uio(buf, len, UIO_READ);

	mutex_enter(file->sc->sc_lock);
	int error = audio_write(file->sc, &uio, 0, file);
	mutex_exit(file->sc->sc_lock);
	if (error) {
		errno = error;
		return -1;
	}
	return (int)len;
}

audio_file_t *
sys_open(struct audio_softc *sc, int mode)
{
	audio_file_t *file;

	file = calloc(1, sizeof(audio_file_t));
	file->sc = sc;

	if (mode == AUMODE_PLAY) {
		audio_track_init(sc, &file->ptrack, AUMODE_PLAY);
	} else {
		audio_track_init(sc, &file->rtrack, AUMODE_RECORD);
	}

	SLIST_INSERT_HEAD(&sc->sc_files, file, entry);

	return file;
}

// ioctl(AUDIO_DRAIN) 相当
int
sys_ioctl_drain(audio_track_t *track)
{
	// 割り込みエミュレートしているときはメインループに制御を戻さないといけない
	audio_trackmixer_t *mixer = track->mixer;
	struct audio_softc *sc = mixer->sc;

	mutex_enter(sc->sc_lock);
	audio_track_drain(track);
	mutex_exit(sc->sc_lock);

	return 0;
}

void
audio_print_format2(const char *s, const audio_format2_t *fmt)
{
	char fmtstr[64];

	audio_format2_tostr(fmtstr, sizeof(fmtstr), fmt);
	printf("%s %s\n", s, fmtstr);
}

void
audio_format2_tostr(char *buf, size_t bufsize, const audio_format2_t *fmt)
{
	int n;

	n = 0;
	n += snprintf(buf + n, bufsize - n, "enc=%d", fmt->encoding);

	if (fmt->precision == fmt->stride) {
		n += snprintf(buf + n, bufsize - n, " %dbit", fmt->precision);
	} else {
		n += snprintf(buf + n, bufsize - n, " %d/%dbit",
			fmt->precision, fmt->stride);
	}

	snprintf(buf + n, bufsize - n, " %uch %uHz",
	    fmt->channels, fmt->sample_rate);
}
