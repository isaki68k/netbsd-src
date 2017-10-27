#include "aumix.h"
#include "auring.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/audioio.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <pthread.h>

struct audio_dev_netbsd
{
	int fd;
	int frame_bytes;
	audio_format_t fmt;
	int sent_count;
	pthread_mutex_t mutex;
};
typedef struct audio_dev_netbsd audio_dev_netbsd_t;

void
lock(audio_softc_t *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;
	pthread_mutex_lock(&dev->mutex);
}

void
unlock(audio_softc_t *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;
	pthread_mutex_unlock(&dev->mutex);
}

void
audio_attach(audio_softc_t **softc)
{
	audio_softc_t *sc;
	audio_dev_netbsd_t *dev;
	struct audio_info ai;
	int r;

	sc = calloc(1, sizeof(*sc));
	*softc = sc;
	sc->phys = calloc(1, sizeof(*dev));

	dev = sc->phys;
	dev->fd = open("/dev/sound", O_RDWR);

	dev->fmt.encoding = AUDIO_ENCODING_SLINEAR_LE;
	dev->fmt.channels = 2;
	dev->fmt.frequency = 48000;
	dev->fmt.precision = 16;
	dev->fmt.stride = 16;

	AUDIO_INITINFO(&ai);
	ai.mode = AUMODE_PLAY;
	ai.play.sample_rate = dev->fmt.frequency;
	ai.play.encoding    = dev->fmt.encoding;
	ai.play.precision   = dev->fmt.precision;
	ai.play.channels    = dev->fmt.channels;
	r = ioctl(dev->fd, AUDIO_SETINFO, &ai);
	if (r == -1) {
		printf("AUDIO_SETINFO failed\n");
		exit(1);
	}
	dev->frame_bytes = ai.play.precision / 8 * ai.play.channels;

	pthread_mutex_init(&dev->mutex, NULL);

	audio_mixer_init(&sc->mixer_play, sc, AUDIO_PLAY);
	audio_mixer_init(&sc->mixer_rec, sc, AUDIO_REC);
}

void
audio_detach(audio_softc_t *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;

	close(dev->fd);
	free(dev);
	sc->phys = NULL;
}

void
audio_softc_play_start(audio_softc_t *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;
	audio_lanemixer_t *mixer = &sc->mixer_play;

	if (mixer->hw_buf.count <= 0) return;

	lock(sc);

	int count;
	for (int loop = 0; loop < 2; loop++) {
		count = audio_ring_unround_count(&mixer->hw_buf);

		int16_t *src = RING_TOP(int16_t, &mixer->hw_buf);
		
		int bytelen = count * dev->frame_bytes;
		int r = write(dev->fd, src, bytelen);
		if (r == -1) {
			printf("write failed: %s\n", strerror(errno));
			exit(1);
		}
		dev->sent_count += count;

		audio_ring_tookfromtop(&mixer->hw_buf, count);
	}

	unlock(sc);
}

bool
audio_softc_play_busy(audio_softc_t *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;

	if (dev->sent_count > 0) {
		audio_lanemixer_intr(&sc->mixer_play, dev->sent_count);
		dev->sent_count = 0;
	}
	return false;
}

int
audio_softc_get_hw_capacity(audio_softc_t *sc)
{
	return 65536;	/* XXX */
}

void *
audio_softc_allocm(audio_softc_t *sc, int n)
{
	return malloc(n);
}

audio_format_t
audio_softc_get_hw_format(audio_softc_t *sc, int mode)
{
	audio_dev_netbsd_t *dev = sc->phys;
	return dev->fmt;
}

void
WAIT()
{
	usleep(1);
}
