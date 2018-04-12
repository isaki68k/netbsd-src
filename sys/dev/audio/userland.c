#include "compat.h"
#include "userland.h"
#include "audiovar.h"
#include <errno.h>

extern const struct fileops audio_fileops;

struct proc curproc0;
struct proc *curproc = &curproc0;;
struct lwp curlwp0;
struct lwp *curlwp = &curlwp0;
kmutex_t proc_lock0;
kmutex_t *proc_lock = &proc_lock0;

int	fnullop_fcntl(struct file *file, u_int name, void *val)
{
	return 0;
}
void	fnullop_restart(struct file *file)
{
}

static const audio_format2_t audio_default = {
	.sample_rate = 8000,
	.encoding = AUDIO_ENCODING_ULAW,
	.precision = 8,
	.stride = 8,
	.channels = 1,
};

void
audio_softc_init(struct audio_softc *sc, const audio_format2_t *phwfmt,
	const audio_format2_t *rhwfmt)
{
	sc->sc_pmixer = malloc(sizeof(audio_trackmixer_t));
	sc->sc_rmixer = malloc(sizeof(audio_trackmixer_t));
	sc->sc_pparams = audio_default;
	sc->sc_rparams = audio_default;

	audio_mixer_init(sc, AUMODE_PLAY, phwfmt);
	audio_mixer_init(sc, AUMODE_RECORD, rhwfmt);
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

	struct file fp;
	memset(&fp, 0, sizeof(fp));
	fp.f_audioctx = file;

	struct uio uio = buf_to_uio(buf, len, UIO_READ);

	mutex_enter(file->sc->sc_lock);
	int error = audio_fileops.fo_write(&fp, NULL, &uio, NULL, 0);
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
	audio_track_drain(sc, track);
	mutex_exit(sc->sc_lock);

	return 0;
}
